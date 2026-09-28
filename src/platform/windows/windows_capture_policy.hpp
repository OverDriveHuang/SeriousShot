#pragma once

#include "core/error.hpp"
#include "core/result.hpp"

#include <cmath>

namespace hdrshot {

struct WindowsCaptureOptions {
  bool gain_enabled{false};
  bool bypass_sdr_white_enabled{false};
  double gain{0.5};
};

struct WindowsCaptureAdjustment {
  double effective_gain{1.0};
  float white_scale{1.0F};
};

// Pure capture policy. The caller still obtains and validates normal_scale
// through WindowsColor, including when the HDR white bypass is enabled.
[[nodiscard]] inline Result<WindowsCaptureAdjustment, Error>
resolve_capture_adjustment(const WindowsCaptureOptions options,
                           const bool hdr_active,
                           const float normal_scale) {
  using Out = Result<WindowsCaptureAdjustment, Error>;
  if (!std::isfinite(options.gain) || options.gain < 0.0 || options.gain > 3.0)
    return Out::failure({ErrorCode::invalid_input, "WindowsCapturePolicy",
                         Retryability::never, {{"reason", "invalid_gain"}}});
  if (!std::isfinite(normal_scale) || normal_scale <= 0.0F)
    return Out::failure({ErrorCode::invalid_input, "WindowsCapturePolicy",
                         Retryability::never, {{"reason", "invalid_normal_scale"}}});
  return Out::success({options.gain_enabled ? options.gain : 1.0,
                       hdr_active && !options.bypass_sdr_white_enabled
                           ? normal_scale : 1.0F});
}

}  // namespace hdrshot
