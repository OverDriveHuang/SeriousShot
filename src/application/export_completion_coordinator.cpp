#include "application/export_completion_coordinator.hpp"
#include "application/export_diagnostics.hpp"
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

namespace hdrshot {
ExportCompletionCoordinator::ExportCompletionCoordinator(
    ExportCompletionPorts ports, DialogVisibility dialog_visibility, Event event,
    DiagnosticsPort* diagnostics)
    : ports_(ports), dialog_visibility_(std::move(dialog_visibility)), event_(std::move(event)),
      diagnostics_(diagnostics) {
  auto user_event = std::move(event_);
  event_ = [diagnostics, callback = std::move(user_event)](
      const ExportSnapshot& snapshot, UiCommand command, const char* stage, const Outcome* result) {
    if (diagnostics && diagnostics->detailed_logging()) {
      auto event = export_diagnostic(snapshot,
          command == UiCommand::copy_and_close ? "copy" :
          command == UiCommand::save_default ? "save" :
          command == UiCommand::save_as ? "save_as" : "unsupported", stage);
      event.outcome = result && !*result ? "failure" : "observed";
      if (result && !*result) {
        event.error_code = to_string(result->error().code);
        event.error_origin = result->error().origin;
        event.safe_context.insert(result->error().safe_context.begin(), result->error().safe_context.end());
      }
      (void)diagnostics->record(event);
    }
    if (callback) callback(snapshot, command, stage, result);
  };
}

bool ExportCompletionCoordinator::submit(
    const UiCommand command, ExportSnapshot snapshot, Completed completed) {
  // Both platform shells use this one terminal callback. Timing includes queue,
  // codec, destination publication and any save dialog; no pixel re-scan.
  if (diagnostics_) {
    const auto started = std::chrono::steady_clock::now();
    auto context = export_diagnostic(snapshot,
        command == UiCommand::copy_and_close ? "copy" :
        command == UiCommand::save_default ? "save" :
          command == UiCommand::save_as ? "save_as" : "unsupported", "complete");
    completed = [diagnostics = diagnostics_, started, context = std::move(context),
                 done = std::move(completed)](Outcome result) mutable {
      record_export_result(diagnostics, context, result,
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - started).count());
      done(std::move(result));
    };
  }
  const auto fail = [&](Error error) {
    auto result = Outcome::failure(std::move(error));
    if (event_) event_(snapshot, command, "rejected", &result);
    completed(std::move(result));
    return false;
  };
  if (command != UiCommand::copy_and_close && command != UiCommand::save_default &&
      command != UiCommand::save_as) {
    return fail(Error{ErrorCode::invalid_input, "ExportCompletionCoordinator",
                      Retryability::never, {{"reason", "invalid_completion_command"}}});
  }
  bool choose_destination = command == UiCommand::save_as;
  if (command == UiCommand::save_default) {
    const auto directory = ports_.files.prepare_directory(snapshot.default_folder);
    if (!directory || !directory.value()) {
      choose_destination = true;
      if (event_) event_(snapshot, command, "directory_fallback", nullptr);
    }
  }
  std::optional<ChooseSavePathOutcome> destination;
  if (choose_destination) {
    if (dialog_visibility_) dialog_visibility_(true, snapshot.target_display_id);
    auto chosen = ExportWorkflow::choose_save_as_path(
        snapshot.default_folder, ports_.clock, ports_.dialog,
        snapshot.target_display_id, snapshot.save_format);
    if (dialog_visibility_) dialog_visibility_(false, snapshot.target_display_id);
    if (!chosen) return fail(chosen.error());
    if (std::holds_alternative<UserCancelled>(chosen.value())) {
      auto result = ExportWorkflow::cancel(snapshot);
      if (event_) event_(snapshot, command, "cancelled", &result);
      completed(std::move(result));
      return false;
    }
    destination = std::move(chosen.value());
  }
  if (event_) event_(snapshot, command, "queued", nullptr);
  ports_.executor.background(
      [this, command, snapshot = std::move(snapshot),
       destination = std::move(destination), completed = std::move(completed)]() mutable {
    if (event_) event_(snapshot, command, "encode_started", nullptr);
    auto finish = [this, snapshot, command, completed](Outcome result) mutable {
      ports_.executor.main_thread(
          [this, snapshot, command, completed, result = std::move(result)]() mutable {
        if (event_) event_(snapshot, command, "publish", &result);
        completed(std::move(result));
      });
    };
    try {
      if (command == UiCommand::copy_and_close) {
        auto prepared = ExportWorkflow::prepare(snapshot, ports_.text, ports_.pixels,
            ports_.ultra_hdr_input, ports_.ultra_hdr_encoder, ports_.range);
        if (!prepared) { finish(Outcome::failure(prepared.error())); return; }
        auto owned = std::make_shared<PreparedExport>(std::move(prepared.value()));
        ports_.executor.main_thread([this, snapshot, command, completed, owned]() mutable {
          auto result = ExportWorkflow::copy_prepared(*owned, ports_.clipboard);
          if (event_) event_(snapshot, command, "publish", &result);
          completed(std::move(result));
        });
      } else if (destination) {
        finish(ExportWorkflow::save_as_to_destination(snapshot, *destination,
            ports_.files, ports_.text, ports_.pixels, ports_.ultra_hdr_input,
            ports_.ultra_hdr_encoder, ports_.range));
      } else {
        finish(ExportWorkflow::save_default(snapshot, ports_.clock, ports_.files,
            ports_.text, ports_.pixels, ports_.ultra_hdr_input,
            ports_.ultra_hdr_encoder, ports_.range));
      }
    } catch (const std::exception&) {
      finish(Outcome::failure(Error{ErrorCode::invalid_input,
          "ExportCompletionCoordinator", Retryability::after_user_action,
          {{"reason", "background_export_exception"}}}));
    }
  });
  return true;
}
}  // namespace hdrshot
