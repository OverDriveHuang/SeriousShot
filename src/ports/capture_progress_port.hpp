#pragma once
#include "core/error.hpp"
#include "core/ids.hpp"
#include <chrono>
#include <functional>
#include <optional>
namespace hdrshot {
struct CaptureOperationKey {
  SessionId session_id{}; OperationId operation_id{}; DisplayGeneration display_generation{};
  friend bool operator==(const CaptureOperationKey&, const CaptureOperationKey&) = default;
};
enum class CaptureNativeEventKind { image_received, failed };
struct CaptureNativeEvent {
  CaptureOperationKey key; DisplayId display_id{}; CaptureNativeEventKind kind{};
  std::optional<Error> error;
};
class CaptureProgressObserver {
 public:
  virtual ~CaptureProgressObserver() = default;
  virtual void native_event(CaptureNativeEvent event) noexcept = 0;
  virtual bool stop_requested() const noexcept = 0;
};
using CaptureSteadyTime = std::chrono::steady_clock::time_point;
class CaptureRuntimePort {
 public:
  using Task = std::function<void()>; using TimerId = std::uint64_t;
  virtual ~CaptureRuntimePort() = default;
  virtual CaptureSteadyTime now() const noexcept = 0;
  // Always queued. False means closed; no task is retained on rejection.
  virtual bool post(Task task) = 0;
  // Timer effects are owner-thread operations. Native observers only use now/post.
  virtual void arm(TimerId id, CaptureSteadyTime due, Task task) = 0;
  virtual void disarm(TimerId id) = 0;
};
}
