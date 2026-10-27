#pragma once
#include "platform/windows/windows_capture.hpp"
namespace hdrshot {
struct WindowsPreparedCapture { std::shared_ptr<const std::vector<WindowsDisplayInfo>> displays; WindowsBorderlessAccess borderless; };
class WindowsCapturePreparationOwner {
 public:
  using Completion=std::function<void(Result<WindowsPreparedCapture,Error>)>;
  using Enumerate=std::function<Result<std::vector<WindowsDisplayInfo>,Error>()>;
  using Permission=std::function<WindowsBorderlessAccess()>;
  explicit WindowsCapturePreparationOwner(std::shared_ptr<DiagnosticsPort> diagnostics={},WindowsNativeSubmit submit={},Enumerate enumerate={},Permission permission={},std::shared_ptr<WindowsCaptureNativeCalls> native={});
  ~WindowsCapturePreparationOwner();
  bool prepare(SessionId,Completion);
  void cancel(SessionId); void shutdown(); bool busy() const; void diagnostic_snapshot();
 private: struct State;std::shared_ptr<State> state_;
};
}
