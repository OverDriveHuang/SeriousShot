#pragma once

#include "core/error.hpp"
#include "core/result.hpp"
#include "domain/geometry/window_selection.hpp"

#include <functional>

namespace hdrshot {
struct SnapshotWindowsRequest {
  SessionId session_id{};
  OperationId operation_id{};
  DisplaySnapshotSet displays;
};
// Metadata only. Implementations must finish once with a finite timeout, never
// ask for new permissions, and must permit cancellation without waiting for OS.
// Failure is a manual-selection fallback, not failure of the captured image.
class WindowCatalogPort {
 public:
  using Completion = std::function<void(Result<WindowSnapshot, Error>)>;
  virtual ~WindowCatalogPort() = default;
  virtual void snapshot_windows(const SnapshotWindowsRequest&, Completion) = 0;
  virtual void cancel(SessionId, OperationId) = 0;
};
} // namespace hdrshot
