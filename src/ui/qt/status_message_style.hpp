#pragma once
#include <QPalette>
#include <QString>

namespace hdrshot {
inline constexpr int status_message_duration_ms = 6000;
inline constexpr int save_success_duration_ms = 1500;
inline QString status_message_style(const QPalette &palette, bool error) {
  const bool dark = palette.color(QPalette::Window).lightness() < 128;
  const auto background = error
      ? (dark ? "rgba(111,30,38,245)" : "rgba(255,225,228,248)")
      : (dark ? "rgba(25,74,52,245)" : "rgba(221,246,232,248)");
  return QString("QLabel { color: %1; background: %2; border: 1px solid %3;"
                 " border-radius: 8px; padding: 8px 12px; font-weight: 600; }")
      .arg(dark ? "#FFFFFF" : "#17202A", background,
           error ? "#FF7382" : "#3FC77A");
}
} // namespace hdrshot
