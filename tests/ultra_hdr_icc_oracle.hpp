#pragma once

#include "ultra_hdr_metadata_test_support.hpp"
#include <array>
#include <cmath>

namespace hdrshot::test {

// Independent double-precision oracle; never call libultrahdr's PQ/LUT helpers.
// This deliberately includes the retained ICC SDR rendering, NOT pure HDR EOTF.
using IccVector = std::array<double, 3>;
using IccMatrix = std::array<IccVector, 3>;

inline double icc_reference_pq_eotf(double code) {
  const double p = std::pow(code, 32.0 / 2523.0);
  return std::pow(std::max(p - 3424.0 / 4096.0, 0.0) /
      (2413.0 / 128.0 - 2392.0 / 128.0 * p), 16384.0 / 2610.0);
}

inline IccVector icc_multiply(const IccMatrix& m, const IccVector& v) {
  IccVector result{};
  for (std::size_t row = 0; row < 3; ++row)
    for (std::size_t col = 0; col < 3; ++col) result[row] += m[row][col] * v[col];
  return result;
}

inline IccMatrix icc_inverse(const IccMatrix& m) {
  IccMatrix cofactor{};
  for (std::size_t r = 0; r < 3; ++r)
    for (std::size_t c = 0; c < 3; ++c)
      cofactor[r][c] = m[(r+1)%3][(c+1)%3] * m[(r+2)%3][(c+2)%3] -
          m[(r+1)%3][(c+2)%3] * m[(r+2)%3][(c+1)%3];
  const double det = m[0][0]*cofactor[0][0] + m[0][1]*cofactor[0][1] +
      m[0][2]*cofactor[0][2];
  HDRSHOT_CHECK(std::abs(det) > 1e-6);
  IccMatrix result{};
  for (std::size_t r = 0; r < 3; ++r)
    for (std::size_t c = 0; c < 3; ++c) result[r][c] = cofactor[c][r] / det;
  return result;
}

inline IccVector icc_expected_p3_lab(IccVector code) {
  // Existing writer constants, kept separate from production headers/math.
  constexpr IccMatrix p3 = {{{33759.0/65536, 19135.0/65536, 10296.0/65536},
      {15807.0/65536, 45367.0/65536, 4363.0/65536},
      {-69.0/65536, 2745.0/65536, 51385.0/65536}}};
  constexpr IccMatrix rec2020 = {{{.673459, .165661, .125100},
      {.279033, .675338, .0456288}, {-.00193139, .0299794, .797162}}};
  static const auto from_xyz = icc_inverse(rec2020);
  for (double& v : code) v = icc_reference_pq_eotf(v);
  auto xyz = icc_multiply(p3, code);
  const auto rgb2020 = icc_multiply(from_xyz, xyz);
  const double luminance = .2627*rgb2020[0] + .677998*rgb2020[1] + .059302*rgb2020[2];
  constexpr double k = 10000.0 / 203.0;
  const double relative = k * luminance;
  const double gain = luminance <= 0 ? 1 : k * (1 + relative/(k*k)) / (1 + relative);
  constexpr IccVector white = {.9642, 1, .8249};
  for (std::size_t i = 0; i < 3; ++i) {
    const double v = xyz[i] * gain / white[i];
    xyz[i] = v > .008856 ? std::cbrt(v) : 7.787*v + 16.0/116.0;
  }
  IccVector lab = {(116*xyz[1]-16)/100, (500*(xyz[0]-xyz[1])+128)/255,
      (200*(xyz[1]-xyz[2])+128)/255};
  for (double& v : lab) v = std::clamp(v, 0.0, 1.0);
  return lab;
}

inline std::uint16_t icc_u16(std::span<const std::uint8_t> bytes, std::size_t at) {
  HDRSHOT_CHECK(at <= bytes.size() && bytes.size() - at >= 2U);
  return static_cast<std::uint16_t>((std::uint16_t{bytes[at]} << 8U) | bytes[at+1]);
}

inline void check_p3_pq_a2b0_values(std::span<const std::uint8_t> profile) {
  HDRSHOT_CHECK(icc_u32(profile, 20) == 0x4c616220U);
  const auto table = icc_tag(profile, "A2B0");
  HDRSHOT_CHECK(table.size() >= 32U && icc_u32(table, 0) == 0x6d414220U);
  HDRSHOT_CHECK(table[8] == 3 && table[9] == 3);
  HDRSHOT_CHECK(icc_u32(table, 16) == 0 && icc_u32(table, 20) == 0);
  // Both curve sets must be identity, otherwise the nodes aren't raw PQ -> PCS.
  for (const auto offset_field : {12U, 28U}) {
    const auto offset = icc_u32(table, offset_field);
    for (std::size_t channel = 0; channel < 3; ++channel) {
      const auto at = offset + channel*16;
      HDRSHOT_CHECK(icc_u32(table, at) == 0x70617261U);
      HDRSHOT_CHECK(icc_u32(table, at+8) == 0 && icc_u32(table, at+12) == 65536);
    }
  }
  const auto clut = icc_u32(table, 24);
  HDRSHOT_CHECK(clut <= table.size() && table.size()-clut >= 20+17*17*17*6);
  HDRSHOT_CHECK(table[clut] == 17 && table[clut+1] == 17 && table[clut+2] == 17);
  HDRSHOT_CHECK(table[clut+16] == 2);
  // All 4913 nodes, including black/white, gray ramp and off-axis saturated RGB.
  // <=8 16-bit PCS codes allows float PQ/matrix rounding, not rendering changes.
  for (std::size_t r = 0; r < 17; ++r)
    for (std::size_t g = 0; g < 17; ++g)
      for (std::size_t b = 0; b < 17; ++b) {
        const auto expected = icc_expected_p3_lab(
            {static_cast<double>(r)/16, static_cast<double>(g)/16, static_cast<double>(b)/16});
        const auto at = clut + 20 + ((r*17+g)*17+b)*6;
        for (std::size_t c = 0; c < 3; ++c)
          HDRSHOT_CHECK_NEAR(icc_u16(table, at+c*2)/65535.0, expected[c], 8.0/65535);
      }
  const auto gray_at = clut + 20 + ((8*17+8)*17+8)*6;
  const double lightness = icc_u16(table, gray_at)/65535.0*100;
  HDRSHOT_CHECK_NEAR(lightness, 62.718, .015);
  std::cout << "A2B0: 4913 nodes checked; PQ gray 0.5 -> L*=" << lightness << '\n';
}

} // namespace hdrshot::test
