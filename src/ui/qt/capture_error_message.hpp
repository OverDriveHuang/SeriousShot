#pragma once
#include "application/interaction_controller.hpp"
#include "core/error.hpp"
#include <QString>

namespace hdrshot {
inline QString capture_busy_message(UiPhase phase, bool draining) {
  if (draining) {
    //% "The previous capture is still processing. Wait, then press the capture shortcut or choose “Capture” again. If it stays busy, restart SeriousShot."
    return qtTrId("capture.draining");
  }
  if (phase == UiPhase::capturing) {
    //% "Preparing the screen capture…"
    return qtTrId("capture.preparing");
  }
  //% "The capture overlay is already open."
  return qtTrId("capture.already_open");
}
inline QString capture_error_message(const Error &error, const QString &permission_guidance,
                                     const QString &hotkey) {
  if (error.code == ErrorCode::permission_denied)
    return permission_guidance;
  const auto reason = error.safe_context.find("reason");
  if (reason != error.safe_context.end() && reason->second == "capture_draining")
    return capture_busy_message(UiPhase::capturing, true);
  //% "the capture shortcut"
  const auto action = hotkey.isEmpty() ? qtTrId("capture.shortcut_fallback") : hotkey;
  if (reason != error.safe_context.end() && reason->second == "capture_progress_timeout") {
    //% "Could not get all screen images in time. Press %1 or choose “Capture” to retry. If it is still processing, wait before retrying. Restart SeriousShot if it stays busy."
    return qtTrId("capture.progress_timeout").arg(action);
  }
  //% "Could not capture the screen. Press %1 or choose “Capture” to retry. Error: %2"
  return qtTrId("capture.failed").arg(action, QString::fromStdString(to_string(error.code)));
}
inline QString capture_error_message(const Error &error, const QString &permission_guidance) {
  return capture_error_message(error, permission_guidance, {});
}
} // namespace hdrshot
