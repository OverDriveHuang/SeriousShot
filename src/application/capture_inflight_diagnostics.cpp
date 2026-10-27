#include "application/capture_inflight_diagnostics.hpp"
namespace hdrshot {
CaptureInFlightDiagnostics::CaptureInFlightDiagnostics(CaptureOperationKey key,
    std::shared_ptr<DiagnosticsPort> sink, std::vector<DisplayId> targets)
    : key_(key), sink_(std::move(sink)), started_(std::chrono::steady_clock::now()),
      targets_(targets.begin(), targets.end()) {}
void CaptureInFlightDiagnostics::write(DiagnosticEvent event) noexcept {
  try {
    if (!sink_) return;
    event.session_id = key_.session_id; event.operation_id = key_.operation_id;
    event.subsystem = "capture"; event.detail_only = true;
    event.safe_context["generation"] = std::to_string(key_.display_generation);
    if (!sink_->record(event)) write_failed_.store(true);
  } catch (...) { write_failed_.store(true); }
}
void CaptureInFlightDiagnostics::stage(DisplayId display, const char* api, const char* stage_name) noexcept {
  try {
    const auto now = std::chrono::steady_clock::now();
    DiagnosticEvent event; event.stage = stage_name; event.outcome = "observed";
    {
      std::lock_guard lock(mutex_);
      auto& entry = entries_[display];
      const auto elapsed = entry.api == api && entry.since != CaptureSteadyTime{} ?
          std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.since).count() : 0;
      if (std::string(stage_name) == "cancel_requested") cancelled_ = true;
      else {
        if (entry.api != api || std::string(stage_name) == "call_enter" || entry.since == CaptureSteadyTime{}) entry.since = now;
        entry.api = api; entry.stage = stage_name;
      }
      event.safe_context = {{"displayId", std::to_string(display.value)}, {"api", api},
          {"elapsedMs", std::to_string(elapsed)},
          {"batchElapsedMs", std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now - started_).count())},
          {"pending", std::to_string(targets_.size() - received_.size())}, {"received", std::to_string(received_.size())}};
    }
    write(std::move(event));
  } catch (...) { write_failed_.store(true); }
}
void CaptureInFlightDiagnostics::image_received(DisplayId display) noexcept {
  try { std::lock_guard lock(mutex_); if (targets_.contains(display)) received_.insert(display); }
  catch (...) { write_failed_.store(true); }
}
void CaptureInFlightDiagnostics::failure(DisplayId display, const Error& error, const char* stage_name) noexcept {
  try {
    DiagnosticEvent event; event.stage = stage_name; event.outcome = "failure";
    event.error_code = to_string(error.code); event.error_origin = error.origin; event.safe_context = error.safe_context;
    { std::lock_guard lock(mutex_); const auto found = entries_.find(display);
      if (!event.safe_context.contains("api") && found != entries_.end()) event.safe_context["api"] = found->second.api;
    }
    event.safe_context["displayId"] = std::to_string(display.value); write(std::move(event));
  } catch (...) { write_failed_.store(true); }
}
void CaptureInFlightDiagnostics::snapshot() noexcept {
  try {
    std::vector<DiagnosticEvent> events; const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard lock(mutex_);
      for (const auto& [display, entry] : entries_) {
        DiagnosticEvent event; event.stage = "inflight_snapshot"; event.outcome = entry.stage;
        event.safe_context = {{"displayId", std::to_string(display.value)}, {"api", entry.api},
            {"elapsedMs", std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.since).count())},
            {"batchElapsedMs", std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now - started_).count())},
            {"pending", std::to_string(targets_.size() - received_.size())}, {"received", std::to_string(received_.size())}};
        if (cancelled_) event.safe_context["reason"] = "capture_draining";
        events.push_back(std::move(event));
      }
    }
    for (auto& event : events) write(std::move(event));
  } catch (...) { write_failed_.store(true); }
}
}
