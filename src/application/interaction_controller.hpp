#pragma once

#include "application/input_mapper.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace hdrshot {

enum class UiPhase : std::uint8_t {
  idle,
  capturing,
  selecting,
  exporting,
  cancelling,
  recoverable_error,
  completed,
};

enum class ExportMode : std::uint8_t { copy, save_default, save_as };

struct UiState {
  UiPhase phase{UiPhase::idle};
  std::optional<SessionId> session_id;
  std::optional<OperationId> active_operation_id;
  DisplayGeneration display_generation{};
  std::optional<FrameId> frozen_frame_id;
  std::optional<Error> last_error;
  std::uint64_t stale_event_count{};
  std::optional<DisplayId> target_display_id;

  friend bool operator==(const UiState&, const UiState&) = default;
};

struct StartCaptureCommand {
  SessionId session_id;
  OperationId operation_id;
};

struct OverlayCommand {
  UiCommand command;
  OperationId operation_id;
};

struct CaptureReady {
  FrameId frame_id;
};

struct LockTargetDisplayCommand { DisplayId display_id; };

// The platform effect runner emits this after it has taken ownership of the
// immutable edit snapshot and queued the destination work. The Overlay closes
// at acceptance; the resident shell reports the later destination receipt.
struct ExportAccepted {};
struct ExportCancelled {};
struct OperationCancelled {};

struct ExportFailed {
  Error error;
};

using AppEvent = std::variant<
    CaptureReady,
    ExportAccepted,
    ExportCancelled,
    ExportFailed,
    OperationCancelled>;

struct AppEventEnvelope {
  SessionId session_id;
  OperationId operation_id;
  DisplayGeneration display_generation{};
  AppEvent event;
};

struct BeginCaptureEffect {
  SessionId session_id;
  OperationId operation_id;
};

struct ExportEffect {
  ExportMode mode;
  SessionId session_id;
  OperationId operation_id;
  FrameId frame_id;
};

struct CancelEffect {
  SessionId session_id;
  OperationId operation_id;
};

struct CloseOverlayEffect {};

using Effect = std::variant<BeginCaptureEffect, ExportEffect, CancelEffect, CloseOverlayEffect>;

struct ReduceResult {
  UiState state;
  std::vector<Effect> effects;
};

class InteractionController {
 public:
  [[nodiscard]] static ReduceResult reduce(const UiState& state, const StartCaptureCommand& command);
  [[nodiscard]] static ReduceResult reduce(const UiState& state, const OverlayCommand& command);
  [[nodiscard]] static ReduceResult reduce(const UiState& state, const AppEventEnvelope& event);
  [[nodiscard]] static ReduceResult reduce(const UiState& state, LockTargetDisplayCommand command);
};

}  // namespace hdrshot
