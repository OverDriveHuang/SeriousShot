#include "ui/qt/pinned_screenshot_window.hpp"
#include "ui/qt/pinned_screenshot_preview.hpp"
#include "ui/qt/analyzer_source_view.hpp"
#include "ui/qt/source_presentation_controller.hpp"
#include "ui/qt/toolbar_symbol_button.hpp"
#include "ui/qt/toolbar_style.hpp"
#include "ui/qt/pinned_window_style.hpp"
#include <QPainter>
#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QMouseEvent>
#include <QScreen>
#include <QStyleHints>
#include <QWindow>
#include <algorithm>
#include <cmath>

namespace hdrshot {
namespace {
constexpr int minimum_window_width = minimum_pinned_screenshot_short_edge;
class WindowGrip final : public QWidget {
public:
  explicit WindowGrip(QWidget *parent) : QWidget(parent) { setMouseTracking(true); }
protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setPen(QColor(palette().color(QPalette::Window).lightness()<128 ? "#858B94" : "#767E89"));
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen = painter.pen(); pen.setWidthF(1.7);
    pen.setCapStyle(Qt::RoundCap); pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    // Match the approved Lucide move SVG: 24-unit viewBox displayed at 22 px.
    // https://github.com/lucide-icons/lucide/blob/main/icons/move.svg
    painter.translate(width()/2.-11.,height()/2.-11.);
    painter.scale(22./24.,22./24.);
    painter.drawLine(QPointF(12,2),QPointF(12,22));
    painter.drawLine(QPointF(2,12),QPointF(22,12));
    QPainterPath heads;
    heads.moveTo(15,19);heads.lineTo(12,22);heads.lineTo(9,19);
    heads.moveTo(19,9);heads.lineTo(22,12);heads.lineTo(19,15);
    heads.moveTo(5,9);heads.lineTo(2,12);heads.lineTo(5,15);
    heads.moveTo(9,5);heads.lineTo(12,2);heads.lineTo(15,5);
    painter.drawPath(heads);
  }
};
}

QRect PinnedScreenshotWindow::initial_geometry(QSize image, int short_edge,
                                               QRect available) {
  const double factor = double(short_edge) / std::max(1, std::min(image.width(), image.height()));
  QSize size(std::max(minimum_window_width, int(std::lround(image.width() * factor))),
             std::max(100, int(std::lround(image.height() * factor))));
  size.scale(available.size() - QSize(16, 16), Qt::KeepAspectRatio);
  // scale() may upscale; only constrain oversize windows.
  QSize requested(std::max(minimum_window_width, int(std::lround(image.width() * factor))),
                  std::max(100, int(std::lround(image.height() * factor))));
  if (requested.width() <= available.width()-16 && requested.height() <= available.height()-16)
    size = requested;
  size.setWidth(std::max(std::min(minimum_window_width, available.width()), size.width()));
  return QRect(QPoint(available.right()-size.width()-7, available.top()+8), size);
}
PinnedScreenshotWindow::PinnedScreenshotWindow(analysis::Input input,
    PinnedScreenshotDefaults defaults, QWidget *parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint), input_(std::move(input)) {
  setObjectName("pinnedScreenshotWindow");
  setWindowTitle(QStringLiteral("贴图 — SeriousShot"));
  setAttribute(Qt::WA_ShowWithoutActivating);
  setFocusPolicy(Qt::NoFocus);
  setMinimumSize(minimum_window_width, 100);
  setMouseTracking(true);
  source_ = new AnalyzerSourceView(this);
  source_->setObjectName("pinnedSource");
  source_->setFocusPolicy(Qt::NoFocus);
  source_->set_source_size({int(input_.source->size_px().width), int(input_.source->size_px().height)});
  source_->set_tools(false, false, false);
  source_->view_changed = [this] { restore_cursors(); schedule_present(); };
  source_->gesture_changed = [this](bool active) {
    restore_cursors();
    if (active) reveal_toolbar();
    else if (!rect().contains(mapFromGlobal(QCursor::pos()))) hide_timer_.start();
  };

  toolbar_ = new QFrame(this);
  toolbar_->setObjectName("pinnedToolbar");
  toolbar_->setAttribute(Qt::WA_NativeWindow);
  auto *row = new QHBoxLayout(toolbar_);
  row->setContentsMargins(toolbar_style::horizontal_margin, toolbar_style::vertical_margin, toolbar_style::horizontal_margin, toolbar_style::vertical_margin);
  row->setSpacing(toolbar_style::spacing);
  toolbar_->setCursor(Qt::ArrowCursor);
  auto plain = [&](const char *name, QString text, QString tip) {
    auto *b = new QToolButton(toolbar_);
    b->setObjectName(name); b->setText(text); b->setToolTip(tip);
    b->setFixedSize(toolbar_style::button_size,toolbar_style::button_size); b->setFocusPolicy(Qt::NoFocus); row->addWidget(b);
    return b;
  };
  auto *fit = plain("pinFit", QStringLiteral("适应"), QStringLiteral("适应窗口"));
  fit->setCheckable(true); fit->setChecked(true);
  connect(fit, &QToolButton::clicked, this, [this] { source_->fit(); });
  auto *one = plain("pinOneToOne", "1:1", QStringLiteral("原始倍率"));
  one->setCheckable(true);
  connect(one, &QToolButton::clicked, this, [this] {
    source_->zoom_at(QPointF(source_->width()/2., source_->height()/2.), 1./source_->effective_scale());
  });
  auto separator = [&] {
    auto *line = new QFrame(toolbar_); line->setObjectName("pinSeparator");
    line->setProperty("toolbarSeparator", true);
    line->setFixedSize(toolbar_style::separator_width,toolbar_style::separator_height); row->addWidget(line);
  };
  separator();
  grip_ = new WindowGrip(toolbar_);
  grip_->setObjectName("pinWindowGrip"); grip_->setToolTip(QStringLiteral("拖动贴图窗口"));
  grip_->setCursor(Qt::SizeAllCursor); grip_->setMinimumWidth(0);
  row->addWidget(grip_, 1);
  separator();
  auto symbol = [&](const char *name, ToolbarSymbol icon, QString tip) {
    auto *b = new ToolbarSymbolButton(icon, true, toolbar_);
    b->setObjectName(name); b->setToolTip(tip); b->setFixedSize(toolbar_style::button_size,toolbar_style::button_size);
    b->setFocusPolicy(Qt::NoFocus); row->addWidget(b); return b;
  };
  auto action = [&](QToolButton *b, UiCommand command) {
    connect(b, &QToolButton::clicked, this, [this, command] {
      if (!busy_ && action_requested) action_requested(command);
    });
    b->setProperty("pinOperation", true);
  };
  auto *analyze = symbol("pinAnalyze", ToolbarSymbol::analyze, QStringLiteral("在分析器中打开"));
  analyze->setProperty("analysisAction", true); action(analyze, UiCommand::analyze);
  separator();
  auto *save = symbol("pinSave", ToolbarSymbol::save, QStringLiteral("保存；右键另存为"));
  action(save, UiCommand::save_default);
  auto *arrow = plain("pinSaveMenu", {}, QStringLiteral("保存选项"));
  arrow->setArrowType(Qt::DownArrow); // Native glyph, independent of font coverage.
  arrow->setFixedWidth(16); arrow->setProperty("pinOperation", true);
  // Save and its dropdown form one control; outer spacing matches capture.
  row->removeWidget(save); row->removeWidget(arrow);
  auto *save_group = new QWidget(toolbar_);
  auto *save_row = new QHBoxLayout(save_group);
  save_row->setContentsMargins(0,0,0,0); save_row->setSpacing(0);
  save_row->addWidget(save); save_row->addWidget(arrow);
  row->addWidget(save_group);
  auto menu = [this, save] {
    if (busy_) return;
    menu_open_ = true; reveal_toolbar();
    const QPointer<PinnedScreenshotWindow> guard(this);
    QMenu popup;
    auto *save_default = popup.addAction(QStringLiteral("保存"));
    auto *save_as = popup.addAction(QStringLiteral("另存为…"));
    auto *chosen = popup.exec(save->mapToGlobal(QPoint(0,save->height())));
    if (!guard || closed_) return;
    menu_open_ = false;
    if (chosen && action_requested) action_requested(chosen == save_as ? UiCommand::save_as : UiCommand::save_default);
    (void)save_default;
    if (!guard || closed_) return;
    if (!rect().contains(mapFromGlobal(QCursor::pos()))) hide_timer_.start();
  };
  connect(arrow, &QToolButton::clicked, this, menu);
  save->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(save, &QWidget::customContextMenuRequested, this, [menu](QPoint) { menu(); });
  auto *copy = symbol("pinCopy", ToolbarSymbol::copy, QStringLiteral("复制"));
  copy->setProperty("primary", true); action(copy, UiCommand::copy_and_close);
  separator();
  topmost_ = symbol("pinTopmost", ToolbarSymbol::pin, QStringLiteral("窗口置顶"));
  topmost_->setCheckable(true); topmost_->setChecked(defaults.always_on_top);
  connect(topmost_, &QToolButton::toggled, this, [this](bool checked) {
    if (topmost_changed) topmost_changed(checked);
  });
  separator();
  collapse_ = symbol("pinCollapse", ToolbarSymbol::panel_top_close, QStringLiteral("收起贴图"));
  collapse_->setAccessibleName(collapse_->toolTip());
  preview_ = new PinnedScreenshotPreview(*this,*collapse_,input_);
  connect(collapse_, &QToolButton::clicked, this, [this] { set_collapsed(!collapsed_); });
  auto *close = symbol("pinClose", ToolbarSymbol::cancel, QStringLiteral("关闭贴图"));
  close->setProperty("cancelAction", true);
  connect(close, &QToolButton::clicked, this, &QWidget::close);
  status_ = new QLabel(this); status_->setObjectName("pinStatus");
  status_->setAttribute(Qt::WA_NativeWindow); status_->setWordWrap(true); status_->hide();

  hide_timer_.setSingleShot(true); hide_timer_.setInterval(650);
  connect(&hide_timer_, &QTimer::timeout, this, [this] {
    if (!collapsed_ && !menu_open_ && !moving_ && !resizing_ && !source_->dragging()) toolbar_->hide();
  });
  presentation_ = new SourcePresentationController(*this, *source_, [this] {
    SourcePresentationOptions options;
    options.settings.working_space = input_.is_hdr ? analysis::WorkingSpace::display_p3_pq :
                                                    analysis::WorkingSpace::display_p3_sdr;
    return options;
  }, [this](analysis::SourceView view, std::uintptr_t surface) {
    if (!present_requested)
      return false;
    source_->presentation_widget()->setFocusPolicy(Qt::NoFocus);
    if (toolbar_->isVisible()) toolbar_->raise();
    present_requested(std::move(view), surface);
    return true;
  }, [this] { return bool(present_requested); });
  // Observe native presentation children before their existing Source forwarder.
  // Consume only host drags; Source keeps its original pan/gesture input route.
  qApp->installEventFilter(this);
  update_theme();
  resize(533,400);
}
PinnedScreenshotWindow::~PinnedScreenshotWindow() {
  closed_ = true;
  delete preview_; preview_=nullptr;
  cancel_interaction();
  qApp->removeEventFilter(this);
  hide_timer_.stop();
  presentation_->stop();
  source_->view_changed = {};
  source_->gesture_changed = {};
}
bool PinnedScreenshotWindow::always_on_top() const { return topmost_->isChecked(); }
void PinnedScreenshotWindow::set_busy(bool busy) {
  busy_ = busy;
  // Block duplicate requests without flashing every operation's disabled icon.
  // Both action callbacks and the save menu check busy_ before dispatch.
}
void PinnedScreenshotWindow::show_error(QString message) {
  if (closed_) return;
  set_collapsed(false);
  status_->setProperty("displayError", false);
  status_->setText(message); layout_status(true);
  status_->show(); status_->raise(); toolbar_->raise();
}
void PinnedScreenshotWindow::show_display_error(QString message) {
  show_error(std::move(message));
  status_->setProperty("displayError", true);
}
void PinnedScreenshotWindow::clear_display_error() {
  if (!status_->property("displayError").toBool()) return;
  status_->setProperty("displayError", false);
  expanded_status_visible_ = false;
  status_->hide();
}
void PinnedScreenshotWindow::reveal_toolbar() {
  hide_timer_.stop(); toolbar_->show(); toolbar_->raise();
}
void PinnedScreenshotWindow::set_collapsed(bool collapsed) {
  if (closed_ || collapsed_ == collapsed) return;
  preview_->dismiss();
  cancel_interaction();
  if (collapsed) {
    expanded_size_ = size();
    expanded_minimum_size_ = minimumSize();
    expanded_maximum_size_ = maximumSize();
    expanded_source_geometry_ = source_->geometry();
    expanded_status_visible_ = !status_->isHidden();
    collapsed_ = true;
    // Hide before resizing: Source keeps its real viewport and Fit/pan state,
    // while the existing presentation controller cancels pending hidden draws.
    source_->hide();
    status_->hide();
    const int bar_height = toolbar_->height() + 2 * std::max(0, toolbar_->y());
    setFixedSize(width(), bar_height);
  } else {
    setMinimumSize(expanded_minimum_size_);
    setMaximumSize(expanded_maximum_size_);
    resize(expanded_size_);
    collapsed_ = false;
    source_->setGeometry(expanded_source_geometry_);
    // Preserve the moved bar's position. Only keep toolbar entry points inside
    // the current available screen; the image body does not force relocation.
    auto *target = QGuiApplication::screenAt(toolbar_->mapToGlobal(toolbar_->rect().center()));
    if (!target) target = screen();
    if (target) {
      const auto available = target->availableGeometry();
      const auto entries = toolbar_->geometry();
      const int left = available.left() - entries.left();
      const int top = available.top() - entries.top();
      const int right = std::max(left, available.right() - entries.right());
      const int bottom = std::max(top, available.bottom() - entries.bottom());
      move(std::clamp(x(), left, right), std::clamp(y(), top, bottom));
    }
    source_->show();
    if (expanded_status_visible_) {
      layout_status(true);
      status_->show(); status_->raise();
    }
  }
  for (const auto *name : {"pinFit", "pinOneToOne"})
    toolbar_->findChild<QToolButton *>(name)->setEnabled(!collapsed_);
  collapse_->set_symbol(collapsed_ ? ToolbarSymbol::panel_top_open : ToolbarSymbol::panel_top_close);
  collapse_->setToolTip(collapsed_ ? QString{} : QStringLiteral("收起贴图"));
  collapse_->setAccessibleName(collapsed_ ? QStringLiteral("展开贴图") : QStringLiteral("收起贴图"));
  collapse_->setProperty("thumbnailMode",collapsed_);
  preview_->set_collapsed(collapsed_);
  update_theme();
  restore_cursors();
  reveal_toolbar();
  if (!collapsed_) {
    schedule_present();
    if (!rect().contains(mapFromGlobal(QCursor::pos()))) hide_timer_.start();
  }
}
void PinnedScreenshotWindow::layout_status(bool ensure_height) {
  const int top = toolbar_->geometry().bottom() + 9;
  const int desired_height = std::max(56, status_->heightForWidth(std::max(1, width()-16)));
  if (ensure_height && height() < top + desired_height + 8)
    resize(width(), top + desired_height + 8);
  const int available_height = std::max(0, height() - top - 8);
  const int status_height = std::min(desired_height, available_height);
  status_->setGeometry(8, std::max(top, height()-status_height-8), width()-16, status_height);
}
void PinnedScreenshotWindow::schedule_present() {
  if (toolbar_) {
    if (auto *fit=toolbar_->findChild<QToolButton *>("pinFit")) fit->setChecked(source_->transform().fit);
    if (auto *one=toolbar_->findChild<QToolButton *>("pinOneToOne"))
      one->setChecked(!source_->transform().fit && std::abs(source_->effective_scale()-1.)<1e-6);
  }
  refresh_presentation();
}
analysis::SourceView PinnedScreenshotWindow::presentation_view() const {
  return presentation_->view();
}
void PinnedScreenshotWindow::refresh_presentation() {
  if (!closed_ && presentation_)
    presentation_->request_present();
}
void PinnedScreenshotWindow::resizeEvent(QResizeEvent *e) {
  QWidget::resizeEvent(e);
  if (!collapsed_) source_->setGeometry(rect());
  toolbar_->setGeometry(0,4,width(), toolbar_style::button_size + 2*toolbar_style::vertical_margin);
  if (status_->isVisible()) layout_status();
  toolbar_->raise(); schedule_present();
}
void PinnedScreenshotWindow::showEvent(QShowEvent *e) {
  QWidget::showEvent(e); schedule_present();
  reveal_toolbar();
  if (!collapsed_ && !rect().contains(mapFromGlobal(QCursor::pos()))) hide_timer_.start();
}
void PinnedScreenshotWindow::closeEvent(QCloseEvent *e) {
  cancel_interaction();
  closed_ = true; presentation_->stop(); hide_timer_.stop();
  QWidget::closeEvent(e);
}
void PinnedScreenshotWindow::changeEvent(QEvent *e) {
  QWidget::changeEvent(e);
  if (e->type()==QEvent::PaletteChange) update_theme();
}
void PinnedScreenshotWindow::update_theme() {
  if (!toolbar_) return;
  const bool dark = palette().color(QPalette::Window).lightness()<128;
  toolbar_->setStyleSheet(toolbar_style::sheet(dark) + QString(
      "QToolButton:disabled { color:%1; background:transparent; border-color:transparent; }")
      .arg(dark ? "#80848B" : "#9CA0A5"));
  for (auto *b:toolbar_->findChildren<QToolButton *>()) b->setProperty("darkTheme",dark);
  collapse_->setStyleSheet(collapsed_ ? QString(
      "QToolButton {background:%1;} QToolButton:hover {background:%2;border-color:%3;}")
      .arg(dark?"#171B22":"#E0E3E8",dark?"#4D525A":"#DDE0E5",dark?"#A5AFBE":"#6A727E") : QString{});
  status_->setStyleSheet(dark?"background:#25282D;color:#FFB8B8;padding:6px;":
                            "background:#F6F7F9;color:#A52020;padding:6px;");
}
bool PinnedScreenshotWindow::image_fully_visible() const {
  // Use Source's current geometry, including pan, for both routing and cursors.
  // The tolerance only absorbs floating-point noise on an exactly fitted edge.
  constexpr double epsilon = 1e-6;
  return QRectF(source_->rect()).adjusted(-epsilon,-epsilon,epsilon,epsilon)
      .contains(source_->image_rect());
}
void PinnedScreenshotWindow::cancel_interaction() {
  moving_ = resizing_ = false;
  resize_edges_ = {};
  if (QWidget::mouseGrabber()==this) releaseMouse();
  source_->cancel_gesture();
  restore_cursors();
}
void PinnedScreenshotWindow::restore_cursors() {
  // Edge cursors are temporary, never the inherited cursor for image children.
  Qt::CursorShape shape = moving_ ? Qt::SizeAllCursor : Qt::ArrowCursor;
  if (resizing_) {
    const bool horizontal = resize_edges_ & (Qt::LeftEdge|Qt::RightEdge);
    const bool vertical = resize_edges_ & (Qt::TopEdge|Qt::BottomEdge);
    shape = horizontal && vertical ?
      (((resize_edges_&Qt::LeftEdge)&&(resize_edges_&Qt::TopEdge)) ||
       ((resize_edges_&Qt::RightEdge)&&(resize_edges_&Qt::BottomEdge)) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor) :
      (horizontal ? Qt::SizeHorCursor : Qt::SizeVerCursor);
  }
  setCursor(shape);
  source_->setCursor(moving_ || resizing_ ? shape : source_->dragging() ?
      Qt::ClosedHandCursor : image_fully_visible() ? Qt::SizeAllCursor : Qt::OpenHandCursor);
  if (auto *surface = source_->findChild<QWidget *>("analyzerSourceSurface")) surface->unsetCursor();
  toolbar_->setCursor(moving_ || resizing_ ? shape : Qt::ArrowCursor);
  grip_->setCursor(Qt::SizeAllCursor);
}
bool PinnedScreenshotWindow::eventFilter(QObject *object, QEvent *event) {
  if (closed_) return false;
  auto *widget = qobject_cast<QWidget *>(object);
  if (!widget || widget->window()!=this || closed_) return false;
  // Passive thumbnail descendants are rerouted by the preview shell. Fronting
  // and button activation happen once on the resulting logical-button event.
  if (collapse_ && widget!=collapse_ && collapse_->isAncestorOf(widget)) return false;
  if (widget==this && (event->type()==QEvent::UngrabMouse ||
      event->type()==QEvent::WindowDeactivate || event->type()==QEvent::Hide)) {
    cancel_interaction();
  }
  if (event->type()==QEvent::Enter) { restore_cursors(); reveal_toolbar(); }
  if (event->type()==QEvent::Leave && widget==this && !collapsed_) hide_timer_.start();
  const auto kind = event->type();
  if (kind!=QEvent::MouseButtonPress && kind!=QEvent::MouseMove && kind!=QEvent::MouseButtonRelease)
    return false;
  restore_cursors();
  auto *mouse = static_cast<QMouseEvent *>(event);
  const QPoint local = mapFromGlobal(mouse->globalPosition().toPoint());
  Qt::Edges edges;
  if (!collapsed_) {
    constexpr int hit = pinned_window_style::resize_hit_width;
    if (local.x()<hit) edges|=Qt::LeftEdge;
    if (local.x()>=width()-hit) edges|=Qt::RightEdge;
    if (local.y()<hit) edges|=Qt::TopEdge;
    if (local.y()>=height()-hit) edges|=Qt::BottomEdge;
  }
  if (kind==QEvent::MouseButtonPress) {
    const bool native_source = widget==source_->presentation_widget();
    const bool image_region = widget==source_ || native_source;
    const bool physical_left = mouse->button()==Qt::LeftButton &&
        mouse->source()==Qt::MouseEventNotSynthesized;
    const bool host_drag = physical_left && (edges || widget==grip_ ||
        (!collapsed_ && image_region && !source_->dragging() && image_fully_visible()));
    // A native Source press that remains pan is forwarded to Source once by
    // its own filter; perform host fronting on that forwarded route only.
    if (front_requested && (!native_source || host_drag)) front_requested();
    if (host_drag) {
      resizing_=bool(edges); resize_edges_=edges; moving_=!resizing_;
      drag_origin_=mouse->globalPosition().toPoint(); original_geometry_=geometry();
      grabMouse(); restore_cursors(); reveal_toolbar(); return true;
    }
  }
  if (kind==QEvent::MouseMove && (moving_||resizing_)) {
    if (!mouse->buttons().testFlag(Qt::LeftButton)) {
      cancel_interaction();
      return true;
    }
    const auto delta = mouse->globalPosition().toPoint()-drag_origin_;
    if (moving_) move(original_geometry_.topLeft()+delta);
    else {
      auto r=original_geometry_;
      if (resize_edges_&Qt::LeftEdge) r.setLeft(std::min(r.right()-minimumWidth()+1,r.left()+delta.x()));
      if (resize_edges_&Qt::RightEdge) r.setRight(std::max(r.left()+minimumWidth()-1,r.right()+delta.x()));
      if (resize_edges_&Qt::TopEdge) r.setTop(std::min(r.bottom()-minimumHeight()+1,r.top()+delta.y()));
      if (resize_edges_&Qt::BottomEdge) r.setBottom(std::max(r.top()+minimumHeight()-1,r.bottom()+delta.y()));
      setGeometry(r);
    }
    return true;
  }
  if (kind==QEvent::MouseButtonRelease && (moving_||resizing_)) {
    if (mouse->button()!=Qt::LeftButton) return false;
    moving_=resizing_=false;
    if (QWidget::mouseGrabber()==this) releaseMouse();
    restore_cursors();
    if (!rect().contains(local)) hide_timer_.start();
    return true;
  }
  if (kind==QEvent::MouseMove && !source_->dragging()) {
    if (edges) {
      const bool corner=(edges&(Qt::LeftEdge|Qt::RightEdge))&&(edges&(Qt::TopEdge|Qt::BottomEdge));
      widget->setCursor(corner ? (((edges&Qt::LeftEdge)&&(edges&Qt::TopEdge))||
          ((edges&Qt::RightEdge)&&(edges&Qt::BottomEdge)) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor) :
          (edges&(Qt::LeftEdge|Qt::RightEdge) ? Qt::SizeHorCursor : Qt::SizeVerCursor));
    }
  }
  return false;
}
} // namespace hdrshot
