#pragma once

#include "core/frame.hpp"
#include "core/error.hpp"
#include "core/result.hpp"
#include "ports/source_range_probe_port.hpp"

#include <array>
#include <span>

namespace hdrshot {

// Windows adapter boundary. scRGB is already linear, D65 / BT.709 primaries.
// On HDR desktops 1.0 scRGB is 80 nit; the captured display's SDR white defines
// EDR 1.0. SDR/WCG desktops are display-relative and use scale 1.0 instead.
struct WindowsSourceWhite {
  bool hdr_active{};
  double sdr_white_nits{};
};

class WindowsColor {
 public:
  static Result<float, Error> scrgb_to_edr_scale(WindowsSourceWhite white);
  static std::array<float, 3> scrgb_to_linear_p3(
      std::array<float, 3> rgb, float scrgb_to_edr_scale) noexcept;
  static std::array<float, 3> linear_p3_to_scrgb(
      std::array<float, 3> rgb, float edr_to_scrgb_scale) noexcept;
  // Converts the source once without a gamma roundtrip or clipping negatives.
  static Result<std::vector<std::uint16_t>, Error> normalize_capture(
      std::span<const std::uint16_t> rgba_scrgb, WindowsSourceWhite white);
  static constexpr ColorEncoding linear_p3_encoding() {
    return {ColorPrimaries::display_p3, TransferFunction::linear, AlphaMode::opaque, 0.0};
  }
  static bool valid_linear_p3_roi(const SelectionRoiView& source) noexcept;
};

class WindowsLinearP3RangeProbe final : public SourceRangeProbePort {
 public:
  Result<RangeFitResult, Error> probe(
      const SelectionRoiView& source, const AnnotationPixelPlan& plan,
      RangeProbeOptimization optimization) override;
};
}  // namespace hdrshot
