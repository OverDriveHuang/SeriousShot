#pragma once
#include "core/error.hpp"
#include "application/interaction_controller.hpp"
#include <QString>
namespace hdrshot {
inline QString capture_busy_message(UiPhase phase, bool draining) {
  if (draining) return QStringLiteral("上次截图仍在处理，请稍后再次按截图快捷键或点菜单“截图”。若持续无法恢复，请重启 SeriousShot。");
  if (phase == UiPhase::capturing) return QStringLiteral("正在准备屏幕画面，请稍候。");
  return QStringLiteral("截图界面已经打开。");
}
inline QString capture_error_message(const Error& error, const QString& permission_guidance, const QString& hotkey) {
  if (error.code == ErrorCode::permission_denied) return permission_guidance;
  const auto reason = error.safe_context.find("reason");
  if (reason != error.safe_context.end() && reason->second == "capture_draining")
    return capture_busy_message(UiPhase::capturing, true);
  const auto action = hotkey.isEmpty() ? QStringLiteral("截图快捷键") : hotkey;
  if (reason != error.safe_context.end() && reason->second == "capture_progress_timeout")
    return QStringLiteral("未能及时取得全部屏幕画面。请再次按 %1 或点菜单“截图”重试；若仍在处理，请稍后重试，持续不恢复时重启 SeriousShot。").arg(action);
  return QStringLiteral("未能取得屏幕画面，请再次按 %1 或点菜单“截图”重试。错误：%2")
      .arg(action, QString::fromStdString(to_string(error.code)));
}
inline QString capture_error_message(const Error& error, const QString& permission_guidance) {
  return capture_error_message(error, permission_guidance, {});
}
}
