#include "domain/color/bt2020_srgb_icc_profile.hpp"
#include "domain/color/display_p3_icc_profile.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "domain/color/pq_reference_white_mapper.hpp"
#include "domain/color/pq_to_srgb_mapper.hpp"
#include "test_support.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace {

using hdrshot::ErrorCode;
using hdrshot::Bt2020SrgbIccProfile;
using hdrshot::DisplayP3IccProfile;
using hdrshot::ExtendedP3Mapper;
using hdrshot::HdrPqPrecision;
using hdrshot::PqReferenceWhiteMapper;
using hdrshot::PqToSrgbTransferMapper;

struct HalfCase {
  std::uint16_t bits;
  std::uint16_t expected_u16;
  bool clipped;
};

void half_vectors_match_reference() {
  constexpr std::array cases{
      HalfCase{0x0000, 0, false},
      HalfCase{0x3606, 28941, false},
      HalfCase{0x3810, 38037, false},
      HalfCase{0x38A2, 42869, false},
      HalfCase{0x3938, 47769, false},
      HalfCase{0x3A6D, 57684, false},
      HalfCase{0x3B66, 65526, false},
      HalfCase{0x3B67, 65535, true},
      HalfCase{0x3C00, 65535, true},
  };
  for (const auto& fixture : cases) {
    const auto actual = PqReferenceWhiteMapper::map_source_half(fixture.bits);
    HDRSHOT_CHECK(actual.has_value());
    HDRSHOT_CHECK(actual.value().png_u16 == fixture.expected_u16);
    HDRSHOT_CHECK(actual.value().clipped == fixture.clipped);
  }
}

void linear_edr_vectors_match_reference() {
  struct LinearCase {
    double edr;
    std::uint16_t expected_u16;
    bool clipped;
  };
  constexpr std::array cases{
      LinearCase{0.0, 0, false},
      LinearCase{0.25, 28947, false},
      LinearCase{1.0, 38055, false},
      LinearCase{2.0, 42871, false},
      LinearCase{4.0, 47785, false},
      LinearCase{16.0, 57676, false},
      LinearCase{10000.0 / 203.0, 65535, false},
      LinearCase{50.0, 65535, true},
  };
  for (const auto& fixture : cases) {
    const auto actual = PqReferenceWhiteMapper::map_linear_edr(fixture.edr);
    HDRSHOT_CHECK(actual.has_value());
    HDRSHOT_CHECK(actual.value().png_u16 == fixture.expected_u16);
    HDRSHOT_CHECK(actual.value().clipped == fixture.clipped);
  }
}

void full_lut_has_expected_shape_and_boundary() {
  const auto lut = PqReferenceWhiteMapper::generate_default_lut();
  HDRSHOT_CHECK(lut.size() == PqReferenceWhiteMapper::kLutEntryCount);
  HDRSHOT_CHECK(lut.size() * sizeof(std::uint16_t) == 30722);
  for (std::size_t index = 1; index < lut.size(); ++index) {
    HDRSHOT_CHECK(lut[index] >= lut[index - 1]);
  }
  HDRSHOT_CHECK(lut[0x3B66] == 65526);
  HDRSHOT_CHECK(lut[0x3B67] == 65535);
  HDRSHOT_CHECK(lut[0x3C00] == 65535);
  HDRSHOT_CHECK((0x3C00U - 0x3B67U + 1U) == 154U);
}

void invalid_half_classes_are_rejected() {
  for (const auto bits : {
           std::uint16_t{0x8001},
           std::uint16_t{0x3C01},
           std::uint16_t{0x7C00},
           std::uint16_t{0xFC00},
           std::uint16_t{0x7E00},
       }) {
    const auto result = PqReferenceWhiteMapper::map_source_half(bits);
    HDRSHOT_CHECK(!result.has_value());
    HDRSHOT_CHECK(result.error().code == ErrorCode::invalid_color_contract);
  }
}

void mathematical_reference_points_are_stable() {
  HDRSHOT_CHECK_NEAR(PqReferenceWhiteMapper::st2084_eotf(0.5078125), 99.735368165010, 0.0000005);
  HDRSHOT_CHECK_NEAR(PqReferenceWhiteMapper::st2084_oetf(203.0), 0.580688881042, 0.0000005);
  HDRSHOT_CHECK(PqReferenceWhiteMapper::quantize_png_u16(
                    PqReferenceWhiteMapper::st2084_oetf(203.0)) == 38055);
}

void invalid_linear_inputs_are_rejected() {
  for (const auto value : {
           -0.1,
           std::numeric_limits<double>::infinity(),
           std::numeric_limits<double>::quiet_NaN(),
       }) {
    const auto result = PqReferenceWhiteMapper::map_linear_edr(value);
    HDRSHOT_CHECK(!result.has_value());
    HDRSHOT_CHECK(result.error().code == ErrorCode::invalid_color_contract);
  }
}

void pq_bt2020_neutrals_map_to_srgb_and_clip_overwhite() {
  const auto black = PqReferenceWhiteMapper::map_linear_edr(0.0);
  const auto white = PqReferenceWhiteMapper::map_linear_edr(1.0);
  const auto overwhite = PqReferenceWhiteMapper::map_linear_edr(2.0);
  HDRSHOT_CHECK(black.has_value() && white.has_value() && overwhite.has_value());
  const std::array<std::uint16_t, 12> input{
      black.value().png_u16, black.value().png_u16, black.value().png_u16, 65535,
      white.value().png_u16, white.value().png_u16, white.value().png_u16, 65535,
      overwhite.value().png_u16, overwhite.value().png_u16, overwhite.value().png_u16, 65535,
  };
  const auto result = PqToSrgbTransferMapper::map_rgba16(input, 203.0);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value().rgba_bt2020_srgb_u16[0] == 0);
  HDRSHOT_CHECK(result.value().rgba_bt2020_srgb_u16[4] >= 65530);
  HDRSHOT_CHECK(result.value().rgba_bt2020_srgb_u16[5] >= 65530);
  HDRSHOT_CHECK(result.value().rgba_bt2020_srgb_u16[6] >= 65530);
  HDRSHOT_CHECK(result.value().rgba_bt2020_srgb_u16[8] == 65535);
  HDRSHOT_CHECK(result.value().rgba_bt2020_srgb_u16[9] == 65535);
  HDRSHOT_CHECK(result.value().rgba_bt2020_srgb_u16[10] == 65535);
  HDRSHOT_CHECK(result.value().clipped_pixel_count == 1);
  HDRSHOT_CHECK(result.value().clipped_channel_count == 3);
}

void sdr_transfer_conversion_preserves_bt2020_channels_without_relabeling() {
  const auto quarter = PqReferenceWhiteMapper::map_linear_edr(0.25);
  HDRSHOT_CHECK(quarter.has_value());
  const std::array<std::uint16_t, 4> red_only{
      quarter.value().png_u16, 0, 0, 65535};
  const auto result = PqToSrgbTransferMapper::map_rgba16(red_only, 203.0);
  HDRSHOT_CHECK(result.has_value());
  // sRGB OETF(0.25) ~= 0.5371. This also proves that the PQ code was decoded
  // and re-encoded instead of merely receiving different metadata.
  HDRSHOT_CHECK(result.value().rgba_bt2020_srgb_u16[0] >= 35190);
  HDRSHOT_CHECK(result.value().rgba_bt2020_srgb_u16[0] <= 35210);
  // No BT.2020-to-P3/BT.709 matrix is allowed in this transfer-only path.
  HDRSHOT_CHECK(result.value().rgba_bt2020_srgb_u16[1] == 0);
  HDRSHOT_CHECK(result.value().rgba_bt2020_srgb_u16[2] == 0);
  HDRSHOT_CHECK(result.value().rgba_bt2020_srgb_u16[3] == 65535);
}

std::uint32_t read_u32_be(
    const std::span<const std::uint8_t> bytes,
    const std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
      (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
      (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
      static_cast<std::uint32_t>(bytes[offset + 3U]);
}

void bt2020_srgb_icc_has_expected_matrix_trc_contract() {
  const auto profile = Bt2020SrgbIccProfile::bytes();
  HDRSHOT_CHECK(profile.size() >= 128U);
  HDRSHOT_CHECK(read_u32_be(profile, 0U) == profile.size());
  HDRSHOT_CHECK(read_u32_be(profile, 8U) == 0x04300000U);
  HDRSHOT_CHECK(read_u32_be(profile, 12U) == 0x6D6E7472U);  // mntr
  HDRSHOT_CHECK(read_u32_be(profile, 16U) == 0x52474220U);  // RGB
  HDRSHOT_CHECK(read_u32_be(profile, 20U) == 0x58595A20U);  // XYZ
  HDRSHOT_CHECK(read_u32_be(profile, 36U) == 0x61637370U);  // acsp
  HDRSHOT_CHECK(read_u32_be(profile, 128U) == 10U);

  bool has_red_colorant = false;
  bool has_green_colorant = false;
  bool has_blue_colorant = false;
  bool has_srgb_trc = false;
  for (std::size_t index = 0; index < 10U; ++index) {
    const auto entry = 132U + index * 12U;
    const auto tag = read_u32_be(profile, entry);
    const auto payload = static_cast<std::size_t>(read_u32_be(profile, entry + 4U));
    HDRSHOT_CHECK(payload + 12U <= profile.size());
    has_red_colorant |= tag == 0x7258595AU;
    has_green_colorant |= tag == 0x6758595AU;
    has_blue_colorant |= tag == 0x6258595AU;
    if (tag == 0x72545243U) {
      HDRSHOT_CHECK(read_u32_be(profile, payload) == 0x70617261U);  // para
      HDRSHOT_CHECK(profile[payload + 8U] == 0U && profile[payload + 9U] == 3U);
      has_srgb_trc = true;
    }
  }
  HDRSHOT_CHECK(has_red_colorant && has_green_colorant && has_blue_colorant);
  HDRSHOT_CHECK(has_srgb_trc);
}

void extended_display_p3_decode_and_sdr_direct_quantization_are_stable() {
  const auto half = ExtendedP3Mapper::decode_binary16(0x3E00);
  HDRSHOT_CHECK(half.has_value());
  HDRSHOT_CHECK_NEAR(half.value(), 1.5F, 0.000001F);
  const auto linear = ExtendedP3Mapper::inverse_extended_srgb(half.value());
  HDRSHOT_CHECK(linear > 1.0F);
  HDRSHOT_CHECK_NEAR(
      ExtendedP3Mapper::encode_extended_srgb(linear), 1.5F, 0.000001F);
  HDRSHOT_CHECK(ExtendedP3Mapper::quantize_unorm16(0.5F) == 32768U);
  HDRSHOT_CHECK(ExtendedP3Mapper::annotation_display_p3_u16(0xFFFFFF) ==
                (std::array<std::uint16_t, 3>{65535U, 65535U, 65535U}));
}

void display_p3_icc_has_expected_matrix_trc_contract() {
  const auto profile = DisplayP3IccProfile::bytes();
  HDRSHOT_CHECK(profile.size() >= 128U);
  HDRSHOT_CHECK(read_u32_be(profile, 0U) == profile.size());
  HDRSHOT_CHECK(read_u32_be(profile, 8U) == 0x04200000U);
  HDRSHOT_CHECK(read_u32_be(profile, 12U) == 0x6D6E7472U);  // mntr
  HDRSHOT_CHECK(read_u32_be(profile, 16U) == 0x52474220U);  // RGB
  HDRSHOT_CHECK(read_u32_be(profile, 128U) == 10U);
  bool has_p3_colorants = false;
  bool has_srgb_trc = false;
  for (std::size_t index = 0; index < 10U; ++index) {
    const auto entry = 132U + index * 12U;
    const auto tag = read_u32_be(profile, entry);
    const auto payload = static_cast<std::size_t>(read_u32_be(profile, entry + 4U));
    if (tag == 0x7258595AU) {
      has_p3_colorants = read_u32_be(profile, payload + 8U) == 0x000083DFU;
    }
    if (tag == 0x72545243U) {
      has_srgb_trc = read_u32_be(profile, payload) == 0x70617261U &&
          profile[payload + 8U] == 0U && profile[payload + 9U] == 3U;
    }
  }
  HDRSHOT_CHECK(has_p3_colorants);
  HDRSHOT_CHECK(has_srgb_trc);
}

void hdr_pq_effective_precision_expands_into_standard_u16_samples() {
  const auto pq = 0.537291;
  const auto full = PqReferenceWhiteMapper::quantize_png_u16(
      pq, HdrPqPrecision::bits_16);
  const auto high = PqReferenceWhiteMapper::quantize_png_u16(
      pq, HdrPqPrecision::bits_12);
  const auto small = PqReferenceWhiteMapper::quantize_png_u16(
      pq, HdrPqPrecision::bits_10);
  HDRSHOT_CHECK(full != high);
  HDRSHOT_CHECK(high != small);
  for (const auto [sample, maximum_code] : {
           std::pair{high, 4095U},
           std::pair{small, 1023U},
       }) {
    const auto code = static_cast<std::uint32_t>(std::floor(
        static_cast<double>(sample) * maximum_code / 65535.0 + 0.5));
    const auto expanded = static_cast<std::uint16_t>(std::floor(
        static_cast<double>(code) * 65535.0 / maximum_code + 0.5));
    HDRSHOT_CHECK(sample == expanded);
  }
}

}  // namespace

int main() {
  return hdrshot::test::run({
      {"D1-PQ-LUT-001 reference half vectors", half_vectors_match_reference},
      {"D1-PQ-LINEAR-001 reference EDR vectors", linear_edr_vectors_match_reference},
      {"D1-PQ-LUT-002 full table", full_lut_has_expected_shape_and_boundary},
      {"D1-PQ-INVALID-001 invalid half", invalid_half_classes_are_rejected},
      {"D1-PQ-MATH-001 ST2084 points", mathematical_reference_points_are_stable},
      {"D1-PQ-INVALID-002 invalid linear", invalid_linear_inputs_are_rejected},
      {"D1-PQ-SDR-001 PQ BT.2020 to sRGB", pq_bt2020_neutrals_map_to_srgb_and_clip_overwhite},
      {"D1-PQ-SDR-002 transfer conversion preserves BT.2020 channels", sdr_transfer_conversion_preserves_bt2020_channels_without_relabeling},
      {"D1-ICC-001 BT.2020 sRGB matrix/TRC profile", bt2020_srgb_icc_has_expected_matrix_trc_contract},
      {"D1-P3-001 Extended Display P3 decode/direct SDR", extended_display_p3_decode_and_sdr_direct_quantization_are_stable},
      {"D1-ICC-002 Display P3 matrix/TRC profile", display_p3_icc_has_expected_matrix_trc_contract},
      {"D1-PQ-PRECISION-001 effective PQ precision", hdr_pq_effective_precision_expands_into_standard_u16_samples},
  });
}
