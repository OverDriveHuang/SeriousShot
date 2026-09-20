#include "adapters/shared/libultrahdr_encoder.hpp"
#include "core/image_software_metadata.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "domain/color/display_p3_icc_profile.hpp"

#include <ultrahdr_api.h>

#include <algorithm>
#include <array>
#include <csetjmp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <jpeglib.h>
}

namespace hdrshot {
namespace {

using EncoderHandle = std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_encoder)>;

Error codec_error(
    const char* stage,
    const uhdr_error_info_t* native = nullptr,
    std::map<std::string, std::string> context = {}) {
  context["stage"] = stage;
  if (native != nullptr) {
    context["nativeCode"] = std::to_string(static_cast<int>(native->error_code));
    if (native->has_detail != 0) context["nativeDetail"] = native->detail;
  }
  return Error{ErrorCode::ultra_hdr_encoding_failed, "LibUltraHdrEncoder",
               Retryability::never, std::move(context)};
}

bool succeeded(const uhdr_error_info_t& status) {
  return status.error_code == UHDR_CODEC_OK;
}

struct JpegError {
  jpeg_error_mgr manager{};
  std::jmp_buf jump;
  char message[JMSG_LENGTH_MAX]{};
};

void jpeg_failure(j_common_ptr codec) {
  auto* error = reinterpret_cast<JpegError*>(codec->err);
  codec->err->format_message(codec, error->message);
  std::longjmp(error->jump, 1);
}

struct JpegState {
  jpeg_compress_struct codec{};
  JpegError error{};
  unsigned char* bytes{};
  unsigned long byte_count{};
  ~JpegState() {
    if (codec.mem != nullptr) jpeg_destroy_compress(&codec);
    std::free(bytes);
  }
};

// All nonnegative binary16 inputs through 1.0. A ~15 KB immutable table avoids
// repeating pow() for every channel; calculations use the shared FP32 OETF.
const auto& sdr_srgb_codes() {
  static const auto codes = [] {
    std::array<std::uint8_t, 0x3C01U> result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
      const auto linear = ExtendedP3Mapper::decode_binary16(
          static_cast<std::uint16_t>(i)).value();
      result[i] = static_cast<std::uint8_t>(std::lround(
          255.0F * ExtendedP3Mapper::encode_extended_srgb(linear)));
    }
    return result;
  }();
  return codes;
}

Result<EncodedUltraHdrJpeg, Error> encode_sdr_jpeg(
    const LinearDisplayP3HalfImage& source, UltraHdrJpegQuality quality,
    std::string_view software) {
  const auto icc = DisplayP3IccProfile::bytes();
  if (icc.empty() || icc.size() > std::numeric_limits<unsigned int>::max()) {
    return Result<EncodedUltraHdrJpeg, Error>::failure(codec_error("sdr_icc"));
  }
  const auto width = static_cast<std::size_t>(source.size_px.width);
  std::vector<JSAMPLE> row(width * 3U);
  const auto exif = software.empty() ? std::vector<std::uint8_t>{}
                                     : jpeg_software_exif(software);
  const auto& codes = sdr_srgb_codes();
  const bool allow_edge_clip = source.source_visible_maximum_linear_component.has_value();
  const auto state = std::make_unique<JpegState>();
  state->codec.err = jpeg_std_error(&state->error.manager);
  state->error.manager.error_exit = jpeg_failure;
  // All RAII owners precede setjmp; mutable libjpeg state lives on the heap.
  // No C++ object with a destructor may be created across a fallible JPEG call.
  if (setjmp(state->error.jump) != 0) {
    return Result<EncodedUltraHdrJpeg, Error>::failure(codec_error(
        "sdr_jpeg", nullptr, {{"nativeDetail", state->error.message}}));
  }
  jpeg_create_compress(&state->codec);
  jpeg_mem_dest(&state->codec, &state->bytes, &state->byte_count);
  state->codec.image_width = static_cast<JDIMENSION>(source.size_px.width);
  state->codec.image_height = static_cast<JDIMENSION>(source.size_px.height);
  state->codec.input_components = 3;
  state->codec.in_color_space = JCS_RGB;
  jpeg_set_defaults(&state->codec);
  // Standard JPEG RGB->YCbCr coefficients, independent of P3 primaries.
  // 4:4:4 retains colored text/edges and matches the current UHDR base layout.
  for (int c = 0; c < 3; ++c) {
    state->codec.comp_info[c].h_samp_factor = 1;
    state->codec.comp_info[c].v_samp_factor = 1;
  }
  jpeg_set_quality(&state->codec, ultra_hdr_jpeg_quality_value(quality), TRUE);
  state->codec.dct_method = JDCT_ISLOW;
  jpeg_start_compress(&state->codec, TRUE);
  if (!exif.empty()) {
    jpeg_write_marker(&state->codec, JPEG_APP0 + 1, exif.data(),
                      static_cast<unsigned int>(exif.size()));
  }
  jpeg_write_icc_profile(&state->codec, icc.data(), static_cast<unsigned int>(icc.size()));
  while (state->codec.next_scanline < state->codec.image_height) {
    const auto start = static_cast<std::size_t>(state->codec.next_scanline) * width * 4U;
    for (std::size_t x = 0; x < width; ++x) {
      for (std::size_t c = 0; c < 3; ++c) {
        auto bits = source.rgba_half[start + x * 4U + c];
        // Explicit AA policy: positive finite residual edge HDR may be clipped
        // when a renderer supplied the source-passthrough classification.
        if (allow_edge_clip && bits > 0x3C00U && bits < 0x7C00U) bits = 0x3C00U;
        // Accept signed zero; reject negative, >1, NaN or infinity instead of
        // silently clipping a renderer contract violation into SDR.
        if (bits > 0x3C00U && bits != 0x8000U) {
          return Result<EncodedUltraHdrJpeg, Error>::failure(
              codec_error("sdr_sample_out_of_range"));
        }
        row[x * 3U + c] = codes[bits == 0x8000U ? 0U : bits];
      }
    }
    JSAMPROW row_pointer = row.data();
    if (jpeg_write_scanlines(&state->codec, &row_pointer, 1) != 1) {
      return Result<EncodedUltraHdrJpeg, Error>::failure(codec_error("sdr_scanline"));
    }
  }
  jpeg_finish_compress(&state->codec);
  return Result<EncodedUltraHdrJpeg, Error>::success(EncodedUltraHdrJpeg{
      std::vector<std::uint8_t>(state->bytes, state->bytes + state->byte_count),
      JpegOutputKind::display_p3_sdr});
}

}  // namespace

Result<EncodedUltraHdrJpeg, Error> LibUltraHdrEncoder::encode(
    const UltraHdrEncodeRequest& request) {
  if (request.linear_display_p3 == nullptr ||
      !valid_ultra_hdr_jpeg_quality(request.quality) ||
      !valid_software_metadata(request.software)) {
    return Result<EncodedUltraHdrJpeg, Error>::failure(
        codec_error("validate_request"));
  }
  const auto& source = *request.linear_display_p3;
  if (source.size_px.width <= 0 || source.size_px.height <= 0 ||
      source.size_px.width > kUltraHdrMaximumDimension ||
      source.size_px.height > kUltraHdrMaximumDimension ||
      source.reference_white_nits != kUltraHdrReferenceWhiteNits ||
      !std::isfinite(source.maximum_linear_component) ||
      source.maximum_linear_component < 0.0 ||
      (source.source_visible_maximum_linear_component &&
       (!std::isfinite(*source.source_visible_maximum_linear_component) ||
        *source.source_visible_maximum_linear_component < 0.0 ||
        *source.source_visible_maximum_linear_component > source.maximum_linear_component))) {
    return Result<EncodedUltraHdrJpeg, Error>::failure(
        codec_error("validate_source_contract"));
  }
  const auto width = static_cast<std::size_t>(source.size_px.width);
  const auto height = static_cast<std::size_t>(source.size_px.height);
  if (width > std::numeric_limits<std::size_t>::max() / height / 4U ||
      source.rgba_half.size() != width * height * 4U) {
    return Result<EncodedUltraHdrJpeg, Error>::failure(
        codec_error("validate_source_shape"));
  }

  if (jpeg_output_kind(source) == JpegOutputKind::display_p3_sdr) {
    return encode_sdr_jpeg(source, request.quality, request.software);
  }

  EncoderHandle encoder{uhdr_create_encoder(), &uhdr_release_encoder};
  if (!encoder) {
    return Result<EncodedUltraHdrJpeg, Error>::failure(
        codec_error("create_encoder"));
  }
  uhdr_raw_image_t raw{};
  raw.fmt = UHDR_IMG_FMT_64bppRGBAHalfFloat;
  raw.cg = UHDR_CG_DISPLAY_P3;
  raw.ct = UHDR_CT_LINEAR;
  raw.range = UHDR_CR_FULL_RANGE;
  raw.w = static_cast<unsigned int>(source.size_px.width);
  raw.h = static_cast<unsigned int>(source.size_px.height);
  raw.planes[UHDR_PLANE_PACKED] = const_cast<std::uint16_t*>(
      source.rgba_half.data());
  raw.stride[UHDR_PLANE_PACKED] = raw.w;

  auto status = uhdr_enc_set_raw_image(encoder.get(), &raw, UHDR_HDR_IMG);
  if (!succeeded(status)) {
    return Result<EncodedUltraHdrJpeg, Error>::failure(
        codec_error("set_hdr_image", &status));
  }
  // Let libultrahdr write EXIF while packaging the JPEG, so MPF offsets stay
  // correct. Never insert a new APP1 into the completed gain-map container.
  auto exif = request.software.empty() ? std::vector<std::uint8_t>{}
                                      : jpeg_software_exif(request.software);
  if (!exif.empty()) {
    uhdr_mem_block_t block{exif.data(), exif.size(), exif.size()};
    status = uhdr_enc_set_exif_data(encoder.get(), &block);
    if (!succeeded(status)) {
      return Result<EncodedUltraHdrJpeg, Error>::failure(codec_error("set_exif", &status));
    }
  }
  const auto quality = ultra_hdr_jpeg_quality_value(request.quality);
  status = uhdr_enc_set_quality(encoder.get(), quality, UHDR_BASE_IMG);
  if (!succeeded(status)) {
    return Result<EncodedUltraHdrJpeg, Error>::failure(
        codec_error("set_base_quality", &status));
  }
  status = uhdr_enc_set_quality(encoder.get(), quality, UHDR_GAIN_MAP_IMG);
  if (!succeeded(status)) {
    return Result<EncodedUltraHdrJpeg, Error>::failure(
        codec_error("set_gain_map_quality", &status));
  }
  status = uhdr_enc_set_using_multi_channel_gainmap(encoder.get(), 1);
  if (!succeeded(status)) {
    return Result<EncodedUltraHdrJpeg, Error>::failure(
        codec_error("set_rgb_gain_map", &status));
  }
  status = uhdr_enc_set_gainmap_scale_factor(encoder.get(), 1);
  if (!succeeded(status)) {
    return Result<EncodedUltraHdrJpeg, Error>::failure(
        codec_error("set_gain_map_scale", &status));
  }
  // Shared CMake patch removes API-0's unconditional realtime override.
  // XMP-enabled upstream two-pass merges RGB ranges BEFORE quantization;
  // RGB samples stay independent and ISO/XMP share the same parameters.
  // Tone mapping remains upstream default; a future custom SDR intent belongs
  // inside this adapter/API-1, not the OS shell or application workflow.
  status = uhdr_enc_set_preset(encoder.get(), UHDR_USAGE_BEST_QUALITY);
  if (!succeeded(status)) {
    return Result<EncodedUltraHdrJpeg, Error>::failure(
        codec_error("set_two_pass_preset", &status));
  }
  status = uhdr_enc_set_output_format(encoder.get(), UHDR_CODEC_JPG);
  if (!succeeded(status)) {
    return Result<EncodedUltraHdrJpeg, Error>::failure(
        codec_error("set_jpeg_output", &status));
  }
  const auto target_peak_nits = static_cast<float>(std::clamp(
      source.maximum_linear_component * source.reference_white_nits,
      kUltraHdrReferenceWhiteNits, 10000.0));
  status = uhdr_enc_set_target_display_peak_brightness(
      encoder.get(), target_peak_nits);
  if (!succeeded(status)) {
    return Result<EncodedUltraHdrJpeg, Error>::failure(
        codec_error("set_target_peak", &status));
  }
  status = uhdr_encode(encoder.get());
  if (!succeeded(status)) {
    return Result<EncodedUltraHdrJpeg, Error>::failure(
        codec_error("encode", &status));
  }
  const uhdr_compressed_image_t* stream = uhdr_get_encoded_stream(encoder.get());
  if (stream == nullptr || stream->data == nullptr || stream->data_sz < 4U) {
    return Result<EncodedUltraHdrJpeg, Error>::failure(
        codec_error("get_encoded_stream"));
  }
  const auto* first = static_cast<const std::uint8_t*>(stream->data);
  std::vector<std::uint8_t> bytes(first, first + stream->data_sz);
  if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return Result<EncodedUltraHdrJpeg, Error>::failure(
        codec_error("validate_encoded_size"));
  }
  if (bytes[0] != 0xFFU || bytes[1] != 0xD8U ||
      bytes[bytes.size() - 2U] != 0xFFU || bytes.back() != 0xD9U ||
      is_uhdr_image(static_cast<void*>(bytes.data()),
                    static_cast<int>(bytes.size())) != 1) {
    return Result<EncodedUltraHdrJpeg, Error>::failure(
        codec_error("validate_encoded_stream"));
  }
  return Result<EncodedUltraHdrJpeg, Error>::success(
      EncodedUltraHdrJpeg{std::move(bytes)});
}

}  // namespace hdrshot
