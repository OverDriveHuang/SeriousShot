#pragma once
#include <algorithm>
#include <cmath>

namespace hdrshot::pinned_window_style {
// Window decoration only: neither the Source viewport nor export pixels change.
inline constexpr int resize_hit_width = 4;
inline constexpr double border_line_width = .5; // logical pixels, for each ring
inline constexpr double outer_black_alpha = .34;
inline constexpr double inner_white_alpha = .65;
inline double border_line_physical_pixels(double device_pixel_ratio) {
  return std::max(1., std::round(border_line_width * device_pixel_ratio));
}
} // namespace hdrshot::pinned_window_style
