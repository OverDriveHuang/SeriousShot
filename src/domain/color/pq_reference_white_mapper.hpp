#pragma once

#include "core/error.hpp"
#include "core/result.hpp"
#include "domain/color/hdr_pq_precision.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hdrshot {

struct PqMapResult {
  std::uint16_t png_u16{};
  bool clipped{};
  double source_nits{};
  double output_nits{};
  double output_pq_code{};
};

class PqReferenceWhiteMapper {
 public:
  static constexpr double kDefaultSourceWhiteNits = 100.0;
  static constexpr double kTargetDiffuseWhiteNits = 203.0;
  static constexpr double kPqPeakNits = 10000.0;
  static constexpr std::uint16_t kFirstLegalHalfBits = 0x0000;
  static constexpr std::uint16_t kLastLegalHalfBits = 0x3C00;
  static constexpr std::uint16_t kLastNonClippedHalfBits = 0x3B66;
  static constexpr std::uint16_t kFirstClippedHalfBits = 0x3B67;
  static constexpr std::size_t kLutEntryCount = 15361;

  [[nodiscard]] static double st2084_eotf(double pq_code);
  [[nodiscard]] static double st2084_oetf(double nits);
  [[nodiscard]] static double decode_positive_binary16(std::uint16_t bits);
  [[nodiscard]] static std::uint16_t quantize_png_u16(double pq_code);
  [[nodiscard]] static std::uint16_t quantize_png_u16(
      double pq_code,
      HdrPqPrecision precision);

  [[nodiscard]] static Result<PqMapResult, Error> map_source_half(
      std::uint16_t half_bits,
      double source_reference_white_nits = kDefaultSourceWhiteNits,
      double target_diffuse_white_nits = kTargetDiffuseWhiteNits);

  [[nodiscard]] static Result<PqMapResult, Error> map_linear_edr(
      double linear_edr,
      double target_diffuse_white_nits = kTargetDiffuseWhiteNits,
      HdrPqPrecision precision = HdrPqPrecision::bits_16);

  [[nodiscard]] static std::vector<std::uint16_t> generate_default_lut();
};

}  // namespace hdrshot
