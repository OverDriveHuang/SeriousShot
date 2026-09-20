#pragma once
#include "platform/windows/windows_color.hpp"
#include "ports/capture_preview_ports.hpp"
#include <atomic>
#include <memory>
#include <string>

namespace hdrshot {
struct WindowsDisplayInfo {
  DisplaySnapshot snapshot;
  std::uintptr_t monitor{};
  std::int32_t physical_x{}, physical_y{};
  std::string device_name, friendly_name, gpu_name, icc_path;
  std::uint32_t color_space{}, bits_per_color{}, advanced_color_mode{};
  float min_nits{}, max_nits{}, max_full_frame_nits{};
  WindowsSourceWhite source_white;
};
struct WindowsRawCapture {
  DisplayId display_id;
  PixelSize size_px;
  std::int64_t capture_time_100ns{};
  std::uint32_t texture_format{};
  std::vector<std::uint16_t> rgba_scrgb;
};
// Both calls run on a worker thread in the application; the probe may call them
// synchronously. Raw evidence stays at this platform boundary.
Result<std::vector<WindowsDisplayInfo>, Error> windows_enumerate_displays();
Result<WindowsRawCapture, Error> windows_capture_display(
    const WindowsDisplayInfo& display, const std::atomic_bool* cancelled = nullptr,
    std::uint32_t timeout_ms = 5000);
std::uint32_t windows_build_number();

class WindowsDisplayCatalogPort final : public DisplayCatalogPort {
 public:
  explicit WindowsDisplayCatalogPort(std::shared_ptr<const std::vector<WindowsDisplayInfo>> displays = {}):displays_(std::move(displays)){}
  void snapshot_displays(const SnapshotDisplaysRequest&, Completion completion) override;
 private:
  std::shared_ptr<const std::vector<WindowsDisplayInfo>> displays_;
};
class WindowsCapturePort final : public CapturePort {
 public:
  explicit WindowsCapturePort(std::shared_ptr<const std::vector<WindowsDisplayInfo>> displays = {});
  ~WindowsCapturePort() override;
  void capture(const CaptureBatchRequest& request, Completion completion) override;
  void cancel(SessionId session_id, OperationId operation_id) override;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace hdrshot
