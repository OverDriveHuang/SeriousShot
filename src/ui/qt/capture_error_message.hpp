#pragma once
#include "core/error.hpp"
#include <QString>
namespace hdrshot {
inline QString capture_error_message(const Error& error, const QString& permission_guidance) {
  if (error.code == ErrorCode::permission_denied) return permission_guidance;
  return QStringLiteral("未能取得屏幕画面，请再次按截图快捷键重试。错误：%1")
      .arg(QString::fromStdString(to_string(error.code)));
}
} // namespace hdrshot
