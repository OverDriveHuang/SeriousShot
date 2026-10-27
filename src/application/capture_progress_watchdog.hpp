#pragma once
#include "ports/capture_progress_port.hpp"
#include <atomic>
#include <mutex>
#include <set>
#include <vector>
namespace hdrshot {
class CaptureProgressWatchdog {
 public:
  static constexpr auto kCaptureNoProgressWindow = std::chrono::milliseconds(2000);
  enum class Phase { not_started, waiting_native, processing, succeeded, failed, timed_out, cancelled };
  struct Snapshot {
    CaptureOperationKey key; Phase phase{Phase::not_started};
    std::size_t pending{}, received{}; CaptureSteadyTime batch_started{}, last_progress{};
    std::optional<CaptureSteadyTime> deadline; std::uint64_t epoch{};
    std::optional<Error> error;
    bool terminal() const { return phase >= Phase::succeeded; }
  };
  struct Action { bool changed{}; Snapshot snapshot; };
  CaptureProgressWatchdog(CaptureOperationKey key, std::function<CaptureSteadyTime()> now);
  Action start(std::vector<DisplayId> targets);
  Action receive(const CaptureNativeEvent& event);
  Action check_deadline(std::uint64_t epoch);
  Action finish(std::optional<Error> error);
  Snapshot snapshot() const;
  bool stop_requested() const noexcept { return stopped_.load(std::memory_order_acquire); }
 private:
  Action terminate(Phase phase, std::optional<Error> error);
  Action timeout();
  mutable std::mutex mutex_; Snapshot state_; std::set<DisplayId> targets_, pending_;
  std::function<CaptureSteadyTime()> now_; std::atomic_bool stopped_{};
};
}
