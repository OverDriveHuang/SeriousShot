#pragma once

#include "platform/macos/screen_capture_kit_adapter.hpp"
#include "ports/capture_preview_ports.hpp"
#include "ports/diagnostics_port.hpp"

#include <memory>

namespace hdrshot {

class MacDisplayCatalogPort final : public DisplayCatalogPort {
 public:
  using Enumerate = std::function<void(MacScreenCaptureKitAdapter::DisplayListCompletion,
      std::function<bool()>, std::function<void(const char*, const char*)>)>;
  explicit MacDisplayCatalogPort(std::shared_ptr<DiagnosticsPort> diagnostics = {}, Enumerate enumerate = {});
  ~MacDisplayCatalogPort() override;
  void snapshot_displays(const SnapshotDisplaysRequest& request, Completion completion) override;
  void cancel(SessionId session_id, OperationId operation_id) override;
  void diagnostic_snapshot() const;
 private:
  struct State;
  std::shared_ptr<State> state_;
  Enumerate enumerate_;
  std::shared_ptr<DiagnosticsPort> diagnostics_;
};

class MacCapturePort final : public CapturePort {
 public:
  using CaptureDisplay = std::function<void(std::uint32_t, MacScreenCaptureKitAdapter::CaptureCompletion, MacCaptureHooks)>;
  // Accept-and-return, or throw without enqueueing. No ambiguous submission result.
  using NativeSubmit = std::function<void(std::function<void()>)>;
  explicit MacCapturePort(std::shared_ptr<DiagnosticsPort> diagnostics = {},
      CaptureDisplay capture_display = {}, NativeSubmit submit = {});
  ~MacCapturePort() override;

  void capture(const CaptureBatchRequest& request, Completion completion) override;
  void cancel(SessionId session_id, OperationId operation_id) override;

  bool busy() const;
  void shutdown();
  void diagnostic_snapshot() const;
 private:
  struct State;
  std::shared_ptr<State> state_;
  CaptureDisplay capture_display_;
  NativeSubmit submit_;
  std::shared_ptr<DiagnosticsPort> diagnostics_;
};

}  // namespace hdrshot
