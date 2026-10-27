#pragma once
#include "ports/capture_progress_port.hpp"
#include <memory>
namespace hdrshot {
// GUI owns this object; cross-thread clients only own handle(), a pure C++ mailbox.
class QtCaptureRuntime {
 public:
  QtCaptureRuntime();
  ~QtCaptureRuntime();
  std::shared_ptr<CaptureRuntimePort> handle() const;
  void shutdown();
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
