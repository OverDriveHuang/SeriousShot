#include "domain/color/pq_reference_white_mapper.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hdrshot {
namespace {

constexpr double kM1 = 2610.0 / 16384.0;
constexpr double kM2 = 2523.0 / 32.0;
constexpr double kC1 = 3424.0 / 4096.0;
constexpr double kC2 = 2413.0 / 128.0;
constexpr double kC3 = 2392.0 / 128.0;

[[nodiscard]] Error invalid_color_error(const char* reason) {
  return Error{
      ErrorCode::invalid_color_contract,
      "PqReferenceWhiteMapper",
      Retryability::never,
      {{"reason", reason}},
  };
}

}  // namespace

double PqReferenceWhiteMapper::st2084_eotf(const double pq_code) {
  const auto code = std::clamp(pq_code, 0.0, 1.0);
  const auto p = std::pow(code, 1.0 / kM2);
  const auto numerator = std::max(p - kC1, 0.0);
  const auto denominator = kC2 - (kC3 * p);
  return kPqPeakNits * std::pow(numerator / denominator, 1.0 / kM1);
}

double PqReferenceWhiteMapper::st2084_oetf(const double nits) {
  const auto normalized = std::clamp(nits, 0.0, kPqPeakNits) / kPqPeakNits;
  const auto p = std::pow(normalized, kM1);
  return std::pow((kC1 + (kC2 * p)) / (1.0 + (kC3 * p)), kM2);
}

double PqReferenceWhiteMapper::decode_positive_binary16(const std::uint16_t bits) {
  const auto exponent = static_cast<std::uint16_t>((bits >> 10U) & 0x1FU);
  const auto mantissa = static_cast<std::uint16_t>(bits & 0x03FFU);
  if (exponent == 0U) {
    return std::ldexp(static_cast<double>(mantissa), -24);
  }
  const auto significand = 1.0 + (static_cast<double>(mantissa) / 1024.0);
  return std::ldexp(significand, static_cast<int>(exponent) - 15);
}

std::uint16_t PqReferenceWhiteMapper::quantize_png_u16(const double pq_code) {
  const auto clamped = std::clamp(pq_code, 0.0, 1.0);
  return static_cast<std::uint16_t>(std::floor((clamped * 65535.0) + 0.5));
}

std::uint16_t PqReferenceWhiteMapper::quantize_png_u16(
    const double pq_code,
    const HdrPqPrecision precision) {
  const auto bits = hdr_pq_precision_bits(precision);
  if (!valid_hdr_pq_precision(precision) || bits == 0U || bits > 16U) {
    return 0U;
  }
  const auto levels = static_cast<std::uint32_t>((1U << bits) - 1U);
  const auto clamped = std::clamp(pq_code, 0.0, 1.0);
  const auto effective_code = static_cast<std::uint32_t>(
      std::floor(clamped * static_cast<double>(levels) + 0.5));
  return static_cast<std::uint16_t>(std::floor(
      static_cast<double>(effective_code) * 65535.0 /
          static_cast<double>(levels) +
      0.5));
}

Result<PqMapResult, Error> PqReferenceWhiteMapper::map_source_half(
    const std::uint16_t half_bits,
    const double source_reference_white_nits,
    const double target_diffuse_white_nits) {
  const auto sign = (half_bits & 0x8000U) != 0U;
  const auto exponent = static_cast<std::uint16_t>((half_bits >> 10U) & 0x1FU);
  if (sign) {
    return Result<PqMapResult, Error>::failure(invalid_color_error("negative_half"));
  }
  if (exponent == 0x1FU) {
    return Result<PqMapResult, Error>::failure(invalid_color_error("non_finite_half"));
  }
  if (half_bits > kLastLegalHalfBits) {
    return Result<PqMapResult, Error>::failure(invalid_color_error("pq_code_above_one"));
  }
  if (!std::isfinite(source_reference_white_nits) || source_reference_white_nits <= 0.0 ||
      !std::isfinite(target_diffuse_white_nits) || target_diffuse_white_nits <= 0.0) {
    return Result<PqMapResult, Error>::failure(invalid_color_error("invalid_reference_white"));
  }

  const auto input_pq = decode_positive_binary16(half_bits);
  const auto source_nits = st2084_eotf(input_pq);
  const auto unbounded_output_nits =
      source_nits * target_diffuse_white_nits / source_reference_white_nits;
  const auto clipped = unbounded_output_nits > kPqPeakNits;
  const auto output_nits = std::min(unbounded_output_nits, kPqPeakNits);
  const auto output_pq = st2084_oetf(output_nits);
  return Result<PqMapResult, Error>::success(PqMapResult{
      quantize_png_u16(output_pq),
      clipped,
      source_nits,
      output_nits,
      output_pq,
  });
}

Result<PqMapResult, Error> PqReferenceWhiteMapper::map_linear_edr(
    const double linear_edr,
    const double target_diffuse_white_nits,
    const HdrPqPrecision precision) {
  if (!std::isfinite(linear_edr) || linear_edr < 0.0) {
    return Result<PqMapResult, Error>::failure(invalid_color_error("invalid_linear_edr"));
  }
  if (!std::isfinite(target_diffuse_white_nits) || target_diffuse_white_nits <= 0.0) {
    return Result<PqMapResult, Error>::failure(invalid_color_error("invalid_reference_white"));
  }
  if (!valid_hdr_pq_precision(precision)) {
    return Result<PqMapResult, Error>::failure(
        invalid_color_error("invalid_hdr_pq_precision"));
  }

  const auto unbounded_output_nits = linear_edr * target_diffuse_white_nits;
  const auto clipped = unbounded_output_nits > kPqPeakNits;
  const auto output_nits = std::min(unbounded_output_nits, kPqPeakNits);
  const auto output_pq = st2084_oetf(output_nits);
  return Result<PqMapResult, Error>::success(PqMapResult{
      quantize_png_u16(output_pq, precision),
      clipped,
      linear_edr * kDefaultSourceWhiteNits,
      output_nits,
      output_pq,
  });
}

std::vector<std::uint16_t> PqReferenceWhiteMapper::generate_default_lut() {
  std::vector<std::uint16_t> lut;
  lut.reserve(kLutEntryCount);
  for (std::uint32_t bits = kFirstLegalHalfBits; bits <= kLastLegalHalfBits; ++bits) {
    const auto mapped = map_source_half(static_cast<std::uint16_t>(bits));
    if (!mapped.has_value()) {
      return {};
    }
    lut.push_back(mapped.value().png_u16);
  }
  return lut;
}

}  // namespace hdrshot
