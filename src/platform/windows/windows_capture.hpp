#pragma once
#include "platform/windows/windows_color.hpp"
#include "platform/windows/windows_capture_policy.hpp"
#include "platform/windows/windows_display_mode.hpp"
#include "ports/capture_preview_ports.hpp"
#include <atomic>
#include <memory>
#include <string>

namespace hdrshot {
class DiagnosticsPort;
class WindowsIccProfile;
[[nodiscard]] inline WindowsCaptureOptions windows_capture_options_from_settings(
    const SettingsSnapshot& settings) noexcept {
  return {settings.windows_capture_gain_enabled,
          settings.windows_capture_bypass_sdr_white_enabled,
          settings.windows_scrgb_gain};
}
struct WindowsDisplayInfo {
  DisplaySnapshot snapshot;
  std::uintptr_t monitor{};
  std::int32_t physical_x{}, physical_y{};
  std::string device_name, friendly_name, gpu_name, icc_path;
  std::uint32_t color_space{}, bits_per_color{}, advanced_color_mode{};
  std::uint32_t detected_mode{3}, mode_query_code{}, mode_query_flags{}, mode_raw_active{};
  std::string mode_query_source, mode_fallback_reason;
  std::uint32_t dxgi_adapter_low{};
  std::int32_t dxgi_adapter_high{};
  std::uint32_t target_adapter_low{}, target_id{};
  std::int32_t target_adapter_high{};
  float min_nits{}, max_nits{}, max_full_frame_nits{};
  WindowsSourceWhite source_white;
  std::shared_ptr<const WindowsIccProfile> icc_profile;
};
struct WindowsRawCapture {
  DisplayId display_id;
  PixelSize size_px;
  std::int64_t capture_time_100ns{};
  std::uint32_t texture_format{};
  std::vector<std::uint16_t> rgba_scrgb;
};
// Read-only startup preflight for hard WGC requirements. This does not create
// a capture session or request the optional borderless permission.
Result<WindowsCaptureCapabilityPlan, Error> windows_check_capture_runtime_capabilities(
    DiagnosticsPort* diagnostics = nullptr);
// Both calls run on a worker thread in the application; the probe may call them
// synchronously. Raw evidence stays at this platform boundary.
Result<std::vector<WindowsDisplayInfo>, Error> windows_enumerate_displays(
    DiagnosticsPort* diagnostics = nullptr);
Result<WindowsRawCapture, Error> windows_capture_display(
    const WindowsDisplayInfo& display, const std::atomic_bool* cancelled = nullptr,
    std::uint32_t timeout_ms = 5000, DiagnosticsPort* diagnostics = nullptr);
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
  explicit WindowsCapturePort(std::shared_ptr<const std::vector<WindowsDisplayInfo>> displays = {},
      double gain = 1.0, std::shared_ptr<DiagnosticsPort> diagnostics = {});
  WindowsCapturePort(std::shared_ptr<const std::vector<WindowsDisplayInfo>> displays,
      WindowsCaptureOptions options, std::shared_ptr<DiagnosticsPort> diagnostics = {});
  ~WindowsCapturePort() override;
  // Applied settings affect only captures started after this call.
  void set_gain(double gain);
  void set_capture_options(WindowsCaptureOptions options);
  void capture(const CaptureBatchRequest& request, Completion completion) override;
  void cancel(SessionId session_id, OperationId operation_id) override;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace hdrshot
