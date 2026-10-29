#pragma once

#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVariant>

#include <cstdint>

namespace hdrshot {

enum class ToolbarSymbol : std::uint8_t {
  select,
  rectangle,
  ellipse,
  line,
  brush,
  arrow,
  text,
  undo,
  redo,
  save,
  save_as,
  copy,
  cancel,
  analyze,
  pin,
  pin_create,
  panel_top_close,
  panel_top_open,
};

inline const char* toolbar_symbol_name(const ToolbarSymbol symbol) {
  switch (symbol) {
    case ToolbarSymbol::select: return "select";
    case ToolbarSymbol::rectangle: return "rectangle";
    case ToolbarSymbol::ellipse: return "ellipse";
    case ToolbarSymbol::line:
      return "line";
    case ToolbarSymbol::brush:
      return "brush";
    case ToolbarSymbol::arrow: return "arrow";
    case ToolbarSymbol::text: return "text";
    case ToolbarSymbol::undo: return "undo";
    case ToolbarSymbol::redo: return "redo";
    case ToolbarSymbol::save: return "save";
    case ToolbarSymbol::save_as: return "save_as";
    case ToolbarSymbol::copy: return "copy";
    case ToolbarSymbol::cancel: return "cancel";
    case ToolbarSymbol::analyze: return "analyze";
    case ToolbarSymbol::pin: return "pin";
    case ToolbarSymbol::pin_create: return "pin_create";
    case ToolbarSymbol::panel_top_close: return "panel-top-close";
    case ToolbarSymbol::panel_top_open: return "panel-top-open";
  }
  return "unknown";
}

class ToolbarSymbolButton final : public QToolButton {
 public:
  ToolbarSymbolButton(
      const ToolbarSymbol symbol,
      const bool action_press_feedback,
      QWidget* const parent)
      : QToolButton(parent),
        symbol_(symbol),
        action_press_feedback_(action_press_feedback) {
    setProperty("toolbarSymbol", toolbar_symbol_name(symbol_));
    setProperty("actionPressFeedback", action_press_feedback_);
    setProperty("pressedSymbolScalePercent", 85);
    setProperty("pressedSymbolOffsetPx", 2);
  }

  void set_symbol(ToolbarSymbol symbol) {
    if (symbol_ == symbol) return;
    symbol_ = symbol;
    setProperty("toolbarSymbol", toolbar_symbol_name(symbol_));
    update();
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    QToolButton::paintEvent(event);
    if (property("thumbnailMode").toBool()) return;

    const bool dark = property("darkTheme").toBool();
    QColor color;
    if (property("cancelAction").toBool()) {
      color = QColor(dark ? "#FF6B6B" : "#D43D3D");
    } else if (property("primary").toBool() || property("analysisAction").toBool()) {
      color = QColor("#FFFFFF");
    } else if (isChecked()) {
      color = QColor(dark ? "#20252C" : "#FFFFFF");
    } else {
      color = QColor(dark ? "#E7E9EC" : "#343940");
    }
    if (!isEnabled()) {
      color.setAlphaF(0.42F);
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.translate(width() / 2.0, height() / 2.0);
    if (action_press_feedback_ && isDown()) {
      painter.translate(kPressedSymbolOffsetPx, kPressedSymbolOffsetPx);
      painter.scale(kPressedSymbolScale, kPressedSymbolScale);
    }
    painter.translate(-12.0, -12.0);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    draw_symbol(painter);
  }

 private:
  static constexpr double kPressedSymbolScale = 0.85;
  static constexpr double kPressedSymbolOffsetPx = 2.0;

  void draw_symbol(QPainter& painter) const {
    QPainterPath path;
    switch (symbol_) {
      case ToolbarSymbol::analyze:
        painter.drawLine(QPointF(4, 20), QPointF(20, 20));
        painter.drawLine(QPointF(5, 17), QPointF(5, 11));
        painter.drawLine(QPointF(10, 17), QPointF(10, 5));
        painter.drawLine(QPointF(15, 17), QPointF(15, 9));
        painter.drawLine(QPointF(20, 17), QPointF(20, 3));
        break;
      case ToolbarSymbol::select:
        path.moveTo(5.0, 3.8);
        path.lineTo(18.2, 15.0);
        path.lineTo(11.9, 15.8);
        path.lineTo(8.4, 20.9);
        path.closeSubpath();
        painter.drawPath(path);
        break;
      case ToolbarSymbol::rectangle:
        painter.drawRoundedRect(QRectF(4.0, 5.0, 16.0, 14.0), 1.5, 1.5);
        break;
      case ToolbarSymbol::ellipse:
        painter.drawEllipse(QPointF(12.0, 12.0), 8.5, 6.5);
        break;
      case ToolbarSymbol::line:
        painter.drawLine(QPointF(4.5, 18.5), QPointF(19, 5));
        break;
      case ToolbarSymbol::brush:
        path.moveTo(10.4, 13.6);
        path.lineTo(18.2, 4.2);
        path.quadTo(19.6, 2.7, 21, 4.1);
        path.quadTo(22.3, 5.4, 20.7, 6.8);
        path.lineTo(11.4, 14.6);
        path.moveTo(10.4, 13.6);
        path.lineTo(11.4, 14.6);
        path.moveTo(10.4, 13.6);
        path.cubicTo(8.9, 12.3, 6.7, 13.1, 6.5, 15.2);
        path.cubicTo(6.2, 17.4, 4.6, 18.2, 3, 18.7);
        path.cubicTo(7, 21, 11.8, 19.2, 11.4, 14.6);
        painter.drawPath(path);
        break;
      case ToolbarSymbol::arrow:
        painter.drawLine(QPointF(4.5, 18.5), QPointF(19.0, 5.0));
        path.moveTo(11.5, 5.0);
        path.lineTo(19.0, 5.0);
        path.lineTo(19.0, 12.5);
        painter.drawPath(path);
        break;
      case ToolbarSymbol::text:
        painter.drawLine(QPointF(5.0, 5.5), QPointF(5.0, 4.0));
        painter.drawLine(QPointF(5.0, 4.0), QPointF(19.0, 4.0));
        painter.drawLine(QPointF(19.0, 4.0), QPointF(19.0, 5.5));
        painter.drawLine(QPointF(12.0, 4.0), QPointF(12.0, 20.0));
        painter.drawLine(QPointF(9.0, 20.0), QPointF(15.0, 20.0));
        break;
      case ToolbarSymbol::undo:
        path.moveTo(8.0, 7.0);
        path.lineTo(4.0, 11.0);
        path.lineTo(8.0, 15.0);
        path.moveTo(5.0, 11.0);
        path.lineTo(12.5, 11.0);
        path.cubicTo(16.0, 11.0, 18.5, 13.5, 18.5, 17.0);
        painter.drawPath(path);
        break;
      case ToolbarSymbol::redo:
        path.moveTo(16.0, 7.0);
        path.lineTo(20.0, 11.0);
        path.lineTo(16.0, 15.0);
        path.moveTo(19.0, 11.0);
        path.lineTo(11.5, 11.0);
        path.cubicTo(8.0, 11.0, 5.5, 13.5, 5.5, 17.0);
        painter.drawPath(path);
        break;
      case ToolbarSymbol::save:
        path.moveTo(5.0, 3.5);
        path.lineTo(16.0, 3.5);
        path.lineTo(19.0, 6.5);
        path.lineTo(19.0, 20.0);
        path.lineTo(5.0, 20.0);
        path.closeSubpath();
        path.moveTo(8.0, 3.5);
        path.lineTo(8.0, 9.5);
        path.lineTo(16.0, 9.5);
        path.lineTo(16.0, 3.5);
        path.moveTo(8.0, 20.0);
        path.lineTo(8.0, 14.0);
        path.lineTo(16.0, 14.0);
        path.lineTo(16.0, 20.0);
        painter.drawPath(path);
        break;
      case ToolbarSymbol::save_as:
        path.moveTo(4.5, 3.5);
        path.lineTo(14.5, 3.5);
        path.lineTo(17.5, 6.5);
        path.lineTo(17.5, 11.7);
        path.moveTo(4.5, 3.5);
        path.lineTo(4.5, 19.5);
        path.lineTo(10.5, 19.5);
        path.moveTo(7.5, 3.5);
        path.lineTo(7.5, 9.0);
        path.lineTo(14.5, 9.0);
        path.lineTo(14.5, 3.5);
        path.moveTo(12.2, 18.7);
        path.lineTo(12.7, 15.7);
        path.lineTo(18.6, 9.8);
        path.lineTo(20.7, 11.9);
        path.lineTo(14.8, 17.8);
        path.closeSubpath();
        path.moveTo(17.5, 10.9);
        path.lineTo(19.6, 13.0);
        painter.drawPath(path);
        break;
      case ToolbarSymbol::copy:
        painter.drawRoundedRect(QRectF(8.0, 7.0, 11.0, 13.0), 1.8, 1.8);
        path.moveTo(16.0, 7.0);
        path.lineTo(16.0, 5.8);
        path.cubicTo(16.0, 4.8, 15.2, 4.0, 14.2, 4.0);
        path.lineTo(5.8, 4.0);
        path.cubicTo(4.8, 4.0, 4.0, 4.8, 4.0, 5.8);
        path.lineTo(4.0, 16.2);
        path.cubicTo(4.0, 17.2, 4.8, 18.0, 5.8, 18.0);
        path.lineTo(8.0, 18.0);
        painter.drawPath(path);
        break;
      case ToolbarSymbol::pin:
      case ToolbarSymbol::pin_create:
        painter.save();
        if (symbol_ == ToolbarSymbol::pin_create) {
          painter.translate(12.0, 12.0);
          painter.rotate(45.0);
          painter.translate(-12.0, -12.0);
        }
        path.moveTo(7.0, 3.5);
        path.lineTo(17.0, 3.5);
        path.moveTo(9.0, 3.5);
        path.lineTo(9.0, 9.0);
        path.cubicTo(9.0, 11.5, 5.5, 11.5, 5.5, 15.0);
        path.lineTo(18.5, 15.0);
        path.cubicTo(18.5, 11.5, 15.0, 11.5, 15.0, 9.0);
        path.lineTo(15.0, 3.5);
        path.moveTo(12.0, 15.0);
        path.lineTo(12.0, 21.0);
        painter.drawPath(path);
        painter.restore();
        break;
      case ToolbarSymbol::panel_top_close:
      case ToolbarSymbol::panel_top_open:
        // Lucide panel-top-close/open: shared panel and the original chevrons.
        // https://lucide.dev/icons/panel-top-close
        // https://lucide.dev/icons/panel-top-open
        painter.drawRoundedRect(QRectF(3, 3, 18, 18), 2, 2);
        painter.drawLine(QPointF(3, 9), QPointF(21, 9));
        if (symbol_ == ToolbarSymbol::panel_top_close) {
          path.moveTo(9, 16); path.lineTo(12, 13); path.lineTo(15, 16);
        } else {
          path.moveTo(15, 14); path.lineTo(12, 17); path.lineTo(9, 14);
        }
        painter.drawPath(path);
        break;
      case ToolbarSymbol::cancel:
        painter.drawLine(QPointF(6.0, 6.0), QPointF(18.0, 18.0));
        painter.drawLine(QPointF(18.0, 6.0), QPointF(6.0, 18.0));
        break;
    }
  }

  ToolbarSymbol symbol_;
  bool action_press_feedback_{};
};

}  // namespace hdrshot
