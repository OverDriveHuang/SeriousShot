#include "platform/macos/screen_capture_kit_adapter.hpp"

#include <dispatch/dispatch.h>

#include <atomic>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

namespace {

std::string fourcc(const std::uint32_t value) {
  std::string result(4, ' ');
  result[0] = static_cast<char>((value >> 24U) & 0xFFU);
  result[1] = static_cast<char>((value >> 16U) & 0xFFU);
  result[2] = static_cast<char>((value >> 8U) & 0xFFU);
  result[3] = static_cast<char>(value & 0xFFU);
  return result;
}

struct RunState {
  std::atomic<std::size_t> remaining{};
  std::atomic<bool> failed{};
  std::mutex output_mutex;
};

}  // namespace

int main() {
  using hdrshot::MacScreenCaptureKitAdapter;

  std::cout << "screenCaptureAccess="
            << (MacScreenCaptureKitAdapter::has_capture_access() ? "granted" : "not_granted")
            << '\n';

  MacScreenCaptureKitAdapter::enumerate_displays([](auto display_result) {
    if (!display_result) {
      std::cerr << "enumerate failed: " << hdrshot::to_string(display_result.error().code) << '\n';
      std::exit(2);
    }
    const auto& displays = display_result.value();
    if (displays.empty()) {
      std::cerr << "enumerate returned no displays\n";
      std::exit(3);
    }

    auto state = std::make_shared<RunState>();
    state->remaining = displays.size();
    for (const auto& display : displays) {
      std::cout << "catalog displayId=" << display.display_id
                << " framePt=" << display.desktop_x_pt << ',' << display.desktop_y_pt << ','
                << display.width_pt << ',' << display.height_pt
                << " pointPixelScale=" << display.point_pixel_scale
                << " capturePx=" << display.capture_width_px << 'x'
                << display.capture_height_px << '\n';
      MacScreenCaptureKitAdapter::capture_display(display.display_id, [state](auto capture_result) {
        {
          const std::scoped_lock lock(state->output_mutex);
          if (!capture_result) {
            state->failed = true;
            std::cerr << "capture failed: "
                      << hdrshot::to_string(capture_result.error().code) << '\n';
          } else {
            const auto& frame = capture_result.value();
            const auto& descriptor = frame.descriptor;
            std::cout << "displayId=" << descriptor.display_id
                      << " size=" << descriptor.width_px << 'x' << descriptor.height_px
                      << " sourceStride=" << descriptor.source_bytes_per_row
                      << " pixelFormat=" << fourcc(descriptor.pixel_format_fourcc)
                      << " primaries=" << descriptor.color_primaries
                      << " transfer=" << descriptor.transfer_function
                      << " sourceWhiteNits=" << std::fixed << std::setprecision(1)
                      << descriptor.source_reference_white_nits
                      << " sampleCount=" << frame.rgba_half_extended_p3.size() << '\n';
          }
        }
        if (state->remaining.fetch_sub(1) == 1) {
          std::exit(state->failed ? 4 : 0);
        }
      });
    }
  });

  dispatch_main();
}
