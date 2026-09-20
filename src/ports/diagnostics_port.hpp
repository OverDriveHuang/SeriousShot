#pragma once

#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/result.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <optional>
#include <utility>

namespace hdrshot {

struct DiagnosticEvent {
  SessionId session_id{};
  OperationId operation_id{};
  std::string subsystem;
  std::string command;
  std::string stage;
  std::string outcome;
  std::string error_code;
  std::map<std::string, std::string> safe_context;
  std::string destination_name;
  std::uint64_t bytes{};
  std::optional<std::source_location> error_origin{};
};

struct DiagnosticReceipt {
  std::uint64_t sequence{};
};

class DiagnosticsPort {
 public:
  virtual ~DiagnosticsPort() = default;
  virtual void set_detailed_logging(bool) {}
  [[nodiscard]] virtual bool detailed_logging() const { return false; }
  [[nodiscard]] virtual Result<DiagnosticReceipt, Error> record(
      const DiagnosticEvent& event) = 0;
};

// Common event policy; native adapters only supply safe factual fields.
inline void record_diagnostic_stage(DiagnosticsPort* port,
    SessionId session, OperationId operation, const std::string& subsystem,
    const std::string& stage, const std::string& outcome,
    std::map<std::string, std::string> context = {}, const Error* error = nullptr) {
  if (!port || (!error && !port->detailed_logging())) return;
  DiagnosticEvent event{};
  event.session_id = session;
  event.operation_id = operation;
  event.subsystem = subsystem;
  event.stage = stage;
  event.outcome = outcome;
  event.safe_context = std::move(context);
  if (error) {
    event.error_code = to_string(error->code);
    event.error_origin = error->origin;
    event.safe_context.insert(error->safe_context.begin(), error->safe_context.end());
  }
  (void)port->record(event);
}

// Stable error code only; native context, paths and image data stay out of the
// default log. Hosts may use this for startup/capture failures, not exports
// already recorded by ExportCompletionCoordinator.
inline void record_diagnostic_failure(DiagnosticsPort* port,
    const std::string& subsystem, const Error& error) {
  if (!port) return;
  DiagnosticEvent event{};
  event.subsystem = subsystem;
  event.outcome = "failure";
  event.error_code = to_string(error.code);
  event.error_origin = error.origin;
  event.safe_context = error.safe_context;
  (void)port->record(event);
}

}  // namespace hdrshot
