#pragma once

#include <QAbstractButton>
#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QString>
#include <QToolButton>

namespace hdrshot::analyzer_control_style {

// The old Delta handle measured 28x30 after the window stylesheet was applied.
// Its rendered height is the common, square outer button size.
inline constexpr int icon_button_side = 30;
inline constexpr int icon_canvas_side = 20;
inline constexpr int corner_radius = 5;
inline constexpr int border_width = 1;
// Display P3 linear (0.752, 0.109, 0.077) converted through the inverse of
// ExtendedP3Mapper::annotation_linear_display_p3 and then the sRGB OETF.
// The converted red is the semantic foreground; backgrounds match ordinary
// analyzer controls across all danger actions.
inline constexpr auto danger_normal = "#30343b";
inline constexpr auto danger_hover = "#424750";
inline constexpr auto danger_pressed = "#262a30";
inline constexpr auto danger_border = "#4b515c";
inline constexpr auto danger_foreground = "#F35146";

inline QString danger_qss() {
  return QStringLiteral(
      "QToolButton[analyzerDanger=\"true\"],QPushButton[analyzerDanger=\"true\"]"
      "{background:%1;color:%5;border:%6px solid %4;border-radius:%7px;}"
      "QToolButton[analyzerDanger=\"true\"]:hover,QPushButton[analyzerDanger=\"true\"]:hover"
      "{background:%2;}"
      "QToolButton[analyzerDanger=\"true\"]:pressed,QPushButton[analyzerDanger=\"true\"]:pressed"
      "{background:%3;}")
      .arg(danger_normal, danger_hover, danger_pressed, danger_border,
           danger_foreground)
      .arg(border_width).arg(corner_radius);
}

inline void set_danger(QAbstractButton *button) {
  button->setProperty("analyzerDanger", true);
  button->setStyleSheet(danger_qss());
}

inline void set_icon_button(QToolButton *button, bool danger = false) {
  button->setIconSize(QSize(icon_canvas_side, icon_canvas_side));
  button->setToolButtonStyle(Qt::ToolButtonIconOnly);
  if (danger)
    set_danger(button);
  else
    button->setStyleSheet(QString());
  const int content_side = icon_button_side - 2 * border_width;
  button->setStyleSheet(button->styleSheet() + QStringLiteral(
      "QToolButton{padding:0;border-width:%2px;border-radius:%3px;"
      "min-width:%1px;max-width:%1px;min-height:%1px;max-height:%1px;}")
      .arg(content_side).arg(border_width).arg(corner_radius));
  button->setFixedSize(icon_button_side, icon_button_side);
}

inline void set_danger_text_button(QToolButton *button) {
  set_danger(button);
  const int content_height = icon_button_side - 2 * border_width;
  button->setStyleSheet(button->styleSheet() + QStringLiteral(
      "QToolButton{padding:0;min-height:%1px;max-height:%1px;}")
      .arg(content_height));
  button->setFixedHeight(icon_button_side);
}

// Keep QToolButton's interaction, accessibility and styled panel, while laying
// out its icon and text as one centered group. The default TextBesideIcon
// drawing leaves the group at the left edge under this local QSS.
class CenteredTextToolButton final : public QToolButton {
public:
  using QToolButton::QToolButton;

protected:
  void paintEvent(QPaintEvent *) override {
    QStyleOptionToolButton option;
    initStyleOption(&option);
    const QIcon content_icon = option.icon;
    const QString content_text = option.text;
    option.icon = QIcon();
    option.text.clear();

    QPainter painter(this);
    style()->drawComplexControl(QStyle::CC_ToolButton, &option, &painter, this);

    const int icon_width = iconSize().width();
    const int icon_height = iconSize().height();
    const int gap = fontMetrics().horizontalAdvance(QStringLiteral(" "));
    const int text_width = fontMetrics().horizontalAdvance(content_text);
    const int group_width = icon_width + gap + text_width;
    const int start_x = (width() - group_width) / 2;
    const int icon_y = (height() - icon_height) / 2;
    content_icon.paint(&painter, QRect(start_x, icon_y, icon_width, icon_height),
                       Qt::AlignCenter,
                       isEnabled() ? QIcon::Normal : QIcon::Disabled);
    painter.setPen(option.palette.color(QPalette::ButtonText));
    painter.drawText(QRect(start_x + icon_width + gap, 0,
                           text_width, height()),
                     Qt::AlignVCenter | Qt::AlignLeft, content_text);
  }
};

inline QIcon danger_glyph(bool trash) {
  QPixmap pixmap(icon_canvas_side * 2, icon_canvas_side * 2);
  pixmap.setDevicePixelRatio(2);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(QPen(QColor(danger_foreground), 1.8, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
  if (trash) {
    painter.drawLine(3, 5, 17, 5);
    painter.drawLine(7, 2, 13, 2);
    painter.drawPolyline(QPolygonF{{5, 7}, {6, 18}, {14, 18}, {15, 7}});
    painter.drawLine(8, 9, 8, 15);
    painter.drawLine(12, 9, 12, 15);
  } else {
    painter.drawLine(4, 4, 16, 16);
    painter.drawLine(16, 4, 4, 16);
  }
  return QIcon(pixmap);
}

} // namespace hdrshot::analyzer_control_style
