#pragma once

#include "platform/macos/screen_capture_kit_adapter.hpp"
#include "ports/capture_preview_ports.hpp"
#include "ports/diagnostics_port.hpp"

#include <memory>

namespace hdrshot {

class MacDisplayCatalogPort final : public DisplayCatalogPort {
 public:
  explicit MacDisplayCatalogPort(std::shared_ptr<DiagnosticsPort> diagnostics = {})
      : diagnostics_(std::move(diagnostics)) {}
  void snapshot_displays(const SnapshotDisplaysRequest& request, Completion completion) override;
 private:
  std::shared_ptr<DiagnosticsPort> diagnostics_;
};

class MacCapturePort final : public CapturePort {
 public:
  explicit MacCapturePort(std::shared_ptr<DiagnosticsPort> diagnostics = {});
  ~MacCapturePort() override;

  void capture(const CaptureBatchRequest& request, Completion completion) override;
  void cancel(SessionId session_id, OperationId operation_id) override;

 private:
  struct CancellationRegistry;
  std::shared_ptr<CancellationRegistry> cancellations_;
  std::shared_ptr<DiagnosticsPort> diagnostics_;
};

}  // namespace hdrshot
