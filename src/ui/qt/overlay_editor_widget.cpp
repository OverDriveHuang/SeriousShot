#include "ui/qt/overlay_editor_widget.hpp"

#include <QComboBox>
#include <QCursor>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QShortcut>
#include <QToolButton>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <utility>

namespace hdrshot {
namespace {

constexpr std::array<std::uint32_t, 7> kColors{
    0xFF4D67, 0xFFC83D, 0xFF8A34, 0x25C06D, 0x36A3FF, 0xFFFFFF, 0x111111};
constexpr std::array<std::uint16_t, 5> kLineWidths{2, 4, 6, 8, 12};
constexpr std::array<std::uint16_t, 6> kFontSizes{12, 16, 20, 24, 32, 48};
constexpr double kPressedSymbolScale = 0.85;
constexpr double kPressedSymbolOffsetPx = 2.0;

QCursor rotation_cursor() {
  // Shared vector symbol, black stroke with white surround on either theme.
  // Qt's platform plugin owns the native cursor; no AppKit/Win32 geometry here.
  static const QCursor cursor = [] {
    QPixmap image(32,32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.arcMoveTo(QRectF(7,7,18,18),35);
    path.arcTo(QRectF(7,7,18,18),35,285);
    const auto end=path.currentPosition();
    path.moveTo(end+QPointF(-6,-1)); path.lineTo(end); path.lineTo(end+QPointF(-1,6));
    for(auto pen : {QPen(Qt::white,5,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin),
                    QPen(Qt::black,2,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin)}) {
      painter.setPen(pen); painter.drawPath(path);
    }
    painter.end();
    return QCursor(image,16,16);
  }();
  return cursor;
}

enum class ToolbarSymbol : std::uint8_t {
  select,
  rectangle,
  ellipse,
  arrow,
  text,
  undo,
  redo,
  save,
  save_as,
  copy,
  cancel,
  analyze,
};

const char* toolbar_symbol_name(const ToolbarSymbol symbol) {
  switch (symbol) {
    case ToolbarSymbol::select: return "select";
    case ToolbarSymbol::rectangle: return "rectangle";
    case ToolbarSymbol::ellipse: return "ellipse";
    case ToolbarSymbol::arrow: return "arrow";
    case ToolbarSymbol::text: return "text";
    case ToolbarSymbol::undo: return "undo";
    case ToolbarSymbol::redo: return "redo";
    case ToolbarSymbol::save: return "save";
    case ToolbarSymbol::save_as: return "save_as";
    case ToolbarSymbol::copy: return "copy";
    case ToolbarSymbol::cancel: return "cancel";
    case ToolbarSymbol::analyze: return "analyze";
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

 protected:
  void paintEvent(QPaintEvent* event) override {
    QToolButton::paintEvent(event);

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
      case ToolbarSymbol::cancel:
        painter.drawLine(QPointF(6.0, 6.0), QPointF(18.0, 18.0));
        painter.drawLine(QPointF(18.0, 6.0), QPointF(6.0, 18.0));
        break;
    }
  }

  ToolbarSymbol symbol_;
  bool action_press_feedback_{};
};

QColor qcolor(const std::uint32_t rgb) {
  return QColor(
      static_cast<int>((rgb >> 16U) & 0xFFU),
      static_cast<int>((rgb >> 8U) & 0xFFU),
      static_cast<int>(rgb & 0xFFU));
}

PixelRect normalized_rect(const PixelPoint start, const PixelPoint end) {
  const auto left = std::min(start.x, end.x);
  const auto top = std::min(start.y, end.y);
  return PixelRect{
      left,
      top,
      std::max(1, std::max(start.x, end.x) - left),
      std::max(1, std::max(start.y, end.y) - top),
  };
}

}  // namespace

OverlayEditorWidget::OverlayEditorWidget(
    const PixelSize capture_size_px,
    const double point_pixel_scale,
    TextRasterizerPort* const text_rasterizer,
    std::string text_editor_font_family,
    QtInputPlatformAdapter& input_platform,
    const SelectionSnapshot initial_selection,
    QWidget* parent)
    : QWidget(parent),
      capture_size_px_(capture_size_px),
      point_pixel_scale_(point_pixel_scale),
      text_rasterizer_(text_rasterizer),
      text_editor_font_family_(std::move(text_editor_font_family)),
      input_platform_(input_platform),
      selection_(initial_selection) {
  setAttribute(Qt::WA_NativeWindow);
  setAttribute(Qt::WA_TranslucentBackground);
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  build_controls();
  apply_theme();
  choose_tool(EditorTool::select);
  const auto initial_plan_ready = rebuild_render_plan();
  Q_ASSERT(initial_plan_ready);
}

void OverlayEditorWidget::set_preview_changed(PreviewChanged callback) {
  preview_changed_ = std::move(callback);
}

void OverlayEditorWidget::set_action_requested(ActionRequested callback) {
  action_requested_ = std::move(callback);
}

void OverlayEditorWidget::set_analysis_available(bool available) {
  toolbar_->findChild<QToolButton*>("actionAnalyze")->setVisible(available);
  toolbar_->findChild<QFrame*>("separatorBeforeAnalyze")->setVisible(available);
  layout_controls();
}

void OverlayEditorWidget::set_completion_bindings(const CompletionBindings bindings) {
  completion_bindings_ = bindings;
}

void OverlayEditorWidget::set_interaction_started(InteractionStarted callback) {
  interaction_started_ = std::move(callback);
}

void OverlayEditorWidget::set_interaction_locked(bool locked) {
  interaction_locked_ = locked;
  update_pointer_cursor(mapFromGlobal(QCursor::pos()));
}

void OverlayEditorWidget::set_window_candidates(WindowSnapshotRef snapshot, DisplayId display) {
  if (!snapshot || !selection_.desktop_rect.empty()) return;
  window_selection_ = std::make_unique<WindowSelectionGesture>(std::move(snapshot), display, capture_size_px_);
  layout_controls();
}
void OverlayEditorWidget::set_initial_gesture_changed(InitialGestureChanged callback) {
  initial_gesture_changed_ = std::move(callback);
}
PreviewHighlight OverlayEditorWidget::initial_highlight() const {
  return window_selection_ ? window_selection_->preview() : PreviewHighlight{};
}
SelectionPointer OverlayEditorWidget::selection_pointer(QPointF point) const {
  return {to_capture_point(point), point.x(), point.y(),
      point.x() >= 0 && point.y() >= 0 && point.x() < width() && point.y() < height()};
}
void OverlayEditorWidget::refresh_window_candidate(QPointF position) {
  if (!selection_.desktop_rect.empty()) return;
  update_pointer_cursor(position);
  if (!window_selection_) return;
  if (interaction_started_ && !interaction_started_()) { clear_window_candidate(); return; }
  const auto previous = initial_highlight();
  window_selection_->move(selection_pointer(position));
  if (previous != initial_highlight()) notify_preview_changed();
}
void OverlayEditorWidget::finish_initial_gesture() {
  if (initial_gesture_changed_) (void)initial_gesture_changed_(false);
}
void OverlayEditorWidget::clear_window_candidate() {
  if (!window_selection_) return;
  const auto previous = initial_highlight();
  const bool pressed = window_selection_->pressed();
  window_selection_->cancel();
  if (pressed) finish_initial_gesture();
  if (previous != initial_highlight()) notify_preview_changed();
}
void OverlayEditorWidget::interrupt_pointer_gesture() {
  clear_window_candidate();
  dragging_selection_ = false;
  dragging_annotation_ = false;
  dragging_annotation_transform_ = false;
  selection_adjustment_.reset();
  annotation_adjustment_.reset();
  annotation_drag_origin_.reset();
  annotation_drag_preview_.reset();
  rotation_drag_.reset();
  transient_annotation_render_plan_.reset();
  drag_current_.reset();
  draft_annotation_.reset();
  last_press_had_selection_ = false;
  if (clean_content_presented_) notify_preview_changed();
  update();
}
void OverlayEditorWidget::restore_interaction_focus(QPointF position) {
  if (text_editor_) text_editor_->setFocus(Qt::MouseFocusReason);
  else setFocus(Qt::MouseFocusReason);
  refresh_window_candidate(position);
}
bool OverlayEditorWidget::event(QEvent* event) {
  if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::Hide ||
      event->type() == QEvent::WindowDeactivate) {
    interrupt_pointer_gesture();
  }
  return QWidget::event(event);
}

void OverlayEditorWidget::show_status(const QString& message, const bool is_error) {
  const bool dark = palette().color(QPalette::Window).lightness() < 128;
  const auto background = is_error
      ? (dark ? "rgba(111,30,38,245)" : "rgba(255,225,228,248)")
      : (dark ? "rgba(25,74,52,245)" : "rgba(221,246,232,248)");
  const auto foreground = dark ? "#FFFFFF" : "#17202A";
  const auto border = is_error ? "#FF7382" : "#3FC77A";
  status_label_->setStyleSheet(QString(
      "QLabel { color: %1; background: %2; border: 1px solid %3;"
      " border-radius: 8px; padding: 8px 12px; font-weight: 600; }")
      .arg(foreground, background, border));
  status_label_->setText(message);
  status_label_->setVisible(true);
  status_label_->adjustSize();
  layout_controls();
  status_label_->raise();
  QTimer::singleShot(6000, status_label_, [label = status_label_] {
    label->setVisible(false);
  });
}

void OverlayEditorWidget::build_controls() {
  toolbar_ = new QFrame(this);
  toolbar_->setObjectName("editorToolbar");
  // Controls must not inherit the drawing canvas's crosshair/resize cursor.
  toolbar_->setCursor(Qt::ArrowCursor);
  toolbar_->setAttribute(Qt::WA_NativeWindow);
  toolbar_layout_ = new QHBoxLayout(toolbar_);
  toolbar_layout_->setContentsMargins(8, 7, 8, 7);
  toolbar_layout_->setSpacing(5);

  constexpr std::array<const char*, 5> labels{"选择", "矩形", "圆形", "箭头", "文字"};
  constexpr std::array<const char*, 5> object_names{
      "toolSelect", "toolRectangle", "toolEllipse", "toolArrow", "toolText"};
  constexpr std::array<ToolbarSymbol, 5> symbols{
      ToolbarSymbol::select,
      ToolbarSymbol::rectangle,
      ToolbarSymbol::ellipse,
      ToolbarSymbol::arrow,
      ToolbarSymbol::text,
  };
  const auto add_separator = [this](const char* object_name) {
    auto* separator = new QFrame(toolbar_);
    separator->setObjectName(QString::fromLatin1(object_name));
    separator->setProperty("toolbarSeparator", true);
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Plain);
    separator->setFixedSize(1, 26);
    toolbar_layout_->addWidget(separator);
  };
  for (std::size_t index = 0; index < labels.size(); ++index) {
    auto* button = new ToolbarSymbolButton(symbols[index], false, toolbar_);
    button->setObjectName(QString::fromLatin1(object_names[index]));
    button->setCheckable(true);
    button->setAutoExclusive(true);
    button->setFixedSize(38, 38);
    button->setToolTip(QString::fromUtf8(labels[index]));
    button->setAccessibleName(QString::fromUtf8(labels[index]));
    const auto tool = static_cast<EditorTool>(index);
    connect(button, &QToolButton::clicked, this, [this, tool] { choose_tool(tool); });
    toolbar_layout_->addWidget(button);
    tool_buttons_[index] = button;
    if (index == 0) {
      add_separator("separatorAfterSelect");
    }
  }

  add_separator("separatorBeforeUndo");
  const auto add_action = [this](
                              const char* label,
                              const UiCommand command,
                              const bool primary = false) {
    auto symbol = ToolbarSymbol::cancel;
    switch (command) {
      case UiCommand::undo:
        symbol = ToolbarSymbol::undo;
        break;
      case UiCommand::redo:
        symbol = ToolbarSymbol::redo;
        break;
      case UiCommand::analyze:
        symbol = ToolbarSymbol::analyze;
        break;
      case UiCommand::save_default:
        symbol = ToolbarSymbol::save;
        break;
      case UiCommand::save_as:
        symbol = ToolbarSymbol::save_as;
        break;
      case UiCommand::copy_and_close:
        symbol = ToolbarSymbol::copy;
        break;
      case UiCommand::cancel_capture:
        symbol = ToolbarSymbol::cancel;
        break;
      default:
        break;
    }
    auto* button = new ToolbarSymbolButton(symbol, true, toolbar_);
    switch (command) {
      case UiCommand::undo: button->setObjectName("actionUndo"); break;
      case UiCommand::redo: button->setObjectName("actionRedo"); break;
      case UiCommand::save_default: button->setObjectName("actionSaveDefault"); break;
      case UiCommand::save_as: button->setObjectName("actionSaveAs"); break;
      case UiCommand::copy_and_close: button->setObjectName("actionCopy"); break;
      case UiCommand::cancel_capture: button->setObjectName("actionCancel"); break;
      case UiCommand::analyze: button->setObjectName("actionAnalyze"); break;
      default: break;
    }
    button->setProperty("primary", primary);
    button->setProperty("cancelAction", command == UiCommand::cancel_capture);
    if (command == UiCommand::analyze) {
      button->setProperty("analysisAction", true);
      button->setStyleSheet("QToolButton { background:#174D3B; border:1px solid #347B5E; border-radius:6px; } QToolButton:hover { background:#226448; } QToolButton:pressed { background:#123C2E; }");
      button->hide(); // only exposed when an actual analysis service is injected
    }
    button->setToolTip(QString::fromUtf8(label));
    button->setAccessibleName(QString::fromUtf8(label));
    button->setFixedSize(38, 38);
    connect(button, &QToolButton::clicked, this, [this, command] {
      if (command == UiCommand::undo) {
        apply_document_command(UndoAnnotation{});
      } else if (command == UiCommand::redo) {
        apply_document_command(RedoAnnotation{});
      } else {
        request_action(command);
      }
    });
    toolbar_layout_->addWidget(button);
  };
  add_action("撤销", UiCommand::undo);
  add_action("重做", UiCommand::redo);
  add_separator("separatorBeforeAnalyze");
  toolbar_->findChild<QFrame*>("separatorBeforeAnalyze")->hide();
  add_action("分析", UiCommand::analyze);
  add_separator("separatorBeforeSave");
  add_action("保存", UiCommand::save_default);
  add_action("另存为…", UiCommand::save_as);
  add_action("复制", UiCommand::copy_and_close, true);
  add_separator("separatorBeforeCancel");
  add_action("取消", UiCommand::cancel_capture);

  property_bar_ = new QFrame(this);
  property_bar_->setObjectName("propertyBar");
  property_bar_->setCursor(Qt::ArrowCursor);
  property_bar_->setAttribute(Qt::WA_NativeWindow);
  property_layout_ = new QHBoxLayout(property_bar_);
  property_layout_->setContentsMargins(10, 6, 10, 6);
  property_layout_->setSpacing(6);
  for (std::size_t index = 0; index < kColors.size(); ++index) {
    auto* button = new QToolButton(property_bar_);
    button->setObjectName(QString("color%1").arg(
        kColors[index], 6, 16, QLatin1Char('0')).toUpper());
    button->setCheckable(true);
    button->setAutoExclusive(true);
    button->setFixedSize(24, 24);
    button->setStyleSheet(QString(
        "QToolButton { background: #%1; border: 2px solid transparent; border-radius: 12px; }"
        "QToolButton:checked { border: 2px solid #5EA8FF; }")
        .arg(kColors[index], 6, 16, QLatin1Char('0')));
    const auto color = kColors[index];
    connect(button, &QToolButton::clicked, this, [this, color] { choose_color(color); });
    property_layout_->addWidget(button);
    color_buttons_[index] = button;
  }
  size_combo_ = new QComboBox(property_bar_);
  size_combo_->setFixedHeight(28);
  size_combo_->setMinimumWidth(76);
  connect(
      size_combo_, &QComboBox::currentTextChanged, this,
      [this](const QString&) {
        if (size_combo_->currentData().isValid()) {
          choose_size(static_cast<std::uint16_t>(size_combo_->currentData().toUInt()));
        }
      });
  property_layout_->addWidget(size_combo_);

  status_label_ = new QLabel(this);
  status_label_->setObjectName("exportStatus");
  status_label_->setAlignment(Qt::AlignCenter);
  status_label_->setWordWrap(true);
  status_label_->setMinimumWidth(360);
  status_label_->setMaximumWidth(720);
  status_label_->setVisible(false);

  delete_annotation_button_ = new ToolbarSymbolButton(
      ToolbarSymbol::cancel, false, this);
  delete_annotation_button_->setObjectName("deleteSelectedAnnotation");
  delete_annotation_button_->setProperty("cancelAction", true);
  delete_annotation_button_->setToolTip(QStringLiteral("删除标注"));
  delete_annotation_button_->setAccessibleName(QStringLiteral("删除标注"));
  delete_annotation_button_->setFixedSize(28, 28);
  delete_annotation_button_->hide();
  connect(delete_annotation_button_, &QToolButton::clicked, this, [this] {
    const auto* object = selected_object();
    if (object != nullptr) {
      apply_document_command(DeleteAnnotation{object->id});
    }
  });
}

void OverlayEditorWidget::apply_theme() {
  const bool dark = palette().color(QPalette::Window).lightness() < 128;
  const auto surface = dark ? "#26292E" : "#F5F6F8";
  const auto text = dark ? "#E7E9EC" : "#343940";
  const auto border = dark ? "#44484F" : "#D5D8DD";
  const auto hover = dark ? "#4D525A" : "#CBCED3";
  const auto selected = dark ? "#D9D9D9" : "#3B4046";
  const auto selected_icon = dark ? "#20252C" : "#FFFFFF";
  const auto cancel = dark ? "#FF6B6B" : "#D43D3D";
  const auto style = QString(
      "QFrame#editorToolbar, QFrame#propertyBar { background: %1; border: 1px solid %3;"
      " border-radius: 10px; }"
      "QToolButton { color: %2; background: transparent; border: 1px solid transparent;"
      " border-radius: 7px; padding: 0; font-weight: 600; }"
      "QToolButton:hover { background: %4; }"
      "QToolButton:pressed { background: %4; }"
      "QToolButton:checked { background: %5; color: %6; border-color: %5; }"
      "QToolButton:checked:hover, QToolButton:checked:pressed { background: %5; }"
      "QToolButton[primary=\"true\"] { background: #1677D2; color: white; }"
      "QToolButton[primary=\"true\"]:hover, QToolButton[primary=\"true\"]:pressed"
      " { background: #1677D2; }"
      "QToolButton[cancelAction=\"true\"] { color: %7; }"
      "QFrame[toolbarSeparator=\"true\"] { background: %3; border: none; }"
      "QComboBox { color: %2; background: %4; border: 1px solid %3; border-radius: 6px;"
      " padding: 0 7px; }"
      "QComboBox QAbstractItemView { color: %2; background: %1; selection-background-color: %5; }"
      "QToolTip { color: %2; background: %1; border: 1px solid %3; }")
      .arg(surface, text, border, hover, selected, selected_icon, cancel);
  for (auto* button : toolbar_->findChildren<QToolButton*>()) {
    button->setProperty("darkTheme", dark);
    button->update();
  }
  toolbar_->setStyleSheet(style);
  property_bar_->setStyleSheet(style);
  delete_annotation_button_->setProperty("darkTheme", dark);
  delete_annotation_button_->setStyleSheet(QString(
      "QToolButton { background: %1; border: 1px solid %2; border-radius: 14px; }"
      "QToolButton:hover { background: %3; }"
      "QToolButton:pressed { background: %3; }")
      .arg(
          dark ? "rgba(38,41,46,248)" : "rgba(250,250,251,248)",
          dark ? "#767B84" : "#858A92",
          dark ? "#565B64" : "#D8DBDF"));
  delete_annotation_button_->update();
}

void OverlayEditorWidget::choose_tool(const EditorTool tool) {
  if (text_editor_ != nullptr && tool != EditorTool::text) {
    if (!finish_pending_text_edit()) return;
  }
  tool_ = tool;
  tool_buttons_[static_cast<std::size_t>(tool)]->setChecked(true);
  update_property_controls();
}

void OverlayEditorWidget::choose_color(const std::uint32_t color_srgb_rgb) {
  color_srgb_rgb_ = color_srgb_rgb;
  const auto found = std::find(kColors.begin(), kColors.end(), color_srgb_rgb);
  if (found != kColors.end()) {
    color_buttons_[static_cast<std::size_t>(found - kColors.begin())]->setChecked(true);
  }
  if (text_editor_ != nullptr && pending_text_style_.has_value()) {
    pending_text_style_->color_srgb_rgb = color_srgb_rgb;
    update_pending_text_editor_style();
    return;
  }
  if (document_.snapshot().selected_object_id.has_value()) {
    const auto id = *document_.snapshot().selected_object_id;
    const auto& objects = document_.snapshot().objects;
    const auto object = std::find_if(
        objects.begin(), objects.end(),
        [id](const AnnotationObject& candidate) { return candidate.id == id; });
    if (object != objects.end()) {
      AnnotationStyle style;
      if (object->kind == AnnotationKind::text) {
        const auto previous = std::get<TextStyle>(object->style);
        style = TextStyle{color_srgb_rgb_, previous.font_size_pt};
      } else {
        const auto previous = std::get<ShapeStyle>(object->style);
        style = ShapeStyle{color_srgb_rgb_, previous.line_width_px};
      }
      apply_document_command(RestyleAnnotation{id, style});
    }
  }
}

void OverlayEditorWidget::choose_size(const std::uint16_t value) {
  if (text_editor_ != nullptr && pending_text_style_.has_value()) {
    font_size_pt_ = value;
    pending_text_style_->font_size_pt = value;
    if (pending_text_bounds_.has_value()) {
      pending_text_bounds_ = normalize_text_bounds(
          *pending_text_bounds_, text_editor_->text().toUtf8().toStdString(),
          pending_text_style_);
    }
    update_pending_text_editor_style();
    return;
  }
  if (document_.snapshot().selected_object_id.has_value()) {
    const auto id = *document_.snapshot().selected_object_id;
    const auto& objects = document_.snapshot().objects;
    const auto object = std::find_if(
        objects.begin(), objects.end(),
        [id](const AnnotationObject& candidate) { return candidate.id == id; });
    if (object != objects.end()) {
      if (object->kind == AnnotationKind::text) {
        font_size_pt_ = value;
        const auto previous = std::get<TextStyle>(object->style);
        const auto style = TextStyle{previous.color_srgb_rgb, font_size_pt_};
        apply_document_command(UpdateTextAnnotation{
            id,
            normalize_text_bounds(object->bounds, object->text, style),
            style,
            object->text});
      } else {
        line_width_px_ = value;
        const auto previous = std::get<ShapeStyle>(object->style);
        apply_document_command(RestyleAnnotation{
            id, ShapeStyle{previous.color_srgb_rgb, line_width_px_}});
      }
    }
    return;
  }
  if (tool_ == EditorTool::text) {
    font_size_pt_ = value;
  } else {
    line_width_px_ = value;
  }
}

bool OverlayEditorWidget::apply_document_command(const AnnotationCommand& command) {
  auto result = AnnotationDocument::apply(document_, command);
  if (!result) {
    return false;
  }
  auto plan = [&]() -> Result<AnnotationRenderPlan, Error> {
    const auto& next = result.value().snapshot();
    if (!annotation_render_plan_ ||
        annotation_render_plan_->source_selection_rect_px != selection_.desktop_rect) {
      return AnnotationRenderPlanner::build(next, selection_.desktop_rect,
          point_pixel_scale_, text_rasterizer_);
    }
    AnnotationRenderPlan reused{next.revision, annotation_render_plan_->output_size_px,
        {}, selection_.desktop_rect};
    const auto previous = document_.snapshot();
    for (const auto& object : next.objects) {
      const auto same = std::find(previous.objects.begin(), previous.objects.end(), object);
      const auto layer = std::find_if(annotation_render_plan_->ordered_layers.begin(),
          annotation_render_plan_->ordered_layers.end(),
          [&](const auto& value) { return value.object_id == object.id; });
      if (same != previous.objects.end() && layer != annotation_render_plan_->ordered_layers.end()) {
        reused.ordered_layers.push_back(*layer);
      } else if (draft_annotation_ && *draft_annotation_ == object && transient_annotation_render_plan_) {
        const auto draft = std::find_if(transient_annotation_render_plan_->ordered_layers.begin(),
            transient_annotation_render_plan_->ordered_layers.end(),
            [&](const auto& value) { return value.object_id == object.id; });
        if (draft != transient_annotation_render_plan_->ordered_layers.end()) {
          reused.ordered_layers.push_back(*draft);
        } else return Result<AnnotationRenderPlan, Error>::failure(Error{
            ErrorCode::state_inconsistent, "OverlayEditorWidget", Retryability::never, {}});
      } else {
        auto single = next; single.objects = {object};
        auto built = AnnotationRenderPlanner::build(single, selection_.desktop_rect,
            point_pixel_scale_, text_rasterizer_);
        if (!built) return built;
        reused.ordered_layers.insert(reused.ordered_layers.end(),
            built.value().ordered_layers.begin(), built.value().ordered_layers.end());
      }
    }
    return Result<AnnotationRenderPlan, Error>::success(std::move(reused));
  }();
  if (!plan) {
    show_status(
        QStringLiteral("标注渲染计划无效：%1")
            .arg(QString::fromStdString(to_string(plan.error().code))),
        true);
    return false;
  }
  document_ = std::move(result.value());
  transient_annotation_render_plan_.reset();
  annotation_render_plan_ = std::make_shared<const AnnotationRenderPlan>(
      std::move(plan.value()));
  update_property_controls();
  update();
  notify_preview_changed();
  return true;
}

void OverlayEditorWidget::request_action(const UiCommand command) {
  if (command != UiCommand::cancel_capture && selection_.desktop_rect.empty()) return;
  // Explicit completion, not focus loss: property controls must keep editing
  // the same pending text. Snapshot consumers only see committed model/plan.
  if (command != UiCommand::cancel_capture && !finish_pending_text_edit()) return;
  if (action_requested_) action_requested_(command);
}

void OverlayEditorWidget::notify_preview_changed() {
  if (preview_changed_) {
    preview_changed_(selection_, document_.snapshot(),
        transient_annotation_render_plan_ ? transient_annotation_render_plan_ : annotation_render_plan_);
  }
}

bool OverlayEditorWidget::rebuild_render_plan() {
  if (selection_.desktop_rect.empty()) {
    if (!document_.snapshot().objects.empty()) {
      show_status(QStringLiteral("空选区不能包含标注"), true);
      return false;
    }
    annotation_render_plan_ = std::make_shared<const AnnotationRenderPlan>(
        AnnotationRenderPlan{document_.snapshot().revision, {}, {}, {}});
    return true;
  }
  auto plan = AnnotationRenderPlanner::build(
      document_.snapshot(),
      selection_.desktop_rect,
      point_pixel_scale_,
      text_rasterizer_);
  if (!plan) {
    show_status(
        QStringLiteral("标注渲染计划无效：%1")
            .arg(QString::fromStdString(to_string(plan.error().code))),
        true);
    return false;
  }
  annotation_render_plan_ = std::make_shared<const AnnotationRenderPlan>(
      std::move(plan.value()));
  return true;
}

bool OverlayEditorWidget::apply_selection_candidate(SelectionSnapshot candidate) {
  if (candidate == selection_) {
    return true;
  }
  if (candidate.desktop_rect.empty()) {
    if (!document_.snapshot().objects.empty()) {
      return false;
    }
    selection_ = candidate;
    transient_annotation_render_plan_.reset();
    annotation_render_plan_ = std::make_shared<const AnnotationRenderPlan>(
        AnnotationRenderPlan{document_.snapshot().revision, {}, {}, {}});
    notify_preview_changed();
    return true;
  }

  std::optional<AnnotationRenderPlan> next_plan;
  const bool can_rebase = annotation_render_plan_ != nullptr &&
      annotation_render_plan_->source_document_revision == document_.snapshot().revision &&
      !annotation_render_plan_->source_selection_rect_px.empty();
  if (can_rebase) {
    auto rebased = AnnotationRenderPlanner::rebase(
        *annotation_render_plan_, candidate.desktop_rect);
    if (!rebased) {
      return false;
    }
    next_plan = std::move(rebased.value());
  }
  if (!next_plan.has_value()) {
    auto built = AnnotationRenderPlanner::build(
        document_.snapshot(), candidate.desktop_rect,
        point_pixel_scale_, text_rasterizer_);
    if (!built) {
      return false;
    }
    next_plan = std::move(built.value());
  }

  selection_ = candidate;
  transient_annotation_render_plan_.reset();
  annotation_render_plan_ = std::make_shared<const AnnotationRenderPlan>(
      std::move(*next_plan));
  notify_preview_changed();
  return true;
}

void OverlayEditorWidget::update_property_controls() {
  const AnnotationObject* selected_object = nullptr;
  if (document_.snapshot().selected_object_id.has_value()) {
    const auto id = *document_.snapshot().selected_object_id;
    const auto& objects = document_.snapshot().objects;
    const auto found = std::find_if(
        objects.begin(), objects.end(),
        [id](const AnnotationObject& candidate) { return candidate.id == id; });
    if (found != objects.end()) {
      selected_object = &*found;
    }
  }
  const bool editing_text = text_editor_ != nullptr && pending_text_style_.has_value();
  property_bar_->setVisible(tool_ != EditorTool::select || selected_object != nullptr);
  delete_annotation_button_->setVisible(
      tool_ == EditorTool::select && selected_object != nullptr && text_editor_ == nullptr);
  if (tool_ == EditorTool::select && selected_object == nullptr) {
    layout_controls();
    return;
  }

  bool use_text_sizes = tool_ == EditorTool::text;
  std::uint32_t displayed_color = color_srgb_rgb_;
  std::uint16_t displayed_size = use_text_sizes ? font_size_pt_ : line_width_px_;
  if (editing_text) {
    use_text_sizes = true;
    displayed_color = pending_text_style_->color_srgb_rgb;
    displayed_size = pending_text_style_->font_size_pt;
  } else if (selected_object != nullptr) {
    use_text_sizes = selected_object->kind == AnnotationKind::text;
    if (use_text_sizes) {
      const auto style = std::get<TextStyle>(selected_object->style);
      displayed_color = style.color_srgb_rgb;
      displayed_size = style.font_size_pt;
      color_srgb_rgb_ = displayed_color;
      font_size_pt_ = displayed_size;
    } else {
      const auto style = std::get<ShapeStyle>(selected_object->style);
      displayed_color = style.color_srgb_rgb;
      displayed_size = style.line_width_px;
      color_srgb_rgb_ = displayed_color;
      line_width_px_ = displayed_size;
    }
  }

  const QSignalBlocker blocker(size_combo_);
  size_combo_->clear();
  if (use_text_sizes) {
    for (const auto value : kFontSizes) {
      size_combo_->addItem(QString::number(value) + " pt", value);
    }
    size_combo_->setCurrentText(QString::number(displayed_size) + " pt");
  } else {
    for (const auto value : kLineWidths) {
      size_combo_->addItem(QString::number(value) + " px", value);
    }
    size_combo_->setCurrentText(QString::number(displayed_size) + " px");
  }
  const auto selected_color = std::find(kColors.begin(), kColors.end(), displayed_color);
  if (selected_color != kColors.end()) {
    color_buttons_[static_cast<std::size_t>(selected_color - kColors.begin())]->setChecked(true);
  }
  layout_controls();
}

void OverlayEditorWidget::layout_controls() {
  if (window_selection_ && selection_.desktop_rect.empty()) {
    toolbar_->hide();
    property_bar_->hide();
    delete_annotation_button_->hide();
    return;
  }
  toolbar_->show();
  toolbar_->adjustSize();
  property_bar_->adjustSize();
  constexpr int screen_margin = 8;
  constexpr int selection_gap = 10;
  constexpr int panel_gap = 8;
  const auto property_height = property_bar_->isVisible() ? property_bar_->height() : 0;
  const auto group_height = toolbar_->height() +
      (property_height > 0 ? property_height + panel_gap : 0);
  auto group_top = std::max(screen_margin, height() - group_height - 22);
  auto center_x = width() / 2;
  if (!selection_.desktop_rect.empty()) {
    const auto selection_rect = to_widget_rect(selection_.desktop_rect);
    center_x = static_cast<int>(std::lround(selection_rect.center().x()));
    const auto below = static_cast<int>(std::ceil(selection_rect.bottom())) + selection_gap;
    const auto above = static_cast<int>(std::floor(selection_rect.top())) - selection_gap - group_height;
    if (below + group_height <= height() - screen_margin) {
      group_top = below;
    } else if (above >= screen_margin) {
      group_top = above;
    } else {
      const auto inside_bottom = static_cast<int>(std::floor(selection_rect.bottom())) -
          selection_gap - group_height;
      const auto inside_top = static_cast<int>(std::ceil(selection_rect.top())) + selection_gap;
      group_top = inside_bottom >= screen_margin ? inside_bottom : inside_top;
      group_top = std::clamp(
          group_top, screen_margin, std::max(screen_margin, height() - group_height - screen_margin));
    }
  }

  const auto toolbar_x = std::clamp(
      center_x - toolbar_->width() / 2,
      screen_margin,
      std::max(screen_margin, width() - toolbar_->width() - screen_margin));
  const auto property_x = std::clamp(
      center_x - property_bar_->width() / 2,
      screen_margin,
      std::max(screen_margin, width() - property_bar_->width() - screen_margin));
  if (property_bar_->isVisible()) {
    property_bar_->move(property_x, group_top);
  }
  toolbar_->move(
      toolbar_x,
      group_top + (property_height > 0 ? property_height + panel_gap : 0));
  toolbar_->raise();
  property_bar_->raise();
  layout_annotation_controls();

  if (status_label_->isVisible()) {
    status_label_->setMaximumWidth(std::max(240, std::min(720, width() - 16)));
    status_label_->adjustSize();
    const auto status_x = std::max(8, (width() - status_label_->width()) / 2);
    const auto next_control_y = property_bar_->isVisible() ? property_bar_->y() : toolbar_->y();
    status_label_->move(
        status_x,
        std::max(8, next_control_y - status_label_->height() - 10));
    status_label_->raise();
  }
}

void OverlayEditorWidget::draw_coverage_layer(
    QPainter& painter,
    const AnnotationCoverageLayer& layer) const {
  if (layer.bounds_px.empty()) {
    return;
  }
  QImage image(
      layer.bounds_px.width,
      layer.bounds_px.height,
      QImage::Format_RGBA8888);
  image.fill(Qt::transparent);
  const auto color = qcolor(layer.color_srgb_rgb);
  for (const auto& span : layer.spans) {
    const auto row = span.y - layer.bounds_px.y;
    const auto start = span.x - layer.bounds_px.x;
    if (row < 0 || row >= image.height() || start < 0 ||
        start + static_cast<std::int32_t>(span.coverage_u8.size()) > image.width()) {
      continue;
    }
    auto* pixels = image.scanLine(row);
    for (std::size_t offset = 0; offset < span.coverage_u8.size(); ++offset) {
      const auto column = start + static_cast<std::int32_t>(offset);
      pixels[column * 4] = static_cast<uchar>(color.red());
      pixels[column * 4 + 1] = static_cast<uchar>(color.green());
      pixels[column * 4 + 2] = static_cast<uchar>(color.blue());
      pixels[column * 4 + 3] = span.coverage_u8[offset];
    }
  }
  painter.drawImage(to_widget_output_rect(layer.bounds_px), image);
}

void OverlayEditorWidget::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  // The translucent native child has a persistent backing store on macOS.
  painter.setCompositionMode(QPainter::CompositionMode_Source);
  painter.fillRect(rect(), QColor(0, 0, 0, 0));
  painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

  const auto render_plan = transient_annotation_render_plan_ != nullptr
      ? transient_annotation_render_plan_
      : annotation_render_plan_;
  if (render_plan != nullptr && !clean_content_presented_) {
    for (const auto& layer : render_plan->ordered_layers) {
      draw_coverage_layer(painter, layer);
    }
  }

  for (const auto& object : document_.snapshot().objects) {
    if (document_.snapshot().selected_object_id == object.id) {
      const auto& displayed_object =
          annotation_drag_preview_.has_value() && annotation_drag_preview_->id == object.id
          ? *annotation_drag_preview_
          : object;
      QPen selection_pen(QColor(94, 168, 255), 1.0, Qt::DashLine);
      painter.setPen(selection_pen);
      painter.setBrush(Qt::NoBrush);
      const auto selected_rect = to_widget_rect(displayed_object.bounds).adjusted(-3, -3, 3, 3);
      std::array<QPointF,8> oriented_handles;
      if (is_rotatable(displayed_object)) {
        const auto points=shape_handles(displayed_object,3.0*point_pixel_scale_);
        QPolygonF polygon;
        for(std::size_t i=0;i<points.size();++i) oriented_handles[i]=shape_widget_point(points[i]);
        for(int i : {0,2,4,6}) polygon << oriented_handles[static_cast<std::size_t>(i)];
        painter.setRenderHint(QPainter::Antialiasing);
        painter.drawPolygon(polygon);
      } else painter.drawRect(selected_rect);
      painter.setPen(QPen(QColor(94, 168, 255), 1.0));
      painter.setBrush(QColor(245, 250, 255));
      constexpr double handle_radius = 3.5;
      const auto draw_handle = [&](const QPointF point) {
        painter.drawEllipse(point, handle_radius, handle_radius);
      };
      if (displayed_object.kind == AnnotationKind::arrow &&
          displayed_object.arrow_geometry.has_value()) {
        draw_handle(to_widget_point(displayed_object.arrow_geometry->start));
        draw_handle(to_widget_point(displayed_object.arrow_geometry->end));
      } else if (is_rotatable(displayed_object)) {
        for (const auto point : oriented_handles) draw_handle(point);
      } else {
        draw_handle(selected_rect.topLeft());
        draw_handle(QPointF(selected_rect.center().x(), selected_rect.top()));
        draw_handle(selected_rect.topRight());
        draw_handle(QPointF(selected_rect.right(), selected_rect.center().y()));
        draw_handle(selected_rect.bottomRight());
        draw_handle(QPointF(selected_rect.center().x(), selected_rect.bottom()));
        draw_handle(selected_rect.bottomLeft());
        draw_handle(QPointF(selected_rect.left(), selected_rect.center().y()));
      }
    }
  }

  if (dragging_annotation_ && drag_current_.has_value()) {
    const auto bounds = normalized_rect(drag_anchor_, *drag_current_);
    if (tool_ == EditorTool::text) {
      painter.setPen(QPen(qcolor(color_srgb_rgb_), 1.0, Qt::DashLine));
      painter.setBrush(Qt::NoBrush);
      painter.drawRect(to_widget_rect(bounds));
      return;
    }

  }
}

void OverlayEditorWidget::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  layout_controls();
  if (text_editor_ != nullptr && pending_text_bounds_.has_value()) {
    text_editor_->setGeometry(to_widget_rect(*pending_text_bounds_).toRect());
  }
}

void OverlayEditorWidget::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  // Set the canvas cursor before the first pointer event. Native cursor
  // realization remains Qt's/platform adapter's job, not a global override.
  update_pointer_cursor(mapFromGlobal(QCursor::pos()));
}

void OverlayEditorWidget::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    return;
  }
  if (interaction_started_ && !interaction_started_()) {
    event->accept();
    return;
  }
  last_press_had_selection_ = !selection_.desktop_rect.empty();
  if (window_selection_ && selection_.desktop_rect.empty()) {
    if (!initial_gesture_changed_ || initial_gesture_changed_(true)) {
      const auto previous = initial_highlight();
      window_selection_->press(selection_pointer(event->position()));
      if (!window_selection_->pressed()) finish_initial_gesture();
      if (previous != initial_highlight()) notify_preview_changed();
    }
    event->accept();
    return;
  }
  if (text_editor_ != nullptr) {
    finish_pending_text_edit();
    event->accept();
    return;
  }
  const auto capture_point = to_capture_point(event->position());
  if (tool_ == EditorTool::select) {
    selection_adjustment_.reset();
    annotation_adjustment_.reset();
    annotation_drag_origin_.reset();
    annotation_drag_preview_.reset();
    rotation_drag_.reset();
    transient_annotation_render_plan_.reset();
    if (selection_.desktop_rect.empty()) {
      drag_anchor_ = capture_point;
      drag_current_ = drag_anchor_;
      selection_drag_origin_ = selection_;
      dragging_selection_ = true;
      selection_ = SelectionModel::update_drag(
          selection_, drag_anchor_, drag_anchor_,
          PixelRect{0, 0, capture_size_px_.width, capture_size_px_.height});
      event->accept();
      return;
    }
    if (const auto adjustment = annotation_adjustment_at(event->position());
        adjustment.has_value()) {
      const auto* object = selected_object();
      if (object != nullptr) {
        drag_anchor_ = capture_point;
        drag_current_ = drag_anchor_;
        annotation_adjustment_ = adjustment;
        annotation_drag_origin_ = *object;
        annotation_drag_preview_ = *object;
        if (*adjustment == AnnotationAdjustment::rotate) {
          rotation_drag_.emplace(*object,to_shape_point(event->position()));
          setCursor(rotation_cursor());
        }
        dragging_annotation_transform_ = true;
        event->accept();
        return;
      }
    }
    if (const auto adjustment = selection_adjustment_at(event->position());
        adjustment.has_value()) {
      drag_anchor_ = capture_point;
      drag_current_ = drag_anchor_;
      selection_drag_origin_ = selection_;
      selection_adjustment_ = adjustment;
      dragging_selection_ = true;
      event->accept();
      return;
    }
    const auto object = capture_point_in_selection(capture_point)
        ? hit_test(capture_point)
        : std::nullopt;
    if (object.has_value()) {
      apply_document_command(SelectAnnotation{object});
      event->accept();
      return;
    }
    apply_document_command(SelectAnnotation{std::nullopt});
  } else {
    if (!capture_point_in_selection(capture_point)) {
      event->ignore();
      return;
    }
    if (tool_ == EditorTool::text) {
      const auto object_id = hit_test(capture_point);
      if (object_id.has_value()) {
        const auto& objects = document_.snapshot().objects;
        const auto found = std::find_if(
            objects.begin(), objects.end(),
            [object_id](const AnnotationObject& candidate) {
              return candidate.id == *object_id && candidate.kind == AnnotationKind::text;
            });
        if (found != objects.end()) {
          begin_existing_text_edit(*found);
          event->accept();
          return;
        }
      }
    }
    drag_anchor_ = to_drawing_point(event->position());
    drag_current_ = drag_anchor_;
    dragging_annotation_ = true;
  }
  event->accept();
}

void OverlayEditorWidget::mouseMoveEvent(QMouseEvent* event) {
  if (window_selection_ && selection_.desktop_rect.empty()) {
    refresh_window_candidate(event->position());
    event->accept();
    return;
  }
  if (!dragging_selection_ && !dragging_annotation_ && !dragging_annotation_transform_) {
    update_pointer_cursor(event->position());
    return;
  }
  if (dragging_selection_) {
    drag_current_ = to_capture_point(event->position());
    const auto bounds = PixelRect{0, 0, capture_size_px_.width, capture_size_px_.height};
    const auto candidate = selection_adjustment_.has_value()
        ? SelectionModel::adjust(
              selection_drag_origin_, *selection_adjustment_, drag_anchor_,
              *drag_current_, bounds, 2, annotation_bounds())
        : SelectionModel::update_drag(
              selection_drag_origin_, drag_anchor_, *drag_current_, bounds);
    if (!candidate.desktop_rect.empty()) {
      const auto candidate_applied = apply_selection_candidate(candidate);
      (void)candidate_applied;
    }
    layout_controls();
  } else if (dragging_annotation_transform_) {
    if (rotation_drag_) (void)rotation_drag_->update(to_shape_point(event->position()),active_drawing_bounds());
    update_annotation_drag_preview(to_capture_point(event->position()));
  } else {
    drag_current_ = to_drawing_point(event->position());
    update_new_annotation_preview(*drag_current_);
  }
  update();
  event->accept();
}

void OverlayEditorWidget::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    return;
  }
  if (window_selection_ && selection_.desktop_rect.empty()) {
    const auto previous = initial_highlight();
    const bool pressed = window_selection_->pressed();
    const auto selected = window_selection_->release(selection_pointer(event->position()));
    if (selected) {
      const bool applied = apply_selection_candidate({selection_.revision + 1, *selected});
      if (!applied) {
        window_selection_->reject_confirmation();
        notify_preview_changed();
      }
      update_property_controls();
      layout_controls();
    } else if (previous != initial_highlight()) {
      notify_preview_changed();
    }
    if (pressed) finish_initial_gesture();
    update_pointer_cursor(event->position());
    event->accept();
    return;
  }
  const bool completed_initial_drag = dragging_selection_ && !selection_adjustment_;
  if (dragging_selection_) {
    const auto end = to_capture_point(event->position());
    const auto bounds = PixelRect{0, 0, capture_size_px_.width, capture_size_px_.height};
    const auto candidate = selection_adjustment_.has_value()
        ? SelectionModel::adjust(
              selection_drag_origin_, *selection_adjustment_, drag_anchor_, end, bounds,
              2, annotation_bounds())
        : SelectionModel::update_drag(
              selection_drag_origin_, drag_anchor_, end, bounds);
    dragging_selection_ = false;
    selection_adjustment_.reset();
    const auto candidate_applied = apply_selection_candidate(candidate);
    (void)candidate_applied;
    layout_controls();
  } else if (dragging_annotation_transform_) {
    if (rotation_drag_) (void)rotation_drag_->update(to_shape_point(event->position()),active_drawing_bounds());
    update_annotation_drag_preview(to_capture_point(event->position()));
    const auto original = annotation_drag_origin_;
    const auto preview = annotation_drag_preview_;
    dragging_annotation_transform_ = false;
    annotation_adjustment_.reset();
    annotation_drag_origin_.reset();
    annotation_drag_preview_.reset();
    rotation_drag_.reset();
    transient_annotation_render_plan_.reset();
    if (original.has_value() && preview.has_value() && *original != *preview) {
      if (preview->kind == AnnotationKind::arrow && preview->arrow_geometry.has_value()) {
        apply_document_command(SetArrowGeometry{preview->id, *preview->arrow_geometry});
      } else if (preview->kind == AnnotationKind::text) {
        apply_document_command(UpdateTextAnnotation{
            preview->id,
            preview->bounds,
            std::get<TextStyle>(preview->style),
            preview->text});
      } else {
        apply_document_command(SetShapeGeometry{preview->id, preview->bounds, preview->transform});
      }
    }
    if (clean_content_presented_) notify_preview_changed();
  } else if (dragging_annotation_) {
    const auto end = to_drawing_point(event->position());
    update_new_annotation_preview(end);
    finish_annotation(end);
    draft_annotation_.reset();
    transient_annotation_render_plan_.reset();
    if (clean_content_presented_) notify_preview_changed();
    dragging_annotation_ = false;
  }
  drag_current_.reset();
  if (completed_initial_drag) update_pointer_cursor(event->position());
  update();
  event->accept();
}

std::optional<SelectionAdjustment> OverlayEditorWidget::selection_adjustment_at(
    const QPointF point) const {
  if (selection_.desktop_rect.empty()) {
    return std::nullopt;
  }
  const auto selection_rect = to_widget_rect(selection_.desktop_rect);
  constexpr double tolerance = 8.0;
  const auto extended = selection_rect.adjusted(-tolerance, -tolerance, tolerance, tolerance);
  if (!extended.contains(point)) {
    return std::nullopt;
  }
  const bool near_left = std::abs(point.x() - selection_rect.left()) <= tolerance;
  const bool near_right = std::abs(point.x() - selection_rect.right()) <= tolerance;
  const bool near_top = std::abs(point.y() - selection_rect.top()) <= tolerance;
  const bool near_bottom = std::abs(point.y() - selection_rect.bottom()) <= tolerance;
  if (near_left && near_top) return SelectionAdjustment::north_west;
  if (near_right && near_top) return SelectionAdjustment::north_east;
  if (near_right && near_bottom) return SelectionAdjustment::south_east;
  if (near_left && near_bottom) return SelectionAdjustment::south_west;
  if (near_top) return SelectionAdjustment::north;
  if (near_right) return SelectionAdjustment::east;
  if (near_bottom) return SelectionAdjustment::south;
  if (near_left) return SelectionAdjustment::west;
  return std::nullopt;
}

void OverlayEditorWidget::leaveEvent(QEvent* event) {
  if (window_selection_) {
    const auto previous = initial_highlight();
    window_selection_->leave();
    if (previous != initial_highlight()) notify_preview_changed();
  }
  if (!dragging_selection_ && !dragging_annotation_ && !dragging_annotation_transform_ &&
      !(window_selection_ && window_selection_->pressed())) {
    unsetCursor();
  }
  QWidget::leaveEvent(event);
}

void OverlayEditorWidget::enterEvent(QEnterEvent* event) {
  QWidget::enterEvent(event);
  refresh_window_candidate(event->position());
}

void OverlayEditorWidget::mouseDoubleClickEvent(QMouseEvent* event) {
  // A double click in a selected object's transform zone remains editing.
  if (tool_ == EditorTool::select) {
    const auto adjustment=annotation_adjustment_at(event->position());
    if (adjustment && *adjustment != AnnotationAdjustment::move) {
      event->accept();
      return;
    }
  }
  // The first click can establish a selection. Its second click must not also
  // export it; a later click sequence in an already-confirmed region may do so.
  if (!last_press_had_selection_ || selection_.desktop_rect.empty() ||
      (interaction_started_ && !interaction_started_())) {
    event->accept();
    return;
  }
  if (event->button() == Qt::LeftButton && selection_.desktop_rect.x <= to_capture_point(event->position()).x &&
      selection_.desktop_rect.right() >= to_capture_point(event->position()).x &&
      selection_.desktop_rect.y <= to_capture_point(event->position()).y &&
      selection_.desktop_rect.bottom() >= to_capture_point(event->position()).y && action_requested_) {
    const auto command = InputMapper::map(
        PointerGesture{FocusContext::image_region, PointerTarget::image_region, 2},
        completion_bindings_);
    if (command.has_value()) {
      request_action(*command);
      event->accept();
    }
  }
}

void OverlayEditorWidget::keyPressEvent(QKeyEvent* event) {
  if (suppress_parent_text_key_ &&
      (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Return ||
       event->key() == Qt::Key_Enter)) {
    event->accept();
    return;
  }
  const auto* focused = focusWidget();
  const bool input_focus = qobject_cast<const QLineEdit*>(focused) != nullptr ||
      qobject_cast<const QComboBox*>(focused) != nullptr;
  const auto command = input_platform_.fixed_overlay_command(
      *event, input_focus ? FocusContext::text_editor : FocusContext::overlay,
      completion_bindings_);
  if (!command.has_value()) {
    QWidget::keyPressEvent(event);
    return;
  }
  if (*command == UiCommand::undo) {
    apply_document_command(UndoAnnotation{});
  } else if (*command == UiCommand::redo) {
    apply_document_command(RedoAnnotation{});
  } else {
    request_action(*command);
  }
  event->accept();
}

void OverlayEditorWidget::finish_annotation(const PixelPoint end) {
  auto bounds = normalized_rect(drag_anchor_, end);
  if (tool_ == EditorTool::text && std::abs(end.x - drag_anchor_.x) < 4 &&
      std::abs(end.y - drag_anchor_.y) < 4) {
    bounds = PixelRect{
        drag_anchor_.x,
        drag_anchor_.y,
        static_cast<std::int32_t>(std::ceil(320.0 * point_pixel_scale_)),
        static_cast<std::int32_t>(std::ceil(
            (static_cast<double>(font_size_pt_) * 1.4 + 12.0) * point_pixel_scale_)),
    };
  }
  if (tool_ == EditorTool::text) {
    begin_text_edit(normalize_text_bounds(bounds));
    return;
  }
  const auto minimum_shape_extent = static_cast<std::int32_t>(
      std::ceil(4.0 * point_pixel_scale_));
  if ((tool_ == EditorTool::rectangle || tool_ == EditorTool::ellipse) &&
      (bounds.width < minimum_shape_extent || bounds.height < minimum_shape_extent)) {
    return;
  }
  if (tool_ == EditorTool::arrow &&
      std::hypot(
          static_cast<double>(end.x - drag_anchor_.x),
          static_cast<double>(end.y - drag_anchor_.y)) <
          std::ceil(6.0 * point_pixel_scale_)) {
    return;
  }
  AnnotationObject object;
  object.id = next_object_id_;
  ++next_object_id_.value;
  object.bounds = bounds;
  switch (tool_) {
    case EditorTool::rectangle:
      object.kind = AnnotationKind::rectangle;
      object.style = ShapeStyle{color_srgb_rgb_, line_width_px_};
      break;
    case EditorTool::ellipse:
      object.kind = AnnotationKind::ellipse;
      object.style = ShapeStyle{color_srgb_rgb_, line_width_px_};
      break;
    case EditorTool::arrow:
      object.kind = AnnotationKind::arrow;
      object.style = ShapeStyle{color_srgb_rgb_, line_width_px_};
      object.arrow_geometry = ArrowGeometry{drag_anchor_, end};
      break;
    case EditorTool::text:
      return;
    case EditorTool::select:
      return;
  }
  apply_document_command(CreateAnnotation{std::move(object)});
}

PixelRect OverlayEditorWidget::normalize_text_bounds(
    PixelRect bounds,
    const std::string& text,
    const std::optional<TextStyle> style) const {
  const auto drawing_bounds = active_drawing_bounds();
  const auto effective_style = style.value_or(TextStyle{color_srgb_rgb_, font_size_pt_});
  const auto minimum_height = static_cast<std::int32_t>(std::ceil(
      (static_cast<double>(effective_style.font_size_pt) * 1.4 + 12.0) *
      point_pixel_scale_));
  auto minimum_width = static_cast<std::int32_t>(std::ceil(80.0 * point_pixel_scale_));
  auto measured_height = minimum_height;
  if (text_rasterizer_ != nullptr && !text.empty()) {
    const auto metrics = text_rasterizer_->measure(TextMeasureRequest{
        text, effective_style.font_size_pt, point_pixel_scale_});
    if (metrics) {
      minimum_width = std::max(
          minimum_width, metrics.value().minimum_mask_size_px.width +
              static_cast<std::int32_t>(std::ceil(4.0 * point_pixel_scale_)));
      measured_height = std::max(
          measured_height, metrics.value().minimum_mask_size_px.height +
              static_cast<std::int32_t>(std::ceil(4.0 * point_pixel_scale_)));
    }
  }
  bounds.width = std::max(bounds.width, minimum_width);
  bounds.height = std::max(bounds.height, measured_height);
  bounds.width = std::min(bounds.width, drawing_bounds.width);
  bounds.height = std::min(bounds.height, drawing_bounds.height);
  bounds.x = std::clamp(bounds.x, drawing_bounds.x, drawing_bounds.right() - bounds.width);
  bounds.y = std::clamp(bounds.y, drawing_bounds.y, drawing_bounds.bottom() - bounds.height);
  return bounds;
}

int OverlayEditorWidget::text_editor_font_pixel_size(const std::uint16_t font_size_pt) const {
  const auto capture_to_widget = static_cast<double>(height()) /
      static_cast<double>(std::max(1, capture_size_px_.height));
  return std::max(
      1,
      static_cast<int>(std::lround(
          static_cast<double>(font_size_pt) * point_pixel_scale_ * capture_to_widget)));
}

void OverlayEditorWidget::begin_text_edit(const PixelRect bounds) {
  if (text_editor_ != nullptr) {
    if (!commit_text_edit()) return;
  }
  apply_document_command(SelectAnnotation{std::nullopt});
  editing_text_object_id_.reset();
  pending_text_bounds_ = bounds;
  pending_text_style_ = TextStyle{color_srgb_rgb_, font_size_pt_};
  text_editor_ = new QLineEdit(this);
  text_editor_->setObjectName("annotationTextEditor");
  text_editor_->setPlaceholderText(QStringLiteral("输入文字，Enter 确认，Esc 取消"));
  text_editor_->setGeometry(to_widget_rect(bounds).toRect());
  update_pending_text_editor_style();
  connect(text_editor_, &QLineEdit::returnPressed, this, [this] { commit_text_edit(); });
  auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), text_editor_);
  escape->setContext(Qt::WidgetWithChildrenShortcut);
  connect(escape, &QShortcut::activated, this, [this] { cancel_text_edit(); });
  text_editor_->show();
  text_editor_->raise();
  text_editor_->setFocus();
}

void OverlayEditorWidget::begin_existing_text_edit(const AnnotationObject& object) {
  if (object.kind != AnnotationKind::text) {
    return;
  }
  const auto existing = object;
  if (text_editor_ != nullptr) {
    if (!finish_pending_text_edit()) return;
  }
  apply_document_command(SelectAnnotation{existing.id});
  editing_text_object_id_ = existing.id;
  pending_text_bounds_ = existing.bounds;
  pending_text_style_ = std::get<TextStyle>(existing.style);
  text_editor_ = new QLineEdit(this);
  text_editor_->setObjectName("annotationTextEditor");
  text_editor_->setText(QString::fromUtf8(existing.text));
  text_editor_->setGeometry(to_widget_rect(existing.bounds).toRect());
  update_pending_text_editor_style();
  connect(text_editor_, &QLineEdit::returnPressed, this, [this] { commit_text_edit(); });
  auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), text_editor_);
  escape->setContext(Qt::WidgetWithChildrenShortcut);
  connect(escape, &QShortcut::activated, this, [this] { cancel_text_edit(); });
  text_editor_->show();
  text_editor_->raise();
  text_editor_->setFocus();
  text_editor_->selectAll();
}

bool OverlayEditorWidget::commit_text_edit() {
  if (text_editor_ == nullptr || !pending_text_bounds_.has_value()) {
    return true;
  }
  const auto text = text_editor_->text().trimmed();
  const auto bounds = normalize_text_bounds(
      *pending_text_bounds_, text.toUtf8().toStdString(), pending_text_style_);
  const auto style = pending_text_style_.value_or(TextStyle{color_srgb_rgb_, font_size_pt_});
  const auto editing_id = editing_text_object_id_;
  if (text.isEmpty()) {
    cancel_text_edit();
    return true;
  }
  if (editing_id.has_value()) {
    if (!apply_document_command(UpdateTextAnnotation{
        *editing_id, bounds, style, text.toUtf8().toStdString()})) return false;
  } else {
    AnnotationObject object;
    object.id = next_object_id_;
    object.kind = AnnotationKind::text;
    object.bounds = bounds;
    object.style = style;
    object.text = text.toUtf8().toStdString();
    if (!apply_document_command(CreateAnnotation{std::move(object)})) return false;
    ++next_object_id_.value;
  }
  cancel_text_edit(); // Remove input only after model and pixel plan commit.
  update_property_controls();
  return true;
}

void OverlayEditorWidget::cancel_text_edit() {
  if (text_editor_ == nullptr) {
    return;
  }
  suppress_parent_text_key_ = true;
  QTimer::singleShot(0, this, [this] { suppress_parent_text_key_ = false; });
  text_editor_->deleteLater();
  text_editor_ = nullptr;
  pending_text_bounds_.reset();
  pending_text_style_.reset();
  editing_text_object_id_.reset();
  setFocus();
}

void OverlayEditorWidget::update_pending_text_editor_style() {
  if (text_editor_ == nullptr || !pending_text_style_.has_value()) {
    return;
  }
  auto font = text_editor_->font();
  if (!text_editor_font_family_.empty()) {
    font.setFamily(QString::fromStdString(text_editor_font_family_));
  }
  font.setWeight(static_cast<QFont::Weight>(450));
  font.setHintingPreference(QFont::PreferDefaultHinting);
  font.setStyleStrategy(static_cast<QFont::StyleStrategy>(
      QFont::PreferAntialias | QFont::NoFontMerging));
  font.setPixelSize(text_editor_font_pixel_size(pending_text_style_->font_size_pt));
  text_editor_->setFont(font);
  text_editor_->setStyleSheet(QString(
      "QLineEdit { color: #%1; background: rgba(8,21,37,220);"
      " border: 2px dashed #5EA8FF; border-radius: 5px; padding: 4px 7px; }"
      "QLineEdit::placeholder { color: #C5CCD6; }")
      .arg(pending_text_style_->color_srgb_rgb, 6, 16, QLatin1Char('0')));
  if (pending_text_bounds_.has_value()) {
    text_editor_->setGeometry(to_widget_rect(*pending_text_bounds_).toRect());
  }
  update_property_controls();
}

bool OverlayEditorWidget::finish_pending_text_edit() {
  if (text_editor_ == nullptr) {
    return true;
  }
  if (text_editor_->text().trimmed().isEmpty()) {
    cancel_text_edit();
    return true;
  }
  return commit_text_edit();
}

const AnnotationObject* OverlayEditorWidget::selected_object() const {
  if (!document_.snapshot().selected_object_id.has_value()) {
    return nullptr;
  }
  const auto id = *document_.snapshot().selected_object_id;
  const auto& objects = document_.snapshot().objects;
  const auto found = std::find_if(
      objects.begin(), objects.end(),
      [id](const AnnotationObject& candidate) { return candidate.id == id; });
  return found == objects.end() ? nullptr : &*found;
}

PixelRect OverlayEditorWidget::annotation_bounds() const {
  if (annotation_render_plan_ != nullptr &&
      annotation_render_plan_->source_document_revision == document_.snapshot().revision &&
      annotation_render_plan_->source_selection_rect_px == selection_.desktop_rect) {
    auto left = std::numeric_limits<std::int32_t>::max();
    auto top = std::numeric_limits<std::int32_t>::max();
    auto right = std::numeric_limits<std::int32_t>::min();
    auto bottom = std::numeric_limits<std::int32_t>::min();
    for (const auto& layer : annotation_render_plan_->ordered_layers) {
      for (const auto& span : layer.spans) {
        if (span.coverage_u8.empty()) {
          continue;
        }
        left = std::min(left, span.x);
        top = std::min(top, span.y);
        right = std::max(
            right,
            span.x + static_cast<std::int32_t>(span.coverage_u8.size()));
        bottom = std::max(bottom, span.y + 1);
      }
    }
    if (left != std::numeric_limits<std::int32_t>::max()) {
      return PixelRect{
          selection_.desktop_rect.x + left,
          selection_.desktop_rect.y + top,
          right - left,
          bottom - top};
    }
  }

  const auto& objects = document_.snapshot().objects;
  if (objects.empty()) {
    return {};
  }
  auto left = objects.front().bounds.x;
  auto top = objects.front().bounds.y;
  auto right = objects.front().bounds.right();
  auto bottom = objects.front().bounds.bottom();
  for (const auto& object : objects) {
    left = std::min(left, object.bounds.x);
    top = std::min(top, object.bounds.y);
    right = std::max(right, object.bounds.right());
    bottom = std::max(bottom, object.bounds.bottom());
  }
  return PixelRect{left, top, right - left, bottom - top};
}

std::optional<AnnotationAdjustment> OverlayEditorWidget::annotation_adjustment_at(
    const QPointF point) const {
  const auto* object = selected_object();
  if (object == nullptr || text_editor_ != nullptr) {
    return std::nullopt;
  }
  constexpr double tolerance = 9.0;
  if (is_rotatable(*object)) {
    // Real controls and other selectable objects win over the outside band.
    for (const auto* control : {static_cast<QWidget*>(toolbar_),
         static_cast<QWidget*>(property_bar_), static_cast<QWidget*>(delete_annotation_button_)}) {
      if (control && control->isVisible() && control->geometry().contains(point.toPoint()))
        return std::nullopt;
    }
    auto result=shape_adjustment_at(*object,to_shape_point(point),
        tolerance*point_pixel_scale_,36.0*point_pixel_scale_);
    if (result==AnnotationAdjustment::rotate) {
      const auto other=hit_test(to_capture_point(point));
      if (other && *other!=object->id) return std::nullopt;
    }
    return result;
  }
  const auto near = [point](const QPointF target) {
    return std::hypot(point.x() - target.x(), point.y() - target.y()) <= tolerance;
  };
  if (object->kind == AnnotationKind::arrow && object->arrow_geometry.has_value()) {
    const auto start = to_widget_point(object->arrow_geometry->start);
    const auto end = to_widget_point(object->arrow_geometry->end);
    if (near(start)) return AnnotationAdjustment::arrow_start;
    if (near(end)) return AnnotationAdjustment::arrow_end;
    const auto dx = end.x() - start.x();
    const auto dy = end.y() - start.y();
    const auto length_squared = dx * dx + dy * dy;
    if (length_squared > 0.0) {
      const auto position = std::clamp(
          ((point.x() - start.x()) * dx + (point.y() - start.y()) * dy) /
              length_squared,
          0.0,
          1.0);
      const auto distance = std::hypot(
          point.x() - (start.x() + position * dx),
          point.y() - (start.y() + position * dy));
      if (distance <= tolerance) return AnnotationAdjustment::move;
    }
    return std::nullopt;
  }

  const auto bounds = to_widget_rect(object->bounds);
  const auto extended = bounds.adjusted(-tolerance, -tolerance, tolerance, tolerance);
  if (!extended.contains(point)) {
    return std::nullopt;
  }
  const bool near_left = std::abs(point.x() - bounds.left()) <= tolerance;
  const bool near_right = std::abs(point.x() - bounds.right()) <= tolerance;
  const bool near_top = std::abs(point.y() - bounds.top()) <= tolerance;
  const bool near_bottom = std::abs(point.y() - bounds.bottom()) <= tolerance;
  if (near_left && near_top) return AnnotationAdjustment::north_west;
  if (near_right && near_top) return AnnotationAdjustment::north_east;
  if (near_right && near_bottom) return AnnotationAdjustment::south_east;
  if (near_left && near_bottom) return AnnotationAdjustment::south_west;
  if (near_top) return AnnotationAdjustment::north;
  if (near_right) return AnnotationAdjustment::east;
  if (near_bottom) return AnnotationAdjustment::south;
  if (near_left) return AnnotationAdjustment::west;
  if (bounds.contains(point)) return AnnotationAdjustment::move;
  return std::nullopt;
}

AnnotationObject OverlayEditorWidget::adjusted_annotation(
    const AnnotationObject& origin,
    const AnnotationAdjustment adjustment,
    const PixelPoint anchor,
    const PixelPoint current) const {
  auto result = origin;
  const auto limits = active_drawing_bounds();
  if (is_rotatable(origin)) {
    if (adjustment == AnnotationAdjustment::move)
      return move_shape(origin,{static_cast<double>(current.x-anchor.x),
                                static_cast<double>(current.y-anchor.y)},limits);
    return resize_shape(origin,adjustment,
        {static_cast<double>(anchor.x),static_cast<double>(anchor.y)},
        {static_cast<double>(current.x),static_cast<double>(current.y)},limits,
        std::max(4,static_cast<int>(std::ceil(4.0*point_pixel_scale_))));
  }
  if (adjustment == AnnotationAdjustment::move) {
    const auto requested_x = current.x - anchor.x;
    const auto requested_y = current.y - anchor.y;
    const auto delta_x = std::clamp(
        requested_x, limits.x - origin.bounds.x, limits.right() - origin.bounds.right());
    const auto delta_y = std::clamp(
        requested_y, limits.y - origin.bounds.y, limits.bottom() - origin.bounds.bottom());
    result.bounds.x += delta_x;
    result.bounds.y += delta_y;
    if (result.arrow_geometry.has_value()) {
      result.arrow_geometry->start.x += delta_x;
      result.arrow_geometry->start.y += delta_y;
      result.arrow_geometry->end.x += delta_x;
      result.arrow_geometry->end.y += delta_y;
    }
    return result;
  }

  if ((adjustment == AnnotationAdjustment::arrow_start ||
       adjustment == AnnotationAdjustment::arrow_end) &&
      result.arrow_geometry.has_value()) {
    const auto endpoint = clamp_point(current, limits);
    if (adjustment == AnnotationAdjustment::arrow_start) {
      result.arrow_geometry->start = endpoint;
    } else {
      result.arrow_geometry->end = endpoint;
    }
    if (result.arrow_geometry->start == result.arrow_geometry->end) {
      return origin;
    }
    const auto left = std::min(result.arrow_geometry->start.x, result.arrow_geometry->end.x);
    const auto top = std::min(result.arrow_geometry->start.y, result.arrow_geometry->end.y);
    result.bounds = PixelRect{
        left,
        top,
        std::max(1, std::max(result.arrow_geometry->start.x, result.arrow_geometry->end.x) - left),
        std::max(1, std::max(result.arrow_geometry->start.y, result.arrow_geometry->end.y) - top)};
    return result;
  }

  auto minimum_width = std::max(
      4, static_cast<std::int32_t>(std::ceil(4.0 * point_pixel_scale_)));
  auto minimum_height = minimum_width;
  if (origin.kind == AnnotationKind::text) {
    const auto style = std::get<TextStyle>(origin.style);
    const auto normalized = normalize_text_bounds(
        PixelRect{origin.bounds.x, origin.bounds.y, 1, 1}, origin.text, style);
    minimum_width = normalized.width;
    minimum_height = normalized.height;
  }
  auto left = origin.bounds.x;
  auto top = origin.bounds.y;
  auto right = origin.bounds.right();
  auto bottom = origin.bounds.bottom();
  const bool adjust_left = adjustment == AnnotationAdjustment::west ||
      adjustment == AnnotationAdjustment::north_west ||
      adjustment == AnnotationAdjustment::south_west;
  const bool adjust_right = adjustment == AnnotationAdjustment::east ||
      adjustment == AnnotationAdjustment::north_east ||
      adjustment == AnnotationAdjustment::south_east;
  const bool adjust_top = adjustment == AnnotationAdjustment::north ||
      adjustment == AnnotationAdjustment::north_west ||
      adjustment == AnnotationAdjustment::north_east;
  const bool adjust_bottom = adjustment == AnnotationAdjustment::south ||
      adjustment == AnnotationAdjustment::south_west ||
      adjustment == AnnotationAdjustment::south_east;
  if (adjust_left) {
    left = std::clamp(current.x, limits.x, right - minimum_width);
  }
  if (adjust_right) {
    right = std::clamp(current.x, left + minimum_width, limits.right());
  }
  if (adjust_top) {
    top = std::clamp(current.y, limits.y, bottom - minimum_height);
  }
  if (adjust_bottom) {
    bottom = std::clamp(current.y, top + minimum_height, limits.bottom());
  }
  result.bounds = PixelRect{left, top, right - left, bottom - top};
  return result;
}

void OverlayEditorWidget::update_new_annotation_preview(const PixelPoint current) {
  if (tool_ == EditorTool::text || !annotation_render_plan_) return;
  AnnotationObject preview;
  preview.id = next_object_id_;
  preview.bounds = normalized_rect(drag_anchor_, current);
  preview.style = ShapeStyle{color_srgb_rgb_, line_width_px_};
  preview.kind = tool_ == EditorTool::ellipse ? AnnotationKind::ellipse
      : tool_ == EditorTool::arrow ? AnnotationKind::arrow : AnnotationKind::rectangle;
  if (preview.kind == AnnotationKind::arrow) preview.arrow_geometry = ArrowGeometry{drag_anchor_, current};
  if (draft_annotation_ && *draft_annotation_ == preview) return;
  auto built = AnnotationRenderPlanner::build(
      AnnotationDocumentSnapshot{document_.snapshot().revision, {preview}, std::nullopt, false, false},
      selection_.desktop_rect, point_pixel_scale_);
  if (!built || built.value().ordered_layers.empty()) return;
  auto next = *annotation_render_plan_;
  next.ordered_layers.push_back(std::move(built.value().ordered_layers.front()));
  draft_annotation_ = preview;
  transient_annotation_render_plan_ = std::make_shared<const AnnotationRenderPlan>(std::move(next));
  if (clean_content_presented_) notify_preview_changed();
}

void OverlayEditorWidget::update_annotation_drag_preview(const PixelPoint current) {
  if (!annotation_drag_origin_.has_value() || !annotation_adjustment_.has_value()) {
    return;
  }
  auto preview = rotation_drag_
      ? rotation_drag_->preview()
      : adjusted_annotation(*annotation_drag_origin_, *annotation_adjustment_, drag_anchor_, current);
  auto snapshot = document_.snapshot();
  const auto found = std::find_if(
      snapshot.objects.begin(), snapshot.objects.end(),
      [&preview](const AnnotationObject& candidate) { return candidate.id == preview.id; });
  if (found == snapshot.objects.end()) {
    return;
  }
  *found = preview;
  if (annotation_render_plan_ == nullptr ||
      annotation_render_plan_->source_document_revision != snapshot.revision ||
      annotation_render_plan_->source_selection_rect_px != selection_.desktop_rect) {
    return;
  }
  auto preview_snapshot = snapshot;
  preview_snapshot.objects = {preview};
  auto preview_plan = AnnotationRenderPlanner::build(
      preview_snapshot, selection_.desktop_rect,
      point_pixel_scale_, text_rasterizer_);
  if (!preview_plan || preview_plan.value().ordered_layers.size() != 1U) {
    return;
  }
  auto next_plan = *annotation_render_plan_;
  const auto layer = std::find_if(
      next_plan.ordered_layers.begin(), next_plan.ordered_layers.end(),
      [&preview](const AnnotationCoverageLayer& value) {
        return value.object_id == preview.id;
      });
  if (layer == next_plan.ordered_layers.end()) {
    return;
  }
  *layer = std::move(preview_plan.value().ordered_layers.front());
  annotation_drag_preview_ = std::move(preview);
  transient_annotation_render_plan_ = std::make_shared<const AnnotationRenderPlan>(
      std::move(next_plan));
  drag_current_ = current;
  layout_annotation_controls();
  if (clean_content_presented_) notify_preview_changed();
}

void OverlayEditorWidget::update_pointer_cursor(const QPointF point) {
  if (interaction_locked_) {
    setCursor(Qt::ArrowCursor);
    return;
  }
  // Preview bounds are not selection edges. Keep the positioning crosshair
  // through the initial drag, including the manual/no-catalog fallback.
  if (selection_.desktop_rect.empty() ||
      (dragging_selection_ && !selection_adjustment_)) {
    setCursor(Qt::CrossCursor);
    return;
  }
  const auto annotation_adjustment = tool_ == EditorTool::select
      ? annotation_adjustment_at(point)
      : std::nullopt;
  if (annotation_adjustment.has_value()) {
    const auto* object=selected_object();
    if (object && is_rotatable(*object)) {
      if (*annotation_adjustment==AnnotationAdjustment::rotate) { setCursor(rotation_cursor()); return; }
      double local_angle=0;
      bool resize=true;
      switch (*annotation_adjustment) {
        case AnnotationAdjustment::north: case AnnotationAdjustment::south: local_angle=std::numbers::pi/2; break;
        case AnnotationAdjustment::east: case AnnotationAdjustment::west: break;
        case AnnotationAdjustment::north_west: case AnnotationAdjustment::south_east: local_angle=std::numbers::pi/4; break;
        case AnnotationAdjustment::north_east: case AnnotationAdjustment::south_west: local_angle=-std::numbers::pi/4; break;
        default: resize=false; break;
      }
      if(resize) {
        const auto sector=static_cast<int>(std::lround(
            (object->transform.rotation_radians+local_angle)/(std::numbers::pi/4)));
        constexpr std::array<Qt::CursorShape,4> shapes{
            Qt::SizeHorCursor,Qt::SizeFDiagCursor,Qt::SizeVerCursor,Qt::SizeBDiagCursor};
        setCursor(shapes[static_cast<std::size_t>((sector%4+4)%4)]); return;
      }
    }
    switch (*annotation_adjustment) {
      case AnnotationAdjustment::rotate: setCursor(rotation_cursor()); return;
      case AnnotationAdjustment::move: setCursor(Qt::SizeAllCursor); return;
      case AnnotationAdjustment::north:
      case AnnotationAdjustment::south: setCursor(Qt::SizeVerCursor); return;
      case AnnotationAdjustment::east:
      case AnnotationAdjustment::west: setCursor(Qt::SizeHorCursor); return;
      case AnnotationAdjustment::north_west:
      case AnnotationAdjustment::south_east: setCursor(Qt::SizeFDiagCursor); return;
      case AnnotationAdjustment::north_east:
      case AnnotationAdjustment::south_west: setCursor(Qt::SizeBDiagCursor); return;
      case AnnotationAdjustment::arrow_start:
      case AnnotationAdjustment::arrow_end: setCursor(Qt::CrossCursor); return;
    }
  }
  if (tool_ == EditorTool::select) {
    if (const auto adjustment = selection_adjustment_at(point); adjustment.has_value()) {
      switch (*adjustment) {
        case SelectionAdjustment::north:
        case SelectionAdjustment::south: setCursor(Qt::SizeVerCursor); return;
        case SelectionAdjustment::east:
        case SelectionAdjustment::west: setCursor(Qt::SizeHorCursor); return;
        case SelectionAdjustment::north_west:
        case SelectionAdjustment::south_east: setCursor(Qt::SizeFDiagCursor); return;
        case SelectionAdjustment::north_east:
        case SelectionAdjustment::south_west: setCursor(Qt::SizeBDiagCursor); return;
      }
    }
    unsetCursor();
    return;
  }
  setCursor(capture_point_in_selection(to_capture_point(point))
      ? Qt::CrossCursor
      : Qt::ArrowCursor);
}

void OverlayEditorWidget::layout_annotation_controls() {
  const auto* object = selected_object();
  if (delete_annotation_button_ == nullptr || object == nullptr ||
      tool_ != EditorTool::select || text_editor_ != nullptr) {
    if (delete_annotation_button_ != nullptr) {
      delete_annotation_button_->hide();
    }
    return;
  }
  const auto& displayed = annotation_drag_preview_.has_value() &&
          annotation_drag_preview_->id == object->id
      ? *annotation_drag_preview_
      : *object;
  const auto bounds = to_widget_rect(is_rotatable(displayed) ? shape_pixel_bounds(displayed) : displayed.bounds);
  const auto x = std::clamp(
      static_cast<int>(std::lround(bounds.right())) - delete_annotation_button_->width() / 2,
      4,
      std::max(4, width() - delete_annotation_button_->width() - 4));
  auto y = static_cast<int>(std::lround(bounds.top())) -
      delete_annotation_button_->height() - 8;
  if (y < 4) {
    y = std::min(
        height() - delete_annotation_button_->height() - 4,
        static_cast<int>(std::lround(bounds.top())) + 8);
  }
  delete_annotation_button_->move(x, std::max(4, y));
  delete_annotation_button_->show();
  delete_annotation_button_->raise();
}

std::optional<ObjectId> OverlayEditorWidget::hit_test(const PixelPoint point) const {
  const auto& objects = document_.snapshot().objects;
  for (auto iterator = objects.rbegin(); iterator != objects.rend(); ++iterator) {
    if (is_rotatable(*iterator)) {
      if (shape_contains(*iterator,{static_cast<double>(point.x),static_cast<double>(point.y)})) return iterator->id;
      continue;
    }
    const auto& bounds = iterator->bounds;
    if (point.x >= bounds.x && point.x <= bounds.right() &&
        point.y >= bounds.y && point.y <= bounds.bottom()) {
      return iterator->id;
    }
  }
  return std::nullopt;
}

PixelPoint OverlayEditorWidget::to_capture_point(const QPointF point) const {
  return PixelPoint{
      std::clamp(
          static_cast<std::int32_t>(point.x() * capture_size_px_.width / std::max(1, width())),
          0,
          capture_size_px_.width),
      std::clamp(
          static_cast<std::int32_t>(point.y() * capture_size_px_.height / std::max(1, height())),
          0,
          capture_size_px_.height),
  };
}

ShapePoint OverlayEditorWidget::to_shape_point(QPointF point) const {
  return {std::clamp(point.x()*capture_size_px_.width/std::max(1,width()),0.0,static_cast<double>(capture_size_px_.width)),
          std::clamp(point.y()*capture_size_px_.height/std::max(1,height()),0.0,static_cast<double>(capture_size_px_.height))};
}
QPointF OverlayEditorWidget::shape_widget_point(ShapePoint point) const {
  return {point.x*width()/std::max(1,capture_size_px_.width),
          point.y*height()/std::max(1,capture_size_px_.height)};
}

PixelPoint OverlayEditorWidget::to_drawing_point(const QPointF point) const {
  return clamp_point(to_capture_point(point), active_drawing_bounds());
}

PixelPoint OverlayEditorWidget::output_to_capture_point(const PixelPoint point) const {
  return PixelPoint{
      selection_.desktop_rect.x + point.x,
      selection_.desktop_rect.y + point.y,
  };
}

bool OverlayEditorWidget::capture_point_in_selection(const PixelPoint point) const {
  return point.x >= selection_.desktop_rect.x && point.x <= selection_.desktop_rect.right() &&
      point.y >= selection_.desktop_rect.y && point.y <= selection_.desktop_rect.bottom();
}

QPointF OverlayEditorWidget::to_widget_point(const PixelPoint point) const {
  return QPointF{
      static_cast<double>(point.x) * width() / std::max(1, capture_size_px_.width),
      static_cast<double>(point.y) * height() / std::max(1, capture_size_px_.height),
  };
}

QRectF OverlayEditorWidget::to_widget_rect(const PixelRect rect) const {
  const auto top_left = to_widget_point(PixelPoint{rect.x, rect.y});
  const auto bottom_right = to_widget_point(PixelPoint{rect.right(), rect.bottom()});
  return QRectF(top_left, bottom_right).normalized();
}

QRectF OverlayEditorWidget::to_widget_output_rect(const PixelRect rect) const {
  const auto top_left = output_to_capture_point(PixelPoint{rect.x, rect.y});
  const auto bottom_right = output_to_capture_point(
      PixelPoint{rect.right(), rect.bottom()});
  return QRectF(to_widget_point(top_left), to_widget_point(bottom_right)).normalized();
}

PixelRect OverlayEditorWidget::active_drawing_bounds() const {
  return selection_.desktop_rect;
}

}  // namespace hdrshot
