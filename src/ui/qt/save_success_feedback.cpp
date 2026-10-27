#include "ui/qt/save_success_feedback.hpp"
#include "ui/qt/status_message_style.hpp"
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCursor>
#include <QLabel>
#include <QMenu>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <utility>

namespace hdrshot {
SaveSuccessFeedback::SaveSuccessFeedback(QObject *parent, ConfigureWindow configure)
    : QObject(parent), configure_window_(std::move(configure)) {
  timer_.setSingleShot(true);
  timer_.setInterval(save_success_duration_ms);
  connect(&timer_, &QTimer::timeout, this, [this] {
    if (label_) label_->window()->hide();
    show_next();
  });
  connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen *removed) {
    if (label_ && label_->isVisible() &&
        (!current_.anchor.screen || current_.anchor.screen == removed)) {
      current_.anchor.screen = nullptr;
      place_current();
    }
  });
}
SaveSuccessFeedback::~SaveSuccessFeedback() { if (label_) delete label_->window(); }

SaveFeedbackAnchor SaveSuccessFeedback::capture(QWidget *source) {
  if (source) source = source->window();
  if (!source) source = QApplication::activeWindow();
  SaveFeedbackAnchor result;
  result.palette = source ? source->palette() : QApplication::palette();
  result.font = source ? source->font() : QApplication::font();
  if (source) {
    result.source_geometry = source->frameGeometry();
    result.screen = source->screen();
  } else {
    result.screen = QGuiApplication::screenAt(QCursor::pos());
  }
  if (!result.screen) result.screen = QGuiApplication::primaryScreen();
  if (result.source_geometry.isEmpty() && result.screen)
    result.source_geometry = result.screen->availableGeometry();
  return result;
}

void SaveSuccessFeedback::completed(const Result<ExportReceipt, Error> &result,
                                    SaveFeedbackAnchor anchor) {
  if (!result) return;
  const auto *file = std::get_if<FileReceipt>(&result.value().destination);
  if (!file) return; // Cancellation is also a successful Result variant.
  queue_.push_back({QString::fromStdString(file->exact_path), std::move(anchor)});
  if ((label_ && label_->isVisible()) || scheduled_) return;
  scheduled_ = true;
  // Let capture completion hide/destroy its overlays before showing feedback.
  QTimer::singleShot(0, this, [this] { scheduled_ = false; show_next(); });
}

void SaveSuccessFeedback::show_next() {
  if (capture_active_) return;
  if (queue_.empty()) return;
  current_ = std::move(queue_.front());
  queue_.pop_front();
  if (!label_) {
    // A translucent top-level QLabel omits its QSS background on Windows.
    // Keep native window attributes on a host and the existing style on a
    // normal child label, as in the capture editor's status message.
    auto *host = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint |
                            Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    host->setObjectName(QStringLiteral("saveSuccessFeedbackHost"));
#ifdef __APPLE__
    // Keep save feedback present when another application is in front.
    host->setAttribute(Qt::WA_MacAlwaysShowToolWindow);
#endif
    host->setAttribute(Qt::WA_ShowWithoutActivating);
    host->setAttribute(Qt::WA_TranslucentBackground);
    host->setAttribute(Qt::WA_QuitOnClose, false);
    host->setFocusPolicy(Qt::NoFocus);
    auto *layout = new QVBoxLayout(host);
    layout->setContentsMargins(0,0,0,0);
    label_ = new QLabel(host);
    layout->addWidget(label_);
    label_->setObjectName(QStringLiteral("saveSuccessFeedback"));
    label_->setFocusPolicy(Qt::NoFocus);
    label_->setTextFormat(Qt::PlainText);
    label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label_->setAlignment(Qt::AlignCenter);
    label_->setWordWrap(true);
    label_->installEventFilter(this);
    label_->setToolTip(QStringLiteral("右键复制完整保存路径"));
  }
  label_->setPalette(current_.anchor.palette);
  label_->setFont(current_.anchor.font);
  label_->setStyleSheet(status_message_style(current_.anchor.palette, false));
  label_->setText(QStringLiteral("成功保存到：\n%1").arg(current_.path));
  place_current();
  if (configure_window_) configure_window_(*label_->window());
  label_->window()->show();
  timer_.start(); // One owned timer; no stale singleShot can hide a later file.
}

void SaveSuccessFeedback::set_capture_active(bool active) {
  if (capture_active_ == active) return;
  capture_active_ = active;
  if (active) {
    timer_.stop();
    if (label_ && label_->isVisible()) {
      queue_.push_front(current_);
      label_->window()->hide();
    }
  } else if (!scheduled_) {
    scheduled_ = true;
    QTimer::singleShot(0, this, [this] { scheduled_ = false; show_next(); });
  }
}

void SaveSuccessFeedback::place_current() {
  auto *screen = current_.anchor.screen.data();
  if (!screen || !QGuiApplication::screens().contains(screen))
    screen = QGuiApplication::primaryScreen();
  if (!screen || !label_) return;
  const auto available = screen->availableGeometry().adjusted(8, 8, -8, -8);
  auto *host = label_->window();
  const int width = std::max(1, std::min(720, available.width()));
  label_->setMinimumWidth(std::min(360, width));
  label_->setMaximumWidth(width);
  label_->setMaximumHeight(std::max(1, available.height()));
  host->setMinimumWidth(std::min(360, width));
  host->setMaximumWidth(width);
  host->setMaximumHeight(std::max(1, available.height()));
  host->adjustSize();
  const auto source = current_.anchor.source_geometry;
  int x = source.center().x() - host->width() / 2;
  int y = source.top() - host->height() - 10;
  if (y < available.top()) y = source.top() + 10;
  x = std::clamp(x, available.left(), std::max(available.left(), available.right() - host->width() + 1));
  y = std::clamp(y, available.top(), std::max(available.top(), available.bottom() - host->height() + 1));
  host->winId();
  if (host->windowHandle()) host->windowHandle()->setScreen(screen);
  host->move(x, y);
}

bool SaveSuccessFeedback::eventFilter(QObject *object, QEvent *event) {
  if (object == label_ && event->type() == QEvent::ContextMenu) {
    timer_.stop();
    const auto path = current_.path;
    QMenu menu;
    auto *copy = menu.addAction(QStringLiteral("复制完整保存路径"));
    auto *context = static_cast<QContextMenuEvent *>(event);
    if (menu.exec(context->globalPos()) == copy)
      QApplication::clipboard()->setText(path);
    timer_.start();
    return true;
  }
  return QObject::eventFilter(object, event);
}
} // namespace hdrshot
