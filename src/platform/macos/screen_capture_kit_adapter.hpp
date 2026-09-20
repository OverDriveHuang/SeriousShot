#pragma once

#include "core/error.hpp"
#include "core/result.hpp"
#include <CoreGraphics/CoreGraphics.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hdrshot {

inline constexpr std::uint32_t kMacRgbaHalfFourcc =
    (static_cast<std::uint32_t>('R') << 24U) |
    (static_cast<std::uint32_t>('G') << 16U) |
    (static_cast<std::uint32_t>('h') << 8U) |
    static_cast<std::uint32_t>('A');

struct MacDisplayInfo {
  std::uint32_t display_id{};
  double desktop_x_pt{};
  double desktop_y_pt{};
  double width_pt{};
  double height_pt{};
  double point_pixel_scale{};
  std::size_t capture_width_px{};
  std::size_t capture_height_px{};
  double current_maximum_edr{1.0};
  double maximum_potential_edr{1.0};

  friend bool operator==(const MacDisplayInfo&, const MacDisplayInfo&) = default;
};

struct MacCaptureFrameDescriptor {
  std::uint32_t display_id{};
  std::size_t width_px{};
  std::size_t height_px{};
  std::size_t source_bytes_per_row{};
  std::uint32_t pixel_format_fourcc{};
  std::string color_primaries;
  std::string transfer_function;
  double source_reference_white_nits{};
  std::uint32_t bitmap_info{};
  std::string returned_space{};

  friend bool operator==(const MacCaptureFrameDescriptor&, const MacCaptureFrameDescriptor&) = default;
};

struct MacCapturedFrame {
  MacCaptureFrameDescriptor descriptor;
  std::vector<std::uint16_t> rgba_half_extended_p3;
};

class MacScreenCaptureKitAdapter {
 public:
  using DisplayListCompletion =
      std::function<void(Result<std::vector<MacDisplayInfo>, Error>)>;
  using CaptureCompletion = std::function<void(Result<MacCapturedFrame, Error>)>;

  [[nodiscard]] static bool has_capture_access() noexcept;
  static void enumerate_displays(DisplayListCompletion completion);
  static void capture_display(std::uint32_t display_id, CaptureCompletion completion);
  [[nodiscard]] static Result<MacCapturedFrame, Error> copy_image(
      CGImageRef image, std::uint32_t display_id);

  [[nodiscard]] static Result<MacCaptureFrameDescriptor, Error> validate_descriptor(
      MacCaptureFrameDescriptor descriptor);
  [[nodiscard]] static Error map_stream_error_code(std::int64_t code, bool stream_error_domain);
};

}  // namespace hdrshot
