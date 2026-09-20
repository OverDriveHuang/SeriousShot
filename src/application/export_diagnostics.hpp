#pragma once
#include "application/export_workflow.hpp"
#include "ports/diagnostics_port.hpp"

namespace hdrshot {
inline DiagnosticEvent export_diagnostic(const ExportSnapshot& snapshot,
    const std::string& command, const std::string& stage) {
  DiagnosticEvent event{};
  event.session_id = snapshot.session_id;
  event.operation_id = snapshot.operation_id;
  event.subsystem = "export"; event.command = command; event.stage = stage;
  event.safe_context = {
      {"format", snapshot.save_format == SaveFormat::ultra_hdr_jpeg ? "JPEG" : "PNG"},
      {"displayId", std::to_string(snapshot.target_display_id.value)},
      {"x", std::to_string(snapshot.selection.desktop_rect.x)},
      {"y", std::to_string(snapshot.selection.desktop_rect.y)},
      {"width", std::to_string(snapshot.selection.desktop_rect.width)},
      {"height", std::to_string(snapshot.selection.desktop_rect.height)},
      {"diffuseWhite", std::to_string(pq_diffuse_white_nits(snapshot.pq_diffuse_white))},
      {"precision", std::to_string(hdr_pq_precision_bits(snapshot.hdr_pq_precision))},
      {"quality", std::to_string(static_cast<unsigned>(snapshot.ultra_hdr_jpeg_quality))}};
  return event;
}

inline void record_export_result(DiagnosticsPort* diagnostics, DiagnosticEvent event,
    const Result<ExportReceipt, Error>& result, std::int64_t elapsed_ms) {
  if (!diagnostics) return;
  event.stage = "complete";
  event.outcome = !result ? "failure" :
      std::holds_alternative<UserCancelled>(result.value().destination) ? "cancelled" : "success";
  event.safe_context["range"] = event.outcome != "success" ? "unknown" :
      result.value().output_encoding.transfer == TransferFunction::pq ? "HDR" : "SDR";
  event.safe_context["elapsedMs"] = std::to_string(elapsed_ms);
  if (!result) {
    event.error_code = to_string(result.error().code);
    event.error_origin = result.error().origin;
    event.safe_context.insert(result.error().safe_context.begin(), result.error().safe_context.end());
  } else {
    std::visit([&event](const auto& destination) {
      if constexpr (requires { destination.bytes_written; }) event.bytes = destination.bytes_written;
    }, result.value().destination);
    event.safe_context["clippedPixels"] = std::to_string(result.value().luminance_clip.clipped_pixel_count);
    event.safe_context["clippedChannels"] = std::to_string(result.value().luminance_clip.clipped_channel_count);
  }
  (void)diagnostics->record(event);
}
} // namespace hdrshot
