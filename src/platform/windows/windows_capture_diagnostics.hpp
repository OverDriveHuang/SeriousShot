#pragma once

#include "platform/windows/windows_capture_policy.hpp"
#include "ports/diagnostics_port.hpp"

#include <cstdint>
#include <charconv>
#include <limits>
#include <string>

namespace hdrshot {

inline std::string capture_adjustment_number(double value) {
  char buffer[64]{};
  const auto result=std::to_chars(buffer,buffer+sizeof(buffer),value,
      std::chars_format::general,std::numeric_limits<double>::max_digits10);
  return {buffer,result.ptr};
}

// Emits only fields already accepted by QtSessionDiagnosticsPort's fixed
// allowlist. Keeping the event construction here makes sink readback tests
// exercise precisely the same events as production capture workers.
inline void record_capture_adjustment_diagnostics(
    DiagnosticsPort* diagnostics, SessionId session_id, OperationId operation_id,
    std::uint64_t display_id, WindowsCaptureOptions options,
    WindowsCaptureAdjustment adjustment, double source_white_nits,
    float normal_scale, bool hdr_active) {
  const auto id=std::to_string(display_id);
  record_diagnostic_stage(diagnostics, session_id, operation_id,
      "windows.color", "capture.options", "success",
      {{"displayId", id}, {"scale", capture_adjustment_number(options.gain)},
       {"source", options.gain_enabled ? "gain_on" : "gain_off"},
       {"reason", options.bypass_sdr_white_enabled ? "white_bypass_on" : "white_bypass_off"}});
  record_diagnostic_stage(diagnostics, session_id, operation_id,
      "windows.color", "capture.gain", "success",
      {{"displayId", id}, {"scale", capture_adjustment_number(adjustment.effective_gain)},
       {"source", options.gain_enabled ? "gain_on" : "gain_off"}});
  record_diagnostic_stage(diagnostics, session_id, operation_id,
      "windows.color", "capture.white_adjustment", "success",
      {{"displayId", id}, {"diffuseWhite", capture_adjustment_number(source_white_nits)},
       {"scale", capture_adjustment_number(normal_scale)},
       {"source", options.bypass_sdr_white_enabled ? "white_bypass_on" : "white_bypass_off"}});
  record_diagnostic_stage(diagnostics, session_id, operation_id,
      "windows.color", "capture.white_applied", "success",
      {{"displayId", id}, {"scale", capture_adjustment_number(adjustment.white_scale)},
       {"reason", hdr_active ? "hdr" : "sdr"}});
}

}  // namespace hdrshot
