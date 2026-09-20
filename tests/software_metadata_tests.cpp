#include "adapters/shared/libultrahdr_encoder.hpp"
#include "core/build_metadata.hpp"
#include "core/image_software_metadata.hpp"
#include "domain/color/display_p3_icc_profile.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "domain/png/streaming_png_encoder.hpp"
#include "software_metadata_test_support.hpp"
#include "ultra_hdr_metadata_test_support.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>

namespace {
using namespace hdrshot;
std::filesystem::path fixture_directory;

void write_fixture(std::string_view name, const std::vector<std::uint8_t>& bytes) {
  if (fixture_directory.empty()) return;
  std::ofstream output(fixture_directory / name, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  output.close();
  HDRSHOT_CHECK(output.good());
}

void compiled_identity_and_exif_layout() {
  HDRSHOT_CHECK(std::regex_match(std::string(build_timestamp()),
      std::regex(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})")));
  HDRSHOT_CHECK(software_identifier() == "SeriousShot; build " + std::string(build_timestamp()));
  HDRSHOT_CHECK(valid_software_metadata(software_identifier()));
  for (const auto value : {std::string_view{"A"}, std::string_view{"123"},
                          std::string_view{"1234"}, software_identifier()}) {
    HDRSHOT_CHECK(test::read_exif_software(jpeg_software_exif(value)) == value);
  }
}

void both_png_encoders_preserve_pixels_and_color_chunks() {
  for (const bool hdr : {false, true}) {
    const PngColorMetadata color = hdr
        ? PngColorMetadata{kDisplayP3PqFullRange, std::nullopt,
                           ContentLightLevelInfo{10000000U, 2030000U}}
        : PngColorMetadata{kDisplayP3SrgbFullRange,
            IccProfilePayload{"Display P3", DisplayP3IccProfile::bytes()}};
    const std::vector<std::uint16_t> samples{1, 8000, 65535, 30000, 50000, 60000};
    EncodePng16Request legacy{2, 1, PngColorType::rgb, samples, color};
    EncodeStreamingPng16Request streaming{2, 1, color,
        [&](std::uint32_t) { return Result<std::vector<std::uint16_t>, Error>::success(samples); }};
    const auto baseline = PngEncoder::encode_16bit(legacy);
    const auto baseline_stream = StreamingPngEncoder::encode_16bit_rgb(streaming);
    legacy.software = streaming.software = software_identifier();
    const auto encoded = PngEncoder::encode_16bit(legacy);
    const auto encoded_stream = StreamingPngEncoder::encode_16bit_rgb(streaming);
    HDRSHOT_CHECK(baseline && baseline_stream && encoded && encoded_stream);
    write_fixture(hdr ? "hdr.png" : "sdr.png", encoded_stream.value().bytes);
    for (const auto pair : {std::pair{&baseline.value(), &encoded.value()},
                            std::pair{&baseline_stream.value(), &encoded_stream.value()}}) {
      const auto parsed = test::read_png_software(pair.second->bytes);
      HDRSHOT_CHECK(parsed.software == software_identifier());
      // Exact IDAT + ICC/cICP/cLLI preservation, including their CRCs.
      HDRSHOT_CHECK(parsed.without_software == pair.first->bytes);
      HDRSHOT_CHECK(test::read_png_software(pair.first->bytes).software.empty());
      HDRSHOT_CHECK(pair.second->bytes.size() - pair.first->bytes.size() ==
                    21U + software_identifier().size());
    }
  }
}

LinearDisplayP3HalfImage jpeg_fixture(bool hdr) {
  LinearDisplayP3HalfImage image;
  image.size_px = {32, 16};
  image.maximum_linear_component = hdr ? 4.0 : 1.0;
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 32; ++x) {
      const float v = float(image.maximum_linear_component) * float(x) / 31.0F;
      for (const float c : {v, v * 0.5F, v * 0.25F, 1.0F})
        image.rgba_half.push_back(ExtendedP3Mapper::encode_binary16(c));
    }
  }
  return image;
}

std::vector<std::uint16_t> decode_hdr(const EncodedUltraHdrJpeg& jpeg) {
  const std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)> decoder{
      uhdr_create_decoder(), &uhdr_release_decoder};
  HDRSHOT_CHECK(decoder != nullptr);
  uhdr_compressed_image_t input{};
  input.data = const_cast<std::uint8_t*>(jpeg.bytes.data());
  input.data_sz = input.capacity = jpeg.bytes.size();
  HDRSHOT_CHECK(uhdr_dec_set_image(decoder.get(), &input).error_code == UHDR_CODEC_OK);
  HDRSHOT_CHECK(uhdr_dec_set_out_img_format(decoder.get(),
      UHDR_IMG_FMT_64bppRGBAHalfFloat).error_code == UHDR_CODEC_OK);
  HDRSHOT_CHECK(uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR).error_code == UHDR_CODEC_OK);
  HDRSHOT_CHECK(uhdr_dec_set_out_max_display_boost(decoder.get(), 4.0F).error_code == UHDR_CODEC_OK);
  HDRSHOT_CHECK(uhdr_dec_probe(decoder.get()).error_code == UHDR_CODEC_OK);
  test::check_dual_metadata(decoder.get());
  test::check_p3_base_and_alternate(decoder.get());
  HDRSHOT_CHECK(uhdr_decode(decoder.get()).error_code == UHDR_CODEC_OK);
  const auto* raw = uhdr_get_decoded_image(decoder.get());
  HDRSHOT_CHECK(raw && raw->w == 32 && raw->h == 16 && raw->cg == UHDR_CG_DISPLAY_P3);
  const auto* data = static_cast<const std::uint16_t*>(raw->planes[UHDR_PLANE_PACKED]);
  std::vector<std::uint16_t> pixels;
  for (unsigned int y = 0; y < raw->h; ++y) {
    const auto* row = data + y * raw->stride[UHDR_PLANE_PACKED] * 4U;
    pixels.insert(pixels.end(), row, row + raw->w * 4U);
  }
  return pixels;
}

void jpeg_sdr_and_hdr_preserve_encoded_images() {
  LibUltraHdrEncoder encoder;
  for (const bool hdr : {false, true}) {
    const auto image = jpeg_fixture(hdr);
    const auto baseline = encoder.encode({&image, UltraHdrJpegQuality::balanced});
    const auto encoded = encoder.encode({&image, UltraHdrJpegQuality::balanced, software_identifier()});
    HDRSHOT_CHECK(baseline && encoded);
    write_fixture(hdr ? "hdr.jpg" : "sdr.jpg", encoded.value().bytes);
    const auto parsed = test::read_jpeg_software(encoded.value().bytes);
    const auto original = test::read_jpeg_software(baseline.value().bytes);
    HDRSHOT_CHECK(parsed.software == software_identifier() && original.software.empty());
    // Base/gain JPEG entropy streams, ICCs and ISO metadata are byte-identical.
    HDRSHOT_CHECK(parsed.without_software == original.without_software);
    HDRSHOT_CHECK(encoded.value().bytes.size() - baseline.value().bytes.size() ==
        37U + software_identifier().size());
    if (hdr) {
      // Probe + actual reconstruction validates the changed MPF offsets too.
      HDRSHOT_CHECK(decode_hdr(encoded.value()) == decode_hdr(baseline.value()));
    } else {
      HDRSHOT_CHECK(encoded.value().kind == JpegOutputKind::display_p3_sdr);
      HDRSHOT_CHECK(is_uhdr_image(const_cast<std::uint8_t*>(encoded.value().bytes.data()),
          static_cast<int>(encoded.value().bytes.size())) == 0);
    }
  }
}

void invalid_metadata_is_rejected_before_writing() {
  const std::array<std::string, 4> invalid{
      std::string{"bad\0value", 9}, "bad\nvalue", std::string(1025, 'x'), "bad\xFF"};
  LibUltraHdrEncoder encoder;
  auto image = jpeg_fixture(true);
  for (const auto& software : invalid) {
    HDRSHOT_CHECK(!valid_software_metadata(software));
    EncodePng16Request legacy{};
    legacy.software = software;
    HDRSHOT_CHECK(PngEncoder::encode_16bit(legacy).error().code == ErrorCode::metadata_conflict);
    EncodeStreamingPng16Request streaming{};
    streaming.software = software;
    HDRSHOT_CHECK(StreamingPngEncoder::encode_16bit_rgb(streaming).error().code == ErrorCode::metadata_conflict);
    HDRSHOT_CHECK(!encoder.encode({&image, UltraHdrJpegQuality::balanced, software}));
  }
}
}  // namespace

int main(int argc, char* argv[]) {
  // Optional offline artifacts for an independent reader (e.g. ExifTool).
  if (argc == 3 && std::string_view(argv[1]) == "--fixtures-dir") {
    fixture_directory = argv[2];
    if (!std::filesystem::is_directory(fixture_directory)) return 2;
  } else if (argc != 1) {
    return 2;
  }
  return hdrshot::test::run({
      {"shared build identity and EXIF IFD layout", compiled_identity_and_exif_layout},
      {"PNG SDR/HDR Software with exact pixel/colour preservation", both_png_encoders_preserve_pixels_and_color_chunks},
      {"JPEG SDR/HDR EXIF with exact pixel/gain preservation", jpeg_sdr_and_hdr_preserve_encoded_images},
      {"invalid Software rejected before encoding", invalid_metadata_is_rejected_before_writing},
  });
}
