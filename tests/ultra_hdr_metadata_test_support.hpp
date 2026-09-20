#pragma once

#include "test_support.hpp"
#include <ultrahdr_api.h>
#include <ultrahdr/jpegdecoderhelper.h>
#include <ultrahdr/gainmapmetadata.h>
#include <ultrahdr/jpegrutils.h>
#include <algorithm>
#include <cmath>
#include <charconv>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace hdrshot::test {

struct DualGainMetadata {
  ultrahdr::uhdr_gainmap_metadata_ext_t iso{}, xmp{};
};

inline DualGainMetadata check_dual_metadata(uhdr_codec_private_t* decoder) {
  for (const auto* jpeg : {uhdr_dec_get_base_image(decoder),
                           uhdr_dec_get_gainmap_image(decoder)}) {
    HDRSHOT_CHECK(jpeg != nullptr);
    ultrahdr::JpegDecoderHelper parsed;
    HDRSHOT_CHECK(parsed.parseImage(jpeg->data, jpeg->data_sz).error_code == UHDR_CODEC_OK);
    HDRSHOT_CHECK(parsed.getXMPSize() > 0);
    HDRSHOT_CHECK(parsed.getIsoMetadataSize() > 0);
    if (jpeg == uhdr_dec_get_base_image(decoder)) {
      const std::string_view xmp(static_cast<const char*>(parsed.getXMPPtr()), parsed.getXMPSize());
      constexpr std::string_view key = "Item:Length=\"";
      const auto at = xmp.find(key);
      HDRSHOT_CHECK(at != std::string_view::npos);
      const auto start = at + key.size(), end = xmp.find('"', start);
      HDRSHOT_CHECK(end != std::string_view::npos);
      std::size_t length = 0;
      const auto converted = std::from_chars(xmp.data() + start, xmp.data() + end, length);
      HDRSHOT_CHECK(converted.ec == std::errc{} && converted.ptr == xmp.data() + end);
      HDRSHOT_CHECK(length == uhdr_dec_get_gainmap_image(decoder)->data_sz);
    }
  }
  const auto* jpeg = uhdr_dec_get_gainmap_image(decoder);
  ultrahdr::JpegDecoderHelper parsed;
  HDRSHOT_CHECK(parsed.parseImage(jpeg->data, jpeg->data_sz).error_code == UHDR_CODEC_OK);
  // Namespace is NUL-terminated in APP2. Decode the ISO fractions explicitly;
  // the normal decoder prefers ISO and would otherwise never test XMP.
  const auto* iso = static_cast<const std::uint8_t*>(parsed.getIsoMetadataPtr());
  const auto size = parsed.getIsoMetadataSize();
  const auto prefix_size = std::string_view("urn:iso:std:iso:ts:21496:-1").size() + 1;
  HDRSHOT_CHECK(size > prefix_size && iso[prefix_size - 1] == 0);
  HDRSHOT_CHECK(std::string_view(reinterpret_cast<const char*>(iso), prefix_size - 1) ==
      "urn:iso:std:iso:ts:21496:-1");
  const std::vector<std::uint8_t> data(iso + prefix_size, iso + size);
  ultrahdr::uhdr_gainmap_metadata_frac fraction{};
  DualGainMetadata result;
  HDRSHOT_CHECK(ultrahdr::uhdr_gainmap_metadata_frac::decodeGainmapMetadata(
      data, &fraction).error_code == UHDR_CODEC_OK);
  HDRSHOT_CHECK(!fraction.backwardDirection && fraction.useBaseColorSpace);
  HDRSHOT_CHECK(fraction.allChannelsIdentical());
  HDRSHOT_CHECK(ultrahdr::uhdr_gainmap_metadata_frac::gainmapMetadataFractionToFloat(
      &fraction, &result.iso).error_code == UHDR_CODEC_OK);
  HDRSHOT_CHECK(ultrahdr::getMetadataFromXMP(
      static_cast<std::uint8_t*>(parsed.getXMPPtr()), parsed.getXMPSize(),
      nullptr, 0, &result.xmp).error_code == UHDR_CODEC_OK);
  HDRSHOT_CHECK(result.iso.use_base_cg && result.xmp.use_base_cg);
  HDRSHOT_CHECK(result.iso.are_all_channels_identical());
  HDRSHOT_CHECK(result.xmp.are_all_channels_identical());
  auto same = [](float a, float b) {
    HDRSHOT_CHECK(std::isfinite(a) && std::isfinite(b));
    // XMP decimal serialization and ISO rational serialization differ slightly.
    HDRSHOT_CHECK(std::abs(a - b) <= 1e-5F * std::max(1.0F, std::abs(a)));
  };
  for (unsigned c = 0; c < 3; ++c) {
    same(result.iso.min_content_boost[c], result.xmp.min_content_boost[c]);
    same(result.iso.max_content_boost[c], result.xmp.max_content_boost[c]);
    same(result.iso.gamma[c], result.xmp.gamma[c]);
    same(result.iso.offset_sdr[c], result.xmp.offset_sdr[c]);
    same(result.iso.offset_hdr[c], result.xmp.offset_hdr[c]);
  }
  same(result.iso.hdr_capacity_min, result.xmp.hdr_capacity_min);
  same(result.iso.hdr_capacity_max, result.xmp.hdr_capacity_max);
  return result;
}

// Reconstruct from each parsed declaration, not the decoder's preferred one.
// Both metadata paths use the very same decoded base/gain samples in linear P3.
inline void check_dual_reconstruction(uhdr_codec_private_t* decoder, bool require_color) {
  const auto metadata = check_dual_metadata(decoder);
  const auto* jpeg = uhdr_dec_get_base_image(decoder);
  ultrahdr::JpegDecoderHelper base_decoder;
  HDRSHOT_CHECK(base_decoder.decompressImage(jpeg->data, jpeg->data_sz,
      ultrahdr::DECODE_TO_RGB_CS).error_code == UHDR_CODEC_OK);
  const auto base = base_decoder.getDecompressedImage();
  const auto* gain = uhdr_get_decoded_gainmap_image(decoder);
  HDRSHOT_CHECK(gain && gain->fmt == UHDR_IMG_FMT_32bppRGBA8888);
  HDRSHOT_CHECK(gain->w == base.w && gain->h == base.h);
  HDRSHOT_CHECK(base.fmt == UHDR_IMG_FMT_24bppRGB888 || base.fmt == UHDR_IMG_FMT_32bppRGBA8888);
  const auto channels = base.fmt == UHDR_IMG_FMT_24bppRGB888 ? 3U : 4U;
  const auto* base_data = static_cast<const std::uint8_t*>(base.planes[UHDR_PLANE_PACKED]);
  const auto* gain_data = static_cast<const std::uint8_t*>(gain->planes[UHDR_PLANE_PACKED]);
  bool color = false;
  double worst = 0;
  for (unsigned y = 0; y < base.h; ++y) for (unsigned x = 0; x < base.w; ++x) {
    const auto b = (y * base.stride[UHDR_PLANE_PACKED] + x) * channels;
    const auto g = (y * gain->stride[UHDR_PLANE_PACKED] + x) * 4U;
    color |= gain_data[g] != gain_data[g + 1] || gain_data[g] != gain_data[g + 2];
    for (unsigned c = 0; c < 3; ++c) {
      const double code = base_data[b + c] / 255.0;
      const double linear = code <= 0.04045 ? code / 12.92 : std::pow((code + 0.055) / 1.055, 2.4);
      const double gain_code = gain_data[g + c] / 255.0;
      // Exercise equal physical display headroom for both declarations,
      // including a partial adaptation, not just forcing W=1.
      for (const double t : {0.0, 0.5, 1.0}) {
        const double display = std::exp2(std::lerp(std::log2(metadata.iso.hdr_capacity_min),
            std::log2(metadata.iso.hdr_capacity_max), t));
        auto restore = [&](const auto& m) {
          const double weight = std::clamp((std::log2(display) - std::log2(m.hdr_capacity_min)) /
              (std::log2(m.hdr_capacity_max) - std::log2(m.hdr_capacity_min)), 0.0, 1.0);
          const double log_gain = std::lerp(std::log2(m.min_content_boost[c]),
              std::log2(m.max_content_boost[c]), std::pow(gain_code, 1.0 / m.gamma[c]));
          return (linear + m.offset_sdr[c]) * std::exp2(log_gain * weight) - m.offset_hdr[c];
        };
        const auto a = restore(metadata.iso), z = restore(metadata.xmp);
        HDRSHOT_CHECK(std::isfinite(a) && std::isfinite(z));
        worst = std::max(worst, std::abs(a - z));
        HDRSHOT_CHECK(std::abs(a - z) <= 5e-5 * std::max(1.0, std::abs(a)));
      }
    }
  }
  HDRSHOT_CHECK(!require_color || color);
  std::cout << "ISO/XMP reconstruction maximum linear P3 difference=" << worst << '\n';
}

inline std::uint32_t icc_u32(std::span<const std::uint8_t> bytes, std::size_t at) {
  HDRSHOT_CHECK(at <= bytes.size() && bytes.size() - at >= 4U);
  return (std::uint32_t{bytes[at]} << 24U) |
      (std::uint32_t{bytes[at + 1]} << 16U) |
      (std::uint32_t{bytes[at + 2]} << 8U) | bytes[at + 3];
}

// These generated profiles fit one ICC APP2 segment. Walk JPEG headers, not
// arbitrary substring matches that could accidentally match entropy data.
inline std::vector<std::uint8_t> jpeg_icc(const uhdr_mem_block_t* jpeg) {
  HDRSHOT_CHECK(jpeg != nullptr && jpeg->data != nullptr && jpeg->data_sz >= 4U);
  std::span<const std::uint8_t> bytes{
      static_cast<const std::uint8_t*>(jpeg->data), jpeg->data_sz};
  HDRSHOT_CHECK(bytes[0] == 0xff && bytes[1] == 0xd8);
  for (std::size_t at = 2; at + 4U <= bytes.size();) {
    HDRSHOT_CHECK(bytes[at] == 0xff);
    const auto marker = bytes[at + 1];
    if (marker == 0xda || marker == 0xd9) break;
    const auto length = (std::size_t{bytes[at + 2]} << 8U) | bytes[at + 3];
    HDRSHOT_CHECK(length >= 2U && length <= bytes.size() - at - 2U);
    const auto payload = bytes.subspan(at + 4U, length - 2U);
    constexpr std::string_view signature{"ICC_PROFILE\0", 12};
    if (marker == 0xe2 && payload.size() >= 14U &&
        std::equal(signature.begin(), signature.end(), payload.begin())) {
      HDRSHOT_CHECK(payload[12] == 1 && payload[13] == 1);
      return {payload.begin() + 14, payload.end()};
    }
    at += length + 2U;
  }
  return {};
}

inline std::span<const std::uint8_t> icc_tag(
    std::span<const std::uint8_t> bytes, std::string_view signature) {
  HDRSHOT_CHECK(bytes.size() >= 132U && icc_u32(bytes, 0) == bytes.size());
  const auto count = icc_u32(bytes, 128);
  HDRSHOT_CHECK(count <= (bytes.size() - 132U) / 12U);
  for (std::size_t tag = 0; tag < count; ++tag) {
    const auto at = 132U + tag * 12U;
    if (!std::equal(signature.begin(), signature.end(), bytes.subspan(at).begin())) continue;
    const auto offset = icc_u32(bytes, at + 4U);
    const auto length = icc_u32(bytes, at + 8U);
    HDRSHOT_CHECK(offset <= bytes.size() && length <= bytes.size() - offset);
    return bytes.subspan(offset, length);
  }
  return {};
}

inline void check_p3_base_and_alternate(uhdr_codec_private_t* decoder) {
  HDRSHOT_CHECK(uhdr_dec_probe(decoder).error_code == UHDR_CODEC_OK);
  const auto base = jpeg_icc(uhdr_dec_get_base_image(decoder));
  const auto alternate = jpeg_icc(uhdr_dec_get_gainmap_image(decoder));
  HDRSHOT_CHECK(!base.empty() && !alternate.empty());
  for (const auto primary : {"rXYZ", "gXYZ", "bXYZ"}) {
    const auto base_xyz = icc_tag(base, primary);
    const auto alternate_xyz = icc_tag(alternate, primary);
    HDRSHOT_CHECK(base_xyz.size() == 20U);
    HDRSHOT_CHECK(std::ranges::equal(base_xyz, alternate_xyz));
  }
  const auto cicp = icc_tag(alternate, "cicp");
  HDRSHOT_CHECK(cicp.size() == 12U);
  HDRSHOT_CHECK(cicp[8] == 12 && cicp[9] == 16 && cicp[10] == 0 && cicp[11] == 1);
  // Catch the upstream inverted write check: all three B curves, the CLUT,
  // and all three A curves must be serialized, not left zero-filled.
  for (const auto tag_name : {"A2B0", "B2A0"}) {
    const auto lut = icc_tag(alternate, tag_name);
    HDRSHOT_CHECK(lut.size() >= 80U);
    const auto b = icc_u32(lut, 12U);
    for (std::size_t channel = 0; channel < 3; ++channel)
      HDRSHOT_CHECK(icc_u32(lut, b + channel * 16U) == 0x70617261U);
    if (std::string_view(tag_name) == "A2B0") {
      const auto clut = icc_u32(lut, 24U);
      const auto a = icc_u32(lut, 28U);
      HDRSHOT_CHECK(clut < lut.size() && lut[clut] == 17U);
      for (std::size_t channel = 0; channel < 3; ++channel)
        HDRSHOT_CHECK(icc_u32(lut, a + channel * 16U) == 0x70617261U);
    }
  }
  const auto base_trc = icc_tag(base, "rTRC");
  HDRSHOT_CHECK(base_trc.size() >= 12U);
  HDRSHOT_CHECK(icc_u32(base_trc, 0) == 0x70617261U);  // parametric sRGB TRC
  HDRSHOT_CHECK(base_trc[8] == 0 && base_trc[9] == 3);
  const auto* metadata = uhdr_dec_get_gainmap_metadata(decoder);
  HDRSHOT_CHECK(metadata != nullptr && metadata->use_base_cg == 1);
}

}  // namespace hdrshot::test
