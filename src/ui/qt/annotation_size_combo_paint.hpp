#pragma once
#include <QComboBox>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>

namespace hdrshot::annotation_size_combo_paint {
enum class State { normal, hover, pressed, popup_open };
inline QSize preferred_size(const QComboBox& combo) {
  int longest = 0;
  for (int row = 0; row < combo.count(); ++row)
    longest = std::max(longest, combo.fontMetrics().horizontalAdvance(combo.itemText(row)));
  return {std::max(76, longest + 31), std::max(28, combo.fontMetrics().height() + 10)};
}
// Single surface and arrow; no native drop-down subcontrol is painted underneath.
inline void paint(QPainter& painter, const QComboBox& combo, State state) {
  const bool dark = combo.palette().color(QPalette::Window).lightness() < 128;
  QColor fill = dark ? QColor("#4D525A") : QColor("#CBCED3");
  if (state == State::hover) fill = dark ? QColor("#606771") : QColor("#B8BEC6");
  if (state == State::pressed || state == State::popup_open)
    fill = dark ? QColor("#44484F") : QColor("#B8BEC6");
  QColor ink = dark ? QColor("#E7E9EC") : QColor("#343940");
  if (!combo.isEnabled()) ink.setAlpha(110);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(QPen(dark ? QColor("#44484F") : QColor("#D5D8DD"), 1));
  painter.setBrush(fill);
  painter.drawRoundedRect(QRectF(combo.rect()).adjusted(.5,.5,-.5,-.5), 6, 6);
  painter.setFont(combo.font()); painter.setPen(ink);
  const QRectF text(7, 0, combo.width()-31, combo.height());
  const auto label = combo.fontMetrics().elidedText(combo.currentText(), Qt::ElideRight, int(text.width()));
  painter.drawText(text, Qt::AlignCenter, label);
  // 6-DIP chevron, with 10-DIP right margin and a separate reserved text area.
  const qreal right = combo.width()-10, y = combo.height()/2.0;
  QPainterPath chevron;
  chevron.moveTo(right-6,y-1.5);chevron.lineTo(right-3,y+1.5);chevron.lineTo(right,y-1.5);
  painter.setPen(QPen(ink,1.2,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
  painter.drawPath(chevron);
}
}  // namespace hdrshot::annotation_size_combo_paint
