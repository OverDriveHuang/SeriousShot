#include "ui/qt/toolbar_style.hpp"
#include "ui/qt/annotation_size_combo.hpp"
#include "ui/qt/status_message_style.hpp"
#include "ui/qt/overlay_editor_widget.hpp"
#include "ports/annotation_font_contract.hpp"
#include "ui/qt/toolbar_symbol_button.hpp"

#include <QComboBox>
#include <QAbstractItemView>
#include <QStyle>
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

const auto kColors = annotation_palette_srgb();
constexpr std::array<EditorTool, 7> kTools{
    EditorTool::select, EditorTool::rectangle, EditorTool::ellipse,
    EditorTool::line,   EditorTool::arrow,     EditorTool::text,
    EditorTool::brush};

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
#ifndef Q_OS_WIN
  // Windows already has one translucent native host above the HDR underlay.
  // Native child canvases/controls split its backing-store flushes: async
  // magnifier labels can be updated in memory while the screen keeps "--".
  // Keep all Windows editor controls in that host; preserve macOS surfaces.
  setAttribute(Qt::WA_NativeWindow);
#endif
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

void OverlayEditorWidget::set_pin_available(bool available) {
  toolbar_->findChild<QToolButton*>("actionPinScreenshot")->setVisible(available);
  toolbar_->findChild<QFrame*>("separatorBeforePin")->setVisible(available);
  layout_controls();
}

void OverlayEditorWidget::set_completion_bindings(const CompletionBindings bindings) {
  completion_bindings_ = bindings;
}

void OverlayEditorWidget::set_interaction_started(InteractionStarted callback) {
  interaction_started_ = std::move(callback);
}

void OverlayEditorWidget::set_interaction_locked(bool locked) {
  if (locked) interrupt_pointer_gesture();
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
  pending_annotation_move_.reset();
  brush_builder_.cancel();
  brush_active_ = false;
  brush_preview_scheduled_ = false;
  pending_brush_points_.clear();
  pending_reuse_.reset();
  ++annotation_preview_epoch_;
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
  status_label_->setStyleSheet(status_message_style(palette(), is_error));
  status_label_->setText(message);
  status_label_->setVisible(true);
  status_label_->adjustSize();
  layout_controls();
  status_label_->raise();
  QTimer::singleShot(status_message_duration_ms, status_label_, [label = status_label_] {
    label->setVisible(false);
  });
}

void OverlayEditorWidget::build_controls() {
  toolbar_ = new QFrame(this);
  toolbar_->setObjectName("editorToolbar");
  // Controls must not inherit the drawing canvas's crosshair/resize cursor.
  toolbar_->setCursor(Qt::ArrowCursor);
#ifndef Q_OS_WIN
  toolbar_->setAttribute(Qt::WA_NativeWindow);
#endif
  toolbar_layout_ = new QHBoxLayout(toolbar_);
  toolbar_layout_->setContentsMargins(toolbar_style::horizontal_margin, toolbar_style::vertical_margin, toolbar_style::horizontal_margin, toolbar_style::vertical_margin);
  toolbar_layout_->setSpacing(toolbar_style::spacing);

  constexpr std::array<const char *, 7> labels{"选择", "矩形", "圆形", "直线",
                                               "箭头", "文字", "画笔"};
  constexpr std::array<const char *, 7> object_names{
      "toolSelect", "toolRectangle", "toolEllipse", "toolLine",
      "toolArrow",  "toolText",      "toolBrush"};
  constexpr std::array<ToolbarSymbol, 7> symbols{
      ToolbarSymbol::select, ToolbarSymbol::rectangle, ToolbarSymbol::ellipse,
      ToolbarSymbol::line,   ToolbarSymbol::arrow,     ToolbarSymbol::text,
      ToolbarSymbol::brush,
  };
  const auto add_separator = [this](const char* object_name) {
    auto* separator = new QFrame(toolbar_);
    separator->setObjectName(QString::fromLatin1(object_name));
    separator->setProperty("toolbarSeparator", true);
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Plain);
    separator->setFixedSize(toolbar_style::separator_width, toolbar_style::separator_height);
    toolbar_layout_->addWidget(separator);
  };
  for (std::size_t index = 0; index < labels.size(); ++index) {
    auto* button = new ToolbarSymbolButton(symbols[index], false, toolbar_);
    button->setObjectName(QString::fromLatin1(object_names[index]));
    button->setCheckable(true);
    button->setAutoExclusive(true);
    button->setFixedSize(toolbar_style::button_size, toolbar_style::button_size);
    button->setToolTip(QString::fromUtf8(labels[index]));
    button->setAccessibleName(QString::fromUtf8(labels[index]));
    const auto tool = kTools[index];
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
      case UiCommand::pin_screenshot:
        symbol = ToolbarSymbol::pin_create;
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
      case UiCommand::pin_screenshot: button->setObjectName("actionPinScreenshot"); break;
      default: break;
    }
    button->setProperty("primary", primary);
    button->setProperty("cancelAction", command == UiCommand::cancel_capture);
    if (command == UiCommand::analyze) {
      button->setProperty("analysisAction", true);
      button->hide(); // only exposed when an actual analysis service is injected
    }
    if (command == UiCommand::pin_screenshot) {
      button->hide(); // only exposed when an actual pin service is injected
    }
    button->setToolTip(QString::fromUtf8(label));
    button->setAccessibleName(QString::fromUtf8(label));
    button->setFixedSize(toolbar_style::button_size, toolbar_style::button_size);
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
  add_separator("separatorBeforePin");
  toolbar_->findChild<QFrame*>("separatorBeforePin")->hide();
  add_action("新建贴图窗口", UiCommand::pin_screenshot);
  add_separator("separatorBeforeCancel");
  add_action("取消", UiCommand::cancel_capture);

  property_bar_ = new QFrame(this);
  property_bar_->setObjectName("propertyBar");
  property_bar_->setCursor(Qt::ArrowCursor);
#ifndef Q_OS_WIN
  property_bar_->setAttribute(Qt::WA_NativeWindow);
#endif
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
  size_combo_ = new AnnotationSizeCombo(property_bar_);
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
  const auto style = toolbar_style::sheet(dark);
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
  if (tool != tool_ && (dragging_annotation_ || dragging_annotation_transform_))
    interrupt_pointer_gesture();
  if (text_editor_ != nullptr && tool != EditorTool::text) {
    if (!finish_pending_text_edit()) return;
  }
  if (tool != tool_ && tool != EditorTool::select &&
      document_.snapshot().selected_object_id)
    if (!apply_document_command(SelectAnnotation{std::nullopt}))
      return;
  tool_ = tool;
  update_pointer_cursor(mapFromGlobal(QCursor::pos()));
  for (std::size_t i = 0; i < kTools.size(); ++i)
    tool_buttons_[i]->setChecked(kTools[i] == tool);
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
        style = TextStyle{color_srgb_rgb_, previous.font_size_dip};
      } else if (object->kind == AnnotationKind::brush) {
        style = BrushStyle{color_srgb_rgb_,
                           std::get<BrushStyle>(object->style).diameter_dip};
      } else {
        const auto previous = std::get<ShapeStyle>(object->style);
        style = ShapeStyle{color_srgb_rgb_, previous.line_width_dip};
      }
      apply_document_command(RestyleAnnotation{id, style});
    }
  }
}

void OverlayEditorWidget::choose_size(const std::uint16_t value) {
  if (text_editor_ != nullptr && pending_text_style_.has_value()) {
    font_size_dip_ = value;
    pending_text_style_->font_size_dip = value;
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
        font_size_dip_ = value;
        const auto previous = std::get<TextStyle>(object->style);
        const auto style = TextStyle{previous.color_srgb_rgb, font_size_dip_};
        apply_document_command(UpdateTextAnnotation{
            id,
            normalize_text_bounds(object->bounds, object->text, style),
            style,
            object->text});
      } else if (object->kind == AnnotationKind::brush) {
        brush_diameter_dip_ = value;
        apply_document_command(RestyleAnnotation{
            id, BrushStyle{std::get<BrushStyle>(object->style).color_srgb_rgb,
                           value}});
      } else {
        line_width_dip_ = value;
        const auto previous = std::get<ShapeStyle>(object->style);
        apply_document_command(RestyleAnnotation{
            id, ShapeStyle{previous.color_srgb_rgb, line_width_dip_}});
      }
    }
    return;
  }
  if (tool_ == EditorTool::text) {
    font_size_dip_ = value;
  } else if (tool_ == EditorTool::brush) {
    brush_diameter_dip_ = value;
  } else {
    line_width_dip_ = value;
  }
}

bool OverlayEditorWidget::apply_document_command(const AnnotationCommand& command) {
  flush_pending_annotation_move();
  if (std::holds_alternative<UndoAnnotation>(command) ||
      std::holds_alternative<RedoAnnotation>(command) ||
      std::holds_alternative<DeleteAnnotation>(command))
    interrupt_pointer_gesture();
  auto result = prepare_annotation_edit(
      document_, annotation_render_plan_.get(), command, edit_context(),
      pending_reuse_ ? &*pending_reuse_ : nullptr);
  if (!result) {
    show_status(
        QStringLiteral("标注渲染计划无效：%1")
            .arg(QString::fromStdString(to_string(result.error().code))),
        true);
    return false;
  }
  ++annotation_preview_epoch_;
  document_ = std::move(result.value().document);
  annotation_render_plan_ = std::move(result.value().plan);
  transient_annotation_render_plan_.reset();
  update_property_controls();
  update();
  notify_preview_changed();
  return true;
}

void OverlayEditorWidget::request_action(const UiCommand command) {
  if (command == UiCommand::cancel_capture)
    interrupt_pointer_gesture();
  else if (!finish_pending_pointer_edit())
    return;
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
    auto prepared = prepare_selection_change(document_, nullptr, selection_,
                                             edit_context());
    if (!prepared)
      return false;
    annotation_render_plan_ = std::move(prepared.value().plan);
    return true;
  }
  auto plan = AnnotationRenderPlanner::build(
      document_.snapshot(), selection_.desktop_rect, point_pixel_scale_,
      text_rasterizer_, &annotation_coverage_cache_);
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

  auto prepared = prepare_selection_change(
      document_, annotation_render_plan_.get(), candidate, edit_context());
  if (!prepared)
    return false;
  selection_ = prepared.value().selection;
  annotation_render_plan_ = std::move(prepared.value().plan);
  transient_annotation_render_plan_.reset();
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
  bool use_brush_sizes = tool_ == EditorTool::brush;
  std::uint32_t displayed_color = color_srgb_rgb_;
  std::uint16_t displayed_size = use_text_sizes    ? font_size_dip_
                                 : use_brush_sizes ? brush_diameter_dip_
                                                   : line_width_dip_;
  if (editing_text) {
    use_text_sizes = true;
    use_brush_sizes = false;
    displayed_color = pending_text_style_->color_srgb_rgb;
    displayed_size = pending_text_style_->font_size_dip;
  } else if (selected_object != nullptr) {
    use_text_sizes = selected_object->kind == AnnotationKind::text;
    use_brush_sizes = selected_object->kind == AnnotationKind::brush;
    if (use_text_sizes) {
      const auto style = std::get<TextStyle>(selected_object->style);
      displayed_color = style.color_srgb_rgb;
      displayed_size = style.font_size_dip;
      color_srgb_rgb_ = displayed_color;
      font_size_dip_ = displayed_size;
    } else if (use_brush_sizes) {
      auto style = std::get<BrushStyle>(selected_object->style);
      displayed_color = style.color_srgb_rgb;
      displayed_size = style.diameter_dip;
      brush_diameter_dip_ = displayed_size;
      color_srgb_rgb_ = displayed_color;
    } else {
      const auto style = std::get<ShapeStyle>(selected_object->style);
      displayed_color = style.color_srgb_rgb;
      displayed_size = style.line_width_dip;
      color_srgb_rgb_ = displayed_color;
      line_width_dip_ = displayed_size;
    }
  }

  const QSignalBlocker blocker(size_combo_);
  size_combo_->clear();
  for (auto value :
       annotation_sizes_dip(use_text_sizes    ? AnnotationKind::text
                            : use_brush_sizes ? AnnotationKind::brush
                                              : AnnotationKind::rectangle))
    size_combo_->addItem(QString::number(value) + " DIP", value);
  size_combo_->setCurrentText(QString::number(displayed_size) + " DIP");
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
        start + static_cast<std::int32_t>(span.length()) > image.width()) {
      continue;
    }
    auto* pixels = image.scanLine(row);
    for (std::size_t offset = 0; offset < span.length(); ++offset) {
      const auto column = start + static_cast<std::int32_t>(offset);
      pixels[column * 4] = static_cast<uchar>(color.red());
      pixels[column * 4 + 1] = static_cast<uchar>(color.green());
      pixels[column * 4 + 2] = static_cast<uchar>(color.blue());
      pixels[column * 4 + 3] = span.coverage_at(offset);
    }
  }
  painter.drawImage(to_widget_output_rect(layer.bounds_px), image);
}

void OverlayEditorWidget::paintEvent(QPaintEvent *event) {
  QPainter painter(this);
  // The translucent native child has a persistent backing store on macOS.
  painter.setCompositionMode(QPainter::CompositionMode_Source);
  painter.fillRect(event->rect(), QColor(0, 0, 0, 0));
  painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
  // Clear old marks in the new hole before clipping new editor decoration.
  // The image itself belongs to the native HDR underlay.
  if(!magnifier_exclusion_.isEmpty())painter.setClipRegion(QRegion(rect()).subtracted(QRegion(magnifier_exclusion_)));

  const auto render_plan = transient_annotation_render_plan_ != nullptr
      ? transient_annotation_render_plan_
      : annotation_render_plan_;
  if (render_plan != nullptr && !clean_content_presented_) {
    if (painted_plan_ != render_plan || painted_coverage_.size() != size()) {
      painted_coverage_ = QImage(size(), QImage::Format_ARGB32_Premultiplied);
      painted_coverage_.fill(Qt::transparent);
      QPainter coverage_painter(&painted_coverage_);
      for (const auto &layer : render_plan->ordered_layers)
        draw_coverage_layer(coverage_painter, layer);
      painted_plan_ = render_plan;
    }
    painter.drawImage(QPoint(0, 0), painted_coverage_);
  }

  if (tool_ == EditorTool::brush && brush_cursor_position_) {
    auto diameter = brush_diameter_dip_ * point_pixel_scale_ * width() /
                    std::max(1, capture_size_px_.width);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Qt::black, 3));
    painter.drawEllipse(*brush_cursor_position_, diameter / 2, diameter / 2);
    painter.setPen(QPen(Qt::white, 1));
    painter.drawEllipse(*brush_cursor_position_, diameter / 2, diameter / 2);
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
      if ((displayed_object.kind == AnnotationKind::arrow ||
           displayed_object.kind == AnnotationKind::line) &&
          displayed_object.arrow_geometry.has_value()) {
        draw_handle(to_widget_point(displayed_object.arrow_geometry->start));
        draw_handle(to_widget_point(displayed_object.arrow_geometry->end));
      } else if (displayed_object.kind == AnnotationKind::brush &&
                 displayed_object.brush_geometry) {
        auto b = brush_centerline_bounds(*displayed_object.brush_geometry);
        auto tl = shape_widget_point({b.left, b.top}),
             br = shape_widget_point({b.right, b.bottom});
        bool x = b.right > b.left, y = b.bottom > b.top;
        if (x && y) {
          draw_handle(tl);
          draw_handle({br.x(), tl.y()});
          draw_handle(br);
          draw_handle({tl.x(), br.y()});
        }
        if (x) {
          draw_handle({tl.x(), (tl.y() + br.y()) / 2});
          draw_handle({br.x(), (tl.y() + br.y()) / 2});
        }
        if (y) {
          draw_handle({(tl.x() + br.x()) / 2, tl.y()});
          draw_handle({(tl.x() + br.x()) / 2, br.y()});
        }
        if (!x && !y)
          draw_handle(tl);
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
  pending_annotation_move_.reset();
  ++annotation_preview_epoch_;
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
        brush_drag_anchor_ = to_brush_point(event->position());
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
    if (tool_ != EditorTool::brush &&
        !capture_point_in_selection(capture_point)) {
      event->ignore();
      return;
    }
    if (tool_ == EditorTool::brush) {
      begin_brush_stroke(event->position());
      event->accept();
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

void OverlayEditorWidget::flush_pending_annotation_move() {
  if (!pending_annotation_move_) return;
  const auto position = *pending_annotation_move_;
  pending_annotation_move_.reset();
  if (!isEnabled() || interaction_locked_) return;
  QMouseEvent move(QEvent::MouseMove, position, mapToGlobal(position),
                   Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
  processing_annotation_move_ = true;
  mouseMoveEvent(&move);
  processing_annotation_move_ = false;
}

void OverlayEditorWidget::mouseMoveEvent(QMouseEvent* event) {
  if (brush_active_) {
    if (brush_builder_.sealed()) {
      event->accept();
      return;
    }
    last_brush_point_ = to_brush_point(event->position());
    pending_brush_points_.push_back(last_brush_point_);
    if (!brush_preview_scheduled_) {
      brush_preview_scheduled_ = true;
      const auto epoch = annotation_preview_epoch_;
      QTimer::singleShot(0, this, [this, epoch] {
        if (epoch == annotation_preview_epoch_) {
          brush_preview_scheduled_ = false;
          flush_pending_brush_preview();
        }
      });
    }
    update_pointer_cursor(event->position());
    event->accept();
    return;
  }
  if (dragging_annotation_transform_ && annotation_drag_origin_ &&
      annotation_drag_origin_->kind == AnnotationKind::brush) {
    update_brush_transform(event->position());
    event->accept();
    return;
  }
  // Rotation has path-dependent boundary latching; keep its event sequence.
  if (coalesce_annotation_moves_ && !processing_annotation_move_ &&
      !rotation_drag_ &&
      (dragging_annotation_ || dragging_annotation_transform_)) {
    pending_annotation_move_ = event->position();
    if (!annotation_move_scheduled_) {
      annotation_move_scheduled_ = true;
      QTimer::singleShot(0, this, [this] {
        annotation_move_scheduled_ = false;
        flush_pending_annotation_move();
      });
    }
    event->accept();
    return;
  }
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
    if (rotation_drag_)
      (void)rotation_drag_->update(to_shape_point(event->position()),
                                   active_drawing_bounds(), annotation_scale());
    update_annotation_drag_preview(to_capture_point(event->position()));
  } else {
    drag_current_ = to_drawing_point(event->position());
    update_new_annotation_preview(*drag_current_);
  }
  update();
  event->accept();
}

void OverlayEditorWidget::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && brush_active_) {
    finish_brush_stroke(to_brush_point(event->position()));
    event->accept();
    return;
  }
  if (event->button() == Qt::LeftButton && dragging_annotation_transform_ &&
      annotation_drag_origin_ &&
      annotation_drag_origin_->kind == AnnotationKind::brush) {
    update_brush_transform(event->position());
    finish_pending_pointer_edit();
    event->accept();
    return;
  }

  // Release uses its own final coordinate below; no need to build the queued
  // move.
  if (event->button() == Qt::LeftButton) {
    pending_annotation_move_.reset();
    ++annotation_preview_epoch_;
  }
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
    if (rotation_drag_)
      (void)rotation_drag_->update(to_shape_point(event->position()),
                                   active_drawing_bounds(), annotation_scale());
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
      if ((preview->kind == AnnotationKind::arrow ||
           preview->kind == AnnotationKind::line) &&
          preview->arrow_geometry.has_value()) {
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
  if (tool_ == EditorTool::brush) update_pointer_cursor(QPointF(-1, -1));
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
  if (tool_ == EditorTool::brush && last_press_had_selection_ &&
      !selection_.desktop_rect.empty()) {
    if (event->button() == Qt::LeftButton &&
        (!interaction_started_ || interaction_started_()))
      begin_brush_stroke(event->position());
    event->accept();
    return;
  }
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
        static_cast<std::int32_t>(
            std::ceil((static_cast<double>(font_size_dip_) * 1.4 + 12.0) *
                      point_pixel_scale_)),
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
  if ((tool_ == EditorTool::arrow || tool_ == EditorTool::line) &&
      std::hypot(static_cast<double>(end.x - drag_anchor_.x),
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
      object.style = ShapeStyle{color_srgb_rgb_, line_width_dip_};
      break;
    case EditorTool::ellipse:
      object.kind = AnnotationKind::ellipse;
      object.style = ShapeStyle{color_srgb_rgb_, line_width_dip_};
      break;
    case EditorTool::line:
    case EditorTool::arrow:
      object.kind = tool_ == EditorTool::line ? AnnotationKind::line
                                              : AnnotationKind::arrow;
      object.style = ShapeStyle{color_srgb_rgb_, line_width_dip_};
      object.arrow_geometry = ArrowGeometry{drag_anchor_, end};
      break;
    case EditorTool::brush:
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
  const auto effective_style =
      style.value_or(TextStyle{color_srgb_rgb_, font_size_dip_});
  const auto minimum_height = static_cast<std::int32_t>(std::ceil(
      (static_cast<double>(effective_style.font_size_dip) * 1.4 + 12.0) *
      point_pixel_scale_));
  auto minimum_width = static_cast<std::int32_t>(std::ceil(80.0 * point_pixel_scale_));
  auto measured_height = minimum_height;
  if (text_rasterizer_ != nullptr && !text.empty()) {
    const auto metrics = text_rasterizer_->measure(TextMeasureRequest{
        text, effective_style.font_size_dip, point_pixel_scale_});
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

int OverlayEditorWidget::text_editor_font_pixel_size(
    const std::uint16_t font_size_dip) const {
  const auto capture_to_widget = static_cast<double>(height()) /
      static_cast<double>(std::max(1, capture_size_px_.height));
  return std::max(
      1, static_cast<int>(std::lround(static_cast<double>(font_size_dip) *
                                      point_pixel_scale_ * capture_to_widget)));
}

void OverlayEditorWidget::begin_text_edit(const PixelRect bounds) {
  if (text_editor_ != nullptr) {
    if (!commit_text_edit()) return;
  }
  apply_document_command(SelectAnnotation{std::nullopt});
  editing_text_object_id_.reset();
  pending_text_bounds_ = bounds;
  pending_text_style_ = TextStyle{color_srgb_rgb_, font_size_dip_};
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
  const auto style =
      pending_text_style_.value_or(TextStyle{color_srgb_rgb_, font_size_dip_});
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
  font.setWeight(static_cast<QFont::Weight>(annotation_font::weight));
  font.setVariableAxis("wght", static_cast<float>(annotation_font::weight));
  font.setHintingPreference(QFont::PreferNoHinting);
  font.setStyleStrategy(static_cast<QFont::StyleStrategy>(
      QFont::PreferAntialias | QFont::NoFontMerging));
  font.setPixelSize(
      text_editor_font_pixel_size(pending_text_style_->font_size_dip));
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
        if (span.length() == 0) {
          continue;
        }
        left = std::min(left, span.x);
        top = std::min(top, span.y);
        right = std::max(
            right,
            span.x + static_cast<std::int32_t>(span.length()));
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
  if ((object->kind == AnnotationKind::arrow ||
       object->kind == AnnotationKind::line) &&
      object->arrow_geometry.has_value()) {
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

  if (object->kind == AnnotationKind::brush && object->brush_geometry) {
    auto b = brush_centerline_bounds(*object->brush_geometry);
    auto tl = shape_widget_point({b.left, b.top}),
         br = shape_widget_point({b.right, b.bottom});
    const bool x = b.right > b.left, y = b.bottom > b.top;
    if (x && y) {
      if (near(tl))
        return AnnotationAdjustment::north_west;
      if (near({br.x(), tl.y()}))
        return AnnotationAdjustment::north_east;
      if (near(br))
        return AnnotationAdjustment::south_east;
      if (near({tl.x(), br.y()}))
        return AnnotationAdjustment::south_west;
    }
    if (y) {
      if (near({(tl.x() + br.x()) / 2, tl.y()}))
        return AnnotationAdjustment::north;
      if (near({(tl.x() + br.x()) / 2, br.y()}))
        return AnnotationAdjustment::south;
    }
    if (x) {
      if (near({tl.x(), (tl.y() + br.y()) / 2}))
        return AnnotationAdjustment::west;
      if (near({br.x(), (tl.y() + br.y()) / 2}))
        return AnnotationAdjustment::east;
    }
    auto p = to_shape_point(point);
    return brush_hit_test(*object, {p.x, p.y}, tolerance * point_pixel_scale_,
                          annotation_scale())
               ? std::optional{AnnotationAdjustment::move}
               : std::nullopt;
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
  if (object->kind == AnnotationKind::brush) {
    auto p = to_shape_point(point);
    return brush_hit_test(*object, {p.x, p.y}, tolerance * point_pixel_scale_,
                          annotation_scale())
               ? std::optional{AnnotationAdjustment::move}
               : std::nullopt;
  }
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
      return move_shape(origin,
                        {static_cast<double>(current.x - anchor.x),
                         static_cast<double>(current.y - anchor.y)},
                        limits, annotation_scale());
    return resize_shape(
        origin, adjustment,
        {static_cast<double>(anchor.x), static_cast<double>(anchor.y)},
        {static_cast<double>(current.x), static_cast<double>(current.y)},
        limits,
        std::max(4, static_cast<int>(std::ceil(4.0 * point_pixel_scale_))),
        annotation_scale());
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
  preview.style = ShapeStyle{color_srgb_rgb_, line_width_dip_};
  preview.kind = tool_ == EditorTool::ellipse ? AnnotationKind::ellipse
                 : tool_ == EditorTool::arrow ? AnnotationKind::arrow
                 : tool_ == EditorTool::line  ? AnnotationKind::line
                                              : AnnotationKind::rectangle;
  if (preview.kind == AnnotationKind::arrow ||
      preview.kind == AnnotationKind::line)
    preview.arrow_geometry = ArrowGeometry{drag_anchor_, current};
  if (draft_annotation_ && *draft_annotation_ == preview) return;
  auto built = prepare_annotation_preview(
      document_, annotation_render_plan_.get(),
      ObjectPreview{PreviewIntent::create, preview, {}}, edit_context());
  if (!built)
    return;
  draft_annotation_ = preview;
  transient_annotation_render_plan_ = std::move(built.value());
  if (clean_content_presented_) notify_preview_changed();
}

void OverlayEditorWidget::update_annotation_drag_preview(const PixelPoint current) {
  if (!annotation_drag_origin_.has_value() || !annotation_adjustment_.has_value()) {
    return;
  }
  auto preview = rotation_drag_
      ? rotation_drag_->preview()
      : adjusted_annotation(*annotation_drag_origin_, *annotation_adjustment_, drag_anchor_, current);
  auto built = prepare_annotation_preview(
      document_, annotation_render_plan_.get(),
      ObjectPreview{PreviewIntent::replace, preview, {}}, edit_context());
  if (!built)
    return;
  annotation_drag_preview_ = std::move(preview);
  transient_annotation_render_plan_ = std::move(built.value());
  drag_current_ = current;
  layout_annotation_controls();
  if (clean_content_presented_) notify_preview_changed();
}

void OverlayEditorWidget::update_pointer_cursor(const QPointF point) {
  const auto raw = to_shape_point(point);
  const auto clip = selection_.desktop_rect;
  const bool brush_area = point.x() >= 0 && point.y() >= 0 &&
      point.x() < width() && point.y() < height() &&
      childAt(point.toPoint()) == nullptr &&
      raw.x >= clip.x && raw.y >= clip.y &&
      raw.x <= clip.right() && raw.y <= clip.bottom();
  if (brush_cursor_position_ &&
      (tool_ != EditorTool::brush || interaction_locked_ ||
       selection_.desktop_rect.empty() || !brush_area)) {
    brush_cursor_position_.reset();
    update();  // Erase the old ring, including when leaving for a child control.
  }
  if (tool_ == EditorTool::brush && !selection_.desktop_rect.empty() &&
      !interaction_locked_ && brush_area) {
    auto diameter = brush_diameter_dip_ * point_pixel_scale_ * width() /
                    std::max(1, capture_size_px_.width);
    auto region = [diameter](QPointF p) {
      return QRectF(p.x() - diameter / 2 - 3, p.y() - diameter / 2 - 3,
                    diameter + 6, diameter + 6)
          .toAlignedRect();
    };
    if (brush_cursor_position_)
      update(region(*brush_cursor_position_));
    brush_cursor_position_ = point;
    update(region(point));
    setCursor(Qt::CrossCursor);
    return;
  }

  if (tool_ == EditorTool::brush && !selection_.desktop_rect.empty()) {
    setCursor(Qt::ArrowCursor);
    return;
  }

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
    if (iterator->kind == AnnotationKind::brush) {
      if (brush_hit_test(*iterator, {double(point.x), double(point.y)},
                         9 * point_pixel_scale_, annotation_scale()))
        return iterator->id;
      continue;
    }
    if (iterator->arrow_geometry) {
      if (line_hit_test(*iterator, {double(point.x), double(point.y)},
                        9 * point_pixel_scale_, annotation_scale()))
        return iterator->id;
      continue;
    }
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

namespace hdrshot {
StrokePoint OverlayEditorWidget::to_brush_point(QPointF point) const {
  const auto p = to_shape_point(point);
  const auto clip = selection_.desktop_rect;
  return {std::clamp(p.x, double(clip.x), double(clip.right())),
          std::clamp(p.y, double(clip.y), double(clip.bottom()))};
}
bool OverlayEditorWidget::begin_brush_stroke(QPointF point) {
  if (brush_active_)
    return false;
  auto raw = to_shape_point(point);
  auto clip = selection_.desktop_rect;
  if (raw.x < clip.x || raw.y < clip.y || raw.x > clip.right() ||
      raw.y > clip.bottom())
    return false;
  if (!apply_document_command(SelectAnnotation{std::nullopt}))
    return false;
  auto first = to_brush_point(point);
  auto result = brush_builder_.begin(
      next_object_id_, {color_srgb_rgb_, brush_diameter_dip_}, first,
      selection_.desktop_rect, annotation_scale());
  if (!result)
    return false;
  brush_active_ = true;
  dragging_annotation_ = true;
  last_brush_point_ = first;
  pending_brush_points_.clear();
  flush_pending_brush_preview();
  update_pointer_cursor(point);
  return true;
}
void OverlayEditorWidget::flush_pending_brush_preview() {
  if (!brush_active_)
    return;
  if (!pending_brush_points_.empty()) {
    auto added = brush_builder_.append(pending_brush_points_);
    if (!added)
      return;
    pending_brush_points_.clear();
  }
  auto snapshot = brush_builder_.snapshot();
  if (!snapshot)
    return;
  auto preview =
      prepare_annotation_preview(document_, annotation_render_plan_.get(),
                                 snapshot.value(), edit_context());
  if (!preview)
    return;
  transient_annotation_render_plan_ = std::move(preview.value());
  notify_preview_changed();
  update();
}
bool OverlayEditorWidget::finish_brush_stroke(StrokePoint final) {
  if (!brush_active_)
    return true;
  if (!pending_brush_points_.empty()) {
    auto added = brush_builder_.append(pending_brush_points_);
    if (!added)
      return false;
    pending_brush_points_.clear();
  }
  auto completed = brush_builder_.finish(final);
  if (!completed)
    return false;
  last_brush_point_ = final;
  const auto &value = completed.value();
  pending_reuse_ = value.reusable;
  const auto committed = apply_document_command(CreateAnnotation{value.object});
  pending_reuse_.reset();
  if (!committed)
    return false;
  ++next_object_id_.value;
  brush_active_ = false;
  dragging_annotation_ = false;
  brush_preview_scheduled_ = false;
  brush_builder_.cancel();
  drag_current_.reset();
  update();
  return true;
}
void OverlayEditorWidget::update_brush_transform(QPointF point) {
  if (!annotation_drag_origin_ || !annotation_adjustment_)
    return;
  auto candidate = resize_brush(
      *annotation_drag_origin_, *annotation_adjustment_, brush_drag_anchor_,
      to_brush_point(point), selection_.desktop_rect, annotation_scale());
  auto preview = prepare_annotation_preview(
      document_, annotation_render_plan_.get(),
      ObjectPreview{PreviewIntent::replace, candidate, {}}, edit_context());
  if (!preview)
    return;
  annotation_drag_preview_ = std::move(candidate);
  transient_annotation_render_plan_ = std::move(preview.value());
  notify_preview_changed();
  layout_annotation_controls();
  update();
}
bool OverlayEditorWidget::finish_pending_pointer_edit() {
  if (brush_active_)
    return finish_brush_stroke(last_brush_point_);
  flush_pending_annotation_move();
  if (dragging_annotation_transform_ && annotation_drag_preview_) {
    const auto value = *annotation_drag_preview_;
    bool result = true;
    if (annotation_drag_origin_ && value != *annotation_drag_origin_) {
      if (value.brush_geometry)
        result = apply_document_command(
            SetBrushGeometry{value.id, *value.brush_geometry});
      else if (value.arrow_geometry)
        result = apply_document_command(
            SetArrowGeometry{value.id, *value.arrow_geometry});
      else if (value.kind == AnnotationKind::text)
        result = apply_document_command(
            UpdateTextAnnotation{value.id, value.bounds,
                                 std::get<TextStyle>(value.style), value.text});
      else
        result = apply_document_command(
            SetShapeGeometry{value.id, value.bounds, value.transform});
    }
    if (!result)
      return false;
    dragging_annotation_transform_ = false;
    annotation_adjustment_.reset();
    annotation_drag_origin_.reset();
    annotation_drag_preview_.reset();
    rotation_drag_.reset();
    transient_annotation_render_plan_.reset();
    notify_preview_changed();
    update();
  }
  if (dragging_annotation_ && drag_current_) {
    const auto count = document_.snapshot().objects.size();
    finish_annotation(*drag_current_);
    if (draft_annotation_ && document_.snapshot().objects.size() == count)
      return false;
    dragging_annotation_ = false;
    draft_annotation_.reset();
    transient_annotation_render_plan_.reset();
    drag_current_.reset();
  }
  return true;
}
} // namespace hdrshot
