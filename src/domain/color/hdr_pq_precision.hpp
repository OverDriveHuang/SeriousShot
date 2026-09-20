#pragma once

#include <cstdint>

namespace hdrshot {

enum class HdrPqPrecision : std::uint8_t {
  bits_10 = 10,
  bits_12 = 12,
  bits_16 = 16,
};

[[nodiscard]] constexpr std::uint8_t hdr_pq_precision_bits(
    const HdrPqPrecision value) noexcept {
  return static_cast<std::uint8_t>(value);
}

[[nodiscard]] constexpr bool valid_hdr_pq_precision(
    const HdrPqPrecision value) noexcept {
  return value == HdrPqPrecision::bits_10 ||
      value == HdrPqPrecision::bits_12 ||
      value == HdrPqPrecision::bits_16;
}

}  // namespace hdrshot
