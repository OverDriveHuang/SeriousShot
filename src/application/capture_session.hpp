#pragma once

#include "core/error.hpp"
#include "core/frame.hpp"
#include "core/result.hpp"
#include "ports/capture_preview_ports.hpp"
#include "ports/window_catalog_port.hpp"
#include "ports/diagnostics_port.hpp"

#include <functional>
#include <memory>
#include <mutex>

namespace hdrshot {

struct BeginCaptureRequest {
  SessionId session_id{};
  OperationId operation_id{};
  FrameId frame_id{};
  std::vector<DisplayId> targets;
  SelectionSnapshot initial_selection{};
};

struct CaptureReadyPayload {
  FrozenDesktopRef frozen_desktop;
  PresentReceipt present_receipt;
  WindowSnapshotRef window_snapshot{};
};

class CaptureSession {
 public:
  using Completion = std::function<void(Result<CaptureReadyPayload, Error>)>;

  CaptureSession(
      DisplayCatalogPort& display_catalog,
      CapturePort& capture_port,
      PreviewPresenterPort& presenter,
      WindowCatalogPort* window_catalog = nullptr);

  CaptureSession(std::shared_ptr<DisplayCatalogPort> display_catalog,
                 std::shared_ptr<CapturePort> capture_port,
                 std::shared_ptr<PreviewPresenterPort> presenter,
                 std::shared_ptr<WindowCatalogPort> window_catalog = {},
                 std::shared_ptr<DiagnosticsPort> diagnostics = {});
  ~CaptureSession();
  CaptureSession(const CaptureSession&) = delete;
  CaptureSession& operator=(const CaptureSession&) = delete;

  void begin(BeginCaptureRequest request, Completion completion);
  void cancel(SessionId session_id, OperationId operation_id);

 private:
  struct State;
  std::shared_ptr<State> state_;
};

}  // namespace hdrshot
