#include "application/interaction_controller.hpp"

#include <type_traits>

namespace hdrshot {
ReduceResult InteractionController::reduce(const UiState& state, LockTargetDisplayCommand command) {
  if (state.phase != UiPhase::selecting || command.display_id.value == 0 ||
      (state.target_display_id && *state.target_display_id != command.display_id)) {
    return {state, {}};
  }
  auto next = state;
  next.target_display_id = command.display_id;
  return {next, {}};
}

namespace {

[[nodiscard]] bool event_matches(const UiState& state, const AppEventEnvelope& envelope) {
  const auto generation_matches = state.display_generation == 0 ||
      state.display_generation == envelope.display_generation;
  return generation_matches && state.session_id.has_value() && state.active_operation_id.has_value() &&
      state.session_id.value() == envelope.session_id &&
      state.active_operation_id.value() == envelope.operation_id;
}

[[nodiscard]] std::optional<ExportMode> export_mode(const UiCommand command) {
  switch (command) {
    case UiCommand::copy_and_close:
      return ExportMode::copy;
    case UiCommand::save_default:
      return ExportMode::save_default;
    case UiCommand::save_as:
      return ExportMode::save_as;
    case UiCommand::cancel_capture:
    case UiCommand::undo:
    case UiCommand::redo:
    case UiCommand::analyze:
      return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace

ReduceResult InteractionController::reduce(
    const UiState& state,
    const StartCaptureCommand& command) {
  if (state.phase != UiPhase::idle && state.phase != UiPhase::completed) {
    return ReduceResult{state, {}};
  }
  auto next = UiState{};
  next.phase = UiPhase::capturing;
  next.session_id = command.session_id;
  next.active_operation_id = command.operation_id;
  return ReduceResult{
      next,
      {BeginCaptureEffect{command.session_id, command.operation_id}},
  };
}

ReduceResult InteractionController::reduce(const UiState& state, const OverlayCommand& command) {
  if (!state.session_id.has_value()) {
    return ReduceResult{state, {}};
  }

  if (command.command == UiCommand::cancel_capture &&
      state.phase != UiPhase::idle && state.phase != UiPhase::completed) {
    auto next = state;
    next.phase = UiPhase::cancelling;
    next.active_operation_id = command.operation_id;
    next.last_error.reset();
    return ReduceResult{
        next,
        {CancelEffect{state.session_id.value(), command.operation_id}},
    };
  }

  const auto mode = export_mode(command.command);
  if ((state.phase != UiPhase::selecting && state.phase != UiPhase::recoverable_error) ||
      !mode.has_value() || !state.frozen_frame_id.has_value()) {
    return ReduceResult{state, {}};
  }

  auto next = state;
  next.phase = UiPhase::exporting;
  next.active_operation_id = command.operation_id;
  next.last_error.reset();
  return ReduceResult{
      next,
      {ExportEffect{
          mode.value(),
          state.session_id.value(),
          command.operation_id,
          state.frozen_frame_id.value(),
      }},
  };
}

ReduceResult InteractionController::reduce(const UiState& state, const AppEventEnvelope& envelope) {
  if (!event_matches(state, envelope)) {
    auto next = state;
    ++next.stale_event_count;
    return ReduceResult{next, {}};
  }

  return std::visit(
      [&state, &envelope](const auto& event) -> ReduceResult {
        using Event = std::decay_t<decltype(event)>;
        if constexpr (std::is_same_v<Event, CaptureReady>) {
          if (state.phase != UiPhase::capturing) {
            return ReduceResult{state, {}};
          }
          auto next = state;
          next.phase = UiPhase::selecting;
          next.display_generation = envelope.display_generation;
          next.frozen_frame_id = event.frame_id;
          next.last_error.reset();
          return ReduceResult{next, {}};
        } else if constexpr (std::is_same_v<Event, ExportAccepted>) {
          if (state.phase != UiPhase::exporting) {
            return ReduceResult{state, {}};
          }
          auto next = state;
          next.phase = UiPhase::completed;
          next.last_error.reset();
          return ReduceResult{next, {CloseOverlayEffect{}}};
        } else if constexpr (std::is_same_v<Event, ExportFailed>) {
          if (state.phase != UiPhase::exporting) {
            return ReduceResult{state, {}};
          }
          auto next = state;
          next.phase = UiPhase::recoverable_error;
          next.last_error = event.error;
          return ReduceResult{next, {}};
        } else if constexpr (std::is_same_v<Event, ExportCancelled>) {
          if (state.phase != UiPhase::exporting) {
            return ReduceResult{state, {}};
          }
          auto next = state;
          next.phase = UiPhase::selecting;
          next.last_error.reset();
          return ReduceResult{next, {}};
        } else {
          if (state.phase != UiPhase::cancelling) {
            return ReduceResult{state, {}};
          }
          auto next = UiState{};
          next.stale_event_count = state.stale_event_count;
          return ReduceResult{next, {}};
        }
      },
      envelope.event);
}

}  // namespace hdrshot
