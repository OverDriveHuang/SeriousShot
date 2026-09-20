#pragma once
#include "application/interaction_controller.hpp"
#include "ports/capture_preview_ports.hpp"

namespace hdrshot {
// Resident, UI-thread-owned session policy; native shells only execute effects.
// Export jobs own separate immutable snapshots and may outlive this session.
class CaptureInteractionSession {
 public:
  [[nodiscard]] bool begin(SessionId id) {
    if (active() || id.value == 0) return false;
    state_ = InteractionController::reduce(state_, StartCaptureCommand{id, OperationId{1}}).state;
    return active();
  }
  [[nodiscard]] bool active() const { return state_.session_id.has_value(); }
  [[nodiscard]] SessionId id() const { return state_.session_id.value_or(SessionId{}); }
  [[nodiscard]] bool accepts(SessionId id) const { return active() && state_.session_id == id; }
  [[nodiscard]] bool ready(const PresentReceipt& receipt) {
    if (!accepts(receipt.session_id)) return false;
    state_ = InteractionController::reduce(state_, AppEventEnvelope{
        receipt.session_id, receipt.operation_id, receipt.display_generation,
        CaptureReady{receipt.frame_id}}).state;
    return state_.phase == UiPhase::selecting;
  }
  [[nodiscard]] bool allows(SessionId id, DisplayId display) const {
    return accepts(id) && state_.phase == UiPhase::selecting && display.value != 0 &&
        (!gesture_owner_ || gesture_owner_ == display) &&
        (!state_.target_display_id || state_.target_display_id == display);
  }
  [[nodiscard]] bool initial_gesture(SessionId id, DisplayId display, bool begin) {
    if (!begin) {
      if (!accepts(id) || gesture_owner_ != display) return false;
      gesture_owner_.reset();
      return true;
    }
    if (!allows(id, display)) return false;
    gesture_owner_ = display;
    return true;
  }
  [[nodiscard]] bool lock(SessionId id, DisplayId display) {
    if (!allows(id, display)) return false;
    state_ = InteractionController::reduce(state_, LockTargetDisplayCommand{display}).state;
    return state_.target_display_id == display;
  }
  [[nodiscard]] bool finish(SessionId id) {
    if (!accepts(id)) return false;
    gesture_owner_.reset();
    const auto operation = *state_.active_operation_id;
    state_ = InteractionController::reduce(state_, OverlayCommand{
        UiCommand::cancel_capture, operation}).state;
    state_ = InteractionController::reduce(state_, AppEventEnvelope{
        id, operation, state_.display_generation, OperationCancelled{}}).state;
    return !active();
  }
 private:
  UiState state_;
  std::optional<DisplayId> gesture_owner_;
};
} // namespace hdrshot
