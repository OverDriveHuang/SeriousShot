#pragma once

#include <bit>
#include <cstdint>

namespace hdrshot {

inline constexpr std::uint32_t kCaptureSdrMaximumEdrBits = 0x3f8050b5U;
inline constexpr float kCaptureSdrMaximumEdr =
    std::bit_cast<float>(kCaptureSdrMaximumEdrBits);
inline constexpr double kCaptureHdrMinimumEdr = 407.0 / 406.0;

// 407/406 lies between this FP32 sample and its successor. Classification is
// inclusive here; source pixels and physical maximum metadata are never changed.
[[nodiscard]] inline constexpr float source_sdr_maximum_edr(bool capture_sdr_tolerance) noexcept {
  return capture_sdr_tolerance ? kCaptureSdrMaximumEdr : 1.0F;
}

[[nodiscard]] inline constexpr bool source_requires_hdr(double maximum,
    bool capture_sdr_tolerance = false) noexcept {
  return capture_sdr_tolerance ? maximum >= kCaptureHdrMinimumEdr : maximum > 1.0;
}

}
