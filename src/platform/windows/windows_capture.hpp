#pragma once
#include "platform/windows/windows_color.hpp"
#include "platform/windows/windows_capture_policy.hpp"
#include "platform/windows/windows_display_mode.hpp"
#include "ports/capture_preview_ports.hpp"
#include "ports/capture_progress_port.hpp"
#include "platform/windows/windows_capture_work.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace hdrshot {
class DiagnosticsPort;
class WindowsIccProfile;
class CaptureInFlightDiagnostics;
struct WindowsCaptureNativeCalls;
struct WindowsBorderlessAccess {
  bool allowed{};
  std::uint32_t native_code{};
  const char* reason{"bordered_fallback"};
  std::optional<Error> error;
};
WindowsBorderlessAccess windows_prepare_borderless_access();
struct WindowsCaptureHooks {
  CaptureOperationKey key{};
  DisplayId display{};
  std::shared_ptr<CaptureProgressObserver> observer;
  std::shared_ptr<CaptureInFlightDiagnostics> facts;
  std::shared_ptr<WindowsCaptureWork> work;
  std::shared_ptr<WindowsCaptureNativeCalls> native;
  std::function<void(const Error&)> failed;
  std::function<void(const char*,bool)> observe_call;
  std::optional<WindowsBorderlessAccess> borderless;
  std::optional<std::uint32_t> frame_timeout;
};
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
  // Original DISPLAYCONFIG_SDR_WHITE_LEVEL value; display notifications compare
  // this integer rather than a rounded luminance conversion.
  std::uint32_t sdr_white_level{};
  std::shared_ptr<const WindowsIccProfile> icc_profile;
  bool icc_profile_assumed_srgb{};
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
Result<WindowsDisplayInfo, Error> windows_query_display_state(
    std::uintptr_t monitor, DiagnosticsPort* diagnostics = nullptr);
// Read-only profile lookup, independent of the current HDR/ACM mode. Explicit
// WCS scope selection is authoritative; an unconfigured profile uses the DC's
// Windows default (which may be sRGB), never a hard-coded profile filename.
Result<std::string, Error> windows_effective_icc_path(
    const std::string& device_name, DiagnosticsPort* diagnostics = nullptr);
// Narrow query seam for deterministic fallback/error propagation tests.
using WindowsIccPathQuery = std::function<Result<std::string, Error>(const std::string&)>;
Result<std::string, Error> windows_effective_icc_path(const std::string& device_name,
    const WindowsIccPathQuery& configured_query, const WindowsIccPathQuery& dc_default_query);
using WindowsIccProfileLoader = std::function<Result<std::shared_ptr<const WindowsIccProfile>, Error>(const std::string&)>;
// Preparation policy only; preserve the actual queried path as provenance.
void windows_resolve_display_icc(WindowsDisplayInfo& display,
    DiagnosticsPort* diagnostics = nullptr, const WindowsIccPathQuery& query = {},
    const WindowsIccProfileLoader& loader = {});
Result<WindowsRawCapture, Error> windows_capture_display(
    const WindowsDisplayInfo& display, const std::atomic_bool* cancelled = nullptr,
    std::uint32_t timeout_ms = 5000, DiagnosticsPort* diagnostics = nullptr,
    const WindowsCaptureHooks* hooks = nullptr);
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
  bool busy() const;
  void shutdown();
  void diagnostic_snapshot();
  void set_displays(std::shared_ptr<const std::vector<WindowsDisplayInfo>> displays,
      WindowsBorderlessAccess access = {});
  using CaptureDisplay = std::function<Result<WindowsRawCapture, Error>(
      const WindowsDisplayInfo&, const std::atomic_bool*, const WindowsCaptureHooks&)>;
  void set_test_seams(WindowsNativeSubmit submit, CaptureDisplay capture = {},
      std::shared_ptr<WindowsCaptureNativeCalls> native = {});
 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
}  // namespace hdrshot
