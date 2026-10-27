#include "application/capture_progress_watchdog.hpp"
namespace hdrshot {
CaptureProgressWatchdog::CaptureProgressWatchdog(CaptureOperationKey key,
    std::function<CaptureSteadyTime()> now) : now_(std::move(now)) { state_.key = key; }
CaptureProgressWatchdog::Snapshot CaptureProgressWatchdog::snapshot() const {
  std::lock_guard lock(mutex_); return state_;
}
CaptureProgressWatchdog::Action CaptureProgressWatchdog::terminate(Phase phase, std::optional<Error> error) {
  state_.phase = phase; state_.error = std::move(error); state_.deadline.reset(); ++state_.epoch;
  if (phase != Phase::succeeded) stopped_.store(true, std::memory_order_release);
  return {true, state_};
}
CaptureProgressWatchdog::Action CaptureProgressWatchdog::timeout() {
  return terminate(Phase::timed_out, Error{ErrorCode::capture_failed, "CaptureProgressWatchdog",
      Retryability::after_user_action, {{"reason", "capture_progress_timeout"}}});
}
CaptureProgressWatchdog::Action CaptureProgressWatchdog::start(std::vector<DisplayId> targets) {
  std::lock_guard lock(mutex_);
  if (state_.phase != Phase::not_started) return {false, state_};
  targets_.insert(targets.begin(), targets.end());
  if (targets.empty() || targets_.size() != targets.size())
    return terminate(Phase::failed, Error{ErrorCode::invalid_input, "CaptureProgressWatchdog",
        Retryability::never, {{"reason", "invalid_display_targets"}}});
  pending_ = targets_; state_.pending = pending_.size(); state_.phase = Phase::waiting_native;
  state_.batch_started = state_.last_progress = now_();
  state_.deadline = state_.last_progress + kCaptureNoProgressWindow; ++state_.epoch;
  return {true, state_};
}
CaptureProgressWatchdog::Action CaptureProgressWatchdog::receive(const CaptureNativeEvent& event) {
  std::lock_guard lock(mutex_);
  if (state_.terminal() || event.key != state_.key || !targets_.contains(event.display_id) ||
      (event.kind == CaptureNativeEventKind::failed && !event.error) ||
      (event.kind == CaptureNativeEventKind::image_received && (event.error || !pending_.contains(event.display_id))))
    return {false, state_};
  const auto now = now_();
  if (state_.deadline && now >= *state_.deadline) return timeout();
  if (event.kind == CaptureNativeEventKind::failed) return terminate(Phase::failed, event.error);
  pending_.erase(event.display_id); state_.pending = pending_.size(); ++state_.received;
  state_.last_progress = now; ++state_.epoch;
  if (pending_.empty()) { state_.deadline.reset(); state_.phase = Phase::processing; }
  else state_.deadline = now + kCaptureNoProgressWindow;
  return {true, state_};
}
CaptureProgressWatchdog::Action CaptureProgressWatchdog::check_deadline(std::uint64_t epoch) {
  std::lock_guard lock(mutex_);
  if (epoch != state_.epoch || !state_.deadline) return {false, state_};
  if (now_() >= *state_.deadline) return timeout();
  return {true, state_};
}
CaptureProgressWatchdog::Action CaptureProgressWatchdog::finish(std::optional<Error> error) {
  std::lock_guard lock(mutex_);
  if (state_.terminal()) return {false, state_};
  if (state_.deadline && now_() >= *state_.deadline) return timeout();
  if (error) return terminate(error->code == ErrorCode::operation_cancelled ? Phase::cancelled : Phase::failed, error);
  if (state_.phase != Phase::processing) return {false, state_};
  return terminate(Phase::succeeded, {});
}
}
