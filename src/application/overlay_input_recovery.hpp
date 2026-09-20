#pragma once

namespace hdrshot {
enum class RecoveryPointerEvent { press, move, release, double_click };
enum class RecoveryPointerAction { forward, consume, restore_focus, ordinary_press };

// One UI-thread state per capture session, shared by every display. Native
// activation BEFORE delivery of the first press must not erase the interruption.
class OverlayInputRecovery {
 public:
  void interrupt() noexcept {
    pending_ = true;
    // The OS can take the mouse grab away without delivering the old release.
    // Do not wait forever for that release; the next gesture restores input.
    swallowing_ = false;
  }
  [[nodiscard]] bool pending() const noexcept { return pending_; }
  [[nodiscard]] bool swallowing() const noexcept { return swallowing_; }
  [[nodiscard]] RecoveryPointerAction pointer(RecoveryPointerEvent event) noexcept {
    if (swallowing_) {
      if (event == RecoveryPointerEvent::release) swallowing_ = false;
      return RecoveryPointerAction::consume;
    }
    if (event == RecoveryPointerEvent::press || event == RecoveryPointerEvent::double_click) {
      if (pending_) {
        pending_ = false;
        swallowing_ = true;
        isolate_double_click_ = true;
        return RecoveryPointerAction::restore_focus;
      }
      const bool was_recovery = isolate_double_click_;
      isolate_double_click_ = false;
      if (was_recovery && event == RecoveryPointerEvent::double_click)
        return RecoveryPointerAction::ordinary_press;
    }
    return pending_ ? RecoveryPointerAction::consume : RecoveryPointerAction::forward;
  }
 private:
  bool pending_{false};
  bool swallowing_{false};
  bool isolate_double_click_{false};
};
} // namespace hdrshot
