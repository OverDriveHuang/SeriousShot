#include "application/interaction_controller.hpp"
#include "test_support.hpp"

namespace {

using namespace hdrshot;

UiState selecting_state() {
  return UiState{
      UiPhase::selecting,
      SessionId{1},
      OperationId{10},
      7,
      FrameId{100},
      std::nullopt,
      0,
  };
}

void start_capture_creates_one_effect() {
  const auto result = InteractionController::reduce(
      UiState{},
      StartCaptureCommand{SessionId{1}, OperationId{10}});
  HDRSHOT_CHECK(result.state.phase == UiPhase::capturing);
  HDRSHOT_CHECK(result.state.session_id == SessionId{1});
  HDRSHOT_CHECK(result.effects.size() == 1);
  HDRSHOT_CHECK(std::holds_alternative<BeginCaptureEffect>(result.effects.front()));
}

void capture_ready_opens_selection() {
  auto state = InteractionController::reduce(
      UiState{},
      StartCaptureCommand{SessionId{1}, OperationId{10}}).state;
  const auto result = InteractionController::reduce(
      state,
      AppEventEnvelope{SessionId{1}, OperationId{10}, 7, CaptureReady{FrameId{100}}});
  HDRSHOT_CHECK(result.state.phase == UiPhase::selecting);
  HDRSHOT_CHECK(result.state.frozen_frame_id == FrameId{100});
  HDRSHOT_CHECK(result.state.display_generation == 7);
}

void copy_closes_after_background_queue_accepts_snapshot() {
  const auto exporting = InteractionController::reduce(
      selecting_state(),
      OverlayCommand{UiCommand::copy_and_close, OperationId{11}});
  HDRSHOT_CHECK(exporting.state.phase == UiPhase::exporting);
  HDRSHOT_CHECK(exporting.effects.size() == 1);
  HDRSHOT_CHECK(std::holds_alternative<ExportEffect>(exporting.effects.front()));
  HDRSHOT_CHECK(std::get<ExportEffect>(exporting.effects.front()).mode == ExportMode::copy);

  const auto accepted = InteractionController::reduce(
      exporting.state,
      AppEventEnvelope{SessionId{1}, OperationId{11}, 7, ExportAccepted{}});
  HDRSHOT_CHECK(accepted.state.phase == UiPhase::completed);
  HDRSHOT_CHECK(accepted.effects.size() == 1);
  HDRSHOT_CHECK(std::holds_alternative<CloseOverlayEffect>(accepted.effects.front()));
}

void export_dispatch_failure_preserves_frame_for_retry() {
  const auto exporting = InteractionController::reduce(
      selecting_state(),
      OverlayCommand{UiCommand::save_default, OperationId{12}}).state;
  const Error error{
      ErrorCode::state_inconsistent,
      "BackgroundExportQueue",
      Retryability::after_recreate,
      {}};
  const auto failed = InteractionController::reduce(
      exporting,
      AppEventEnvelope{SessionId{1}, OperationId{12}, 7, ExportFailed{error}});
  HDRSHOT_CHECK(failed.state.phase == UiPhase::recoverable_error);
  HDRSHOT_CHECK(failed.state.frozen_frame_id == FrameId{100});
  HDRSHOT_CHECK(failed.state.last_error == error);
  HDRSHOT_CHECK(failed.effects.empty());

  const auto retry = InteractionController::reduce(
      failed.state,
      OverlayCommand{UiCommand::save_default, OperationId{15}});
  HDRSHOT_CHECK(retry.state.phase == UiPhase::exporting);
  HDRSHOT_CHECK(retry.state.active_operation_id == OperationId{15});
  HDRSHOT_CHECK(!retry.state.last_error.has_value());
  HDRSHOT_CHECK(retry.effects.size() == 1);
}

void save_as_cancel_returns_to_selection() {
  const auto exporting = InteractionController::reduce(
      selecting_state(),
      OverlayCommand{UiCommand::save_as, OperationId{13}}).state;
  const auto cancelled = InteractionController::reduce(
      exporting,
      AppEventEnvelope{SessionId{1}, OperationId{13}, 7, ExportCancelled{}});
  HDRSHOT_CHECK(cancelled.state.phase == UiPhase::selecting);
  HDRSHOT_CHECK(cancelled.state.frozen_frame_id == FrameId{100});
  HDRSHOT_CHECK(!cancelled.state.last_error.has_value());
}

void stale_event_only_increments_diagnostic_count() {
  const auto state = selecting_state();
  const auto result = InteractionController::reduce(
      state,
      AppEventEnvelope{SessionId{99}, OperationId{10}, 7, CaptureReady{FrameId{999}}});
  HDRSHOT_CHECK(result.state.phase == state.phase);
  HDRSHOT_CHECK(result.state.frozen_frame_id == state.frozen_frame_id);
  HDRSHOT_CHECK(result.state.stale_event_count == 1);
  HDRSHOT_CHECK(result.effects.empty());

  const auto wrong_generation = InteractionController::reduce(
      state,
      AppEventEnvelope{SessionId{1}, OperationId{10}, 999, CaptureReady{FrameId{999}}});
  HDRSHOT_CHECK(wrong_generation.state.frozen_frame_id == state.frozen_frame_id);
  HDRSHOT_CHECK(wrong_generation.state.stale_event_count == 1);
}

void cancel_requires_matching_receipt() {
  const auto cancelling = InteractionController::reduce(
      selecting_state(),
      OverlayCommand{UiCommand::cancel_capture, OperationId{14}});
  HDRSHOT_CHECK(cancelling.state.phase == UiPhase::cancelling);
  HDRSHOT_CHECK(std::holds_alternative<CancelEffect>(cancelling.effects.front()));
  const auto idle = InteractionController::reduce(
      cancelling.state,
      AppEventEnvelope{SessionId{1}, OperationId{14}, 7, OperationCancelled{}});
  HDRSHOT_CHECK(idle.state.phase == UiPhase::idle);
  HDRSHOT_CHECK(!idle.state.session_id.has_value());
  HDRSHOT_CHECK(!idle.state.frozen_frame_id.has_value());
}

}  // namespace

int main() {
  return hdrshot::test::run({
      {"A1-STATE-001", start_capture_creates_one_effect},
      {"A1-CAPTURE-READY-001", capture_ready_opens_selection},
      {"A1-STATE-002/003", copy_closes_after_background_queue_accepts_snapshot},
      {"A1-RECOVERY-001", export_dispatch_failure_preserves_frame_for_retry},
      {"A1-RECOVERY-002", save_as_cancel_returns_to_selection},
      {"A1-STALE-001", stale_event_only_increments_diagnostic_count},
      {"A1-CANCEL-001", cancel_requires_matching_receipt},
  });
}
