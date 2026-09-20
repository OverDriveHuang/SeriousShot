#include "ui/qt/analyzer_layout.hpp"

#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace hdrshot {
namespace {
class SplitHandle final : public QWidget {
public:
  SplitHandle(Qt::Orientation orientation, QWidget *parent)
      : QWidget(parent), orientation_(orientation) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(orientation == Qt::Horizontal ? Qt::SplitHCursor
                                            : Qt::SplitVCursor);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(
        underMouse() || hasFocus() ? QColor("#8dabbc") : QColor("#424750"), 2));
    if (orientation_ == Qt::Horizontal)
      p.drawLine(width() / 2, 4, width() / 2, height() - 4);
    else
      p.drawLine(4, height() / 2, width() - 4, height() / 2);
  }

private:
  Qt::Orientation orientation_;
};
} // namespace

AnalyzerSplitPane::AnalyzerSplitPane(Qt::Orientation orientation, QString name,
                                     QWidget *parent)
    : QWidget(parent), orientation_(orientation),
      handle_(new SplitHandle(orientation, this)) {
  setObjectName(name);
  handle_->setObjectName(name + "Handle");
  handle_->setAccessibleName(name);
  handle_->installEventFilter(this);
  setMinimumSize(0, 0);
}

void AnalyzerSplitPane::set_panes(QWidget *first, QWidget *second) {
  first_ = first;
  second_ = second;
  first_->setParent(this);
  second_->setParent(this);
  first_->show();
  second_->show();
  relayout();
}
void AnalyzerSplitPane::set_ratio(double ratio) {
  ratio_ = std::isfinite(ratio) ? std::clamp(ratio, .01, .99) : .5;
  relayout();
}
void AnalyzerSplitPane::set_minimum_spans(int first, int second) {
  min_first_ = first;
  min_second_ = second;
  relayout();
}
void AnalyzerSplitPane::resizeEvent(QResizeEvent *) { relayout(); }
void AnalyzerSplitPane::relayout() {
  if (!first_ || !second_)
    return;
  const bool a = !first_->isHidden(), b = !second_->isHidden();
  handle_->setVisible(a && b);
  if (!(a && b)) {
    if (a)
      first_->setGeometry(rect());
    if (b)
      second_->setGeometry(rect());
    return;
  }
  const int total = orientation_ == Qt::Horizontal ? width() : height();
  const int available = std::max(0, total - 12);
  const double factor =
      std::min(1., double(available) / std::max(1, min_first_ + min_second_));
  const int low = int(std::ceil(min_first_ * factor)),
            high = int(std::floor(available - min_second_ * factor));
  const int split = std::clamp(int(std::round(available * ratio_)),
                               std::min(low, high), high);
  if (orientation_ == Qt::Horizontal) {
    first_->setGeometry(0, 0, split, height());
    handle_->setGeometry(split, 0, 12, height());
    second_->setGeometry(split + 12, 0, available - split, height());
  } else {
    first_->setGeometry(0, 0, width(), split);
    handle_->setGeometry(0, split, width(), 12);
    second_->setGeometry(0, split + 12, width(), available - split);
  }
  handle_->raise();
}
void AnalyzerSplitPane::place_from_span(double span) {
  const int available =
      std::max(1, (orientation_ == Qt::Horizontal ? width() : height()) - 12);
  const double factor =
      std::min(1., double(available) / std::max(1, min_first_ + min_second_));
  ratio_ =
      std::clamp(span, min_first_ * factor, available - min_second_ * factor) /
      available;
  relayout();
  if (changed)
    changed(ratio_);
}
bool AnalyzerSplitPane::cancel_drag() {
  if (!dragging_)
    return false;
  dragging_ = false;
  ratio_ = start_ratio_;
  handle_->releaseMouse();
  relayout();
  if (changed)
    changed(ratio_);
  return true;
}
bool AnalyzerSplitPane::eventFilter(QObject *object, QEvent *event) {
  if (object != handle_)
    return QWidget::eventFilter(object, event);
  if (event->type() == QEvent::UngrabMouse && dragging_) {
    cancel_drag();
    return false;
  }
  if (event->type() == QEvent::MouseButtonPress) {
    auto *e = static_cast<QMouseEvent *>(event);
    if (e->button() != Qt::LeftButton)
      return false;
    dragging_ = true;
    start_ratio_ = ratio_;
    start_point_ = e->globalPosition().toPoint();
    start_span_ =
        orientation_ == Qt::Horizontal ? first_->width() : first_->height();
    handle_->setFocus();
    handle_->grabMouse();
    return true;
  }
  if (event->type() == QEvent::MouseMove && dragging_) {
    const QPoint delta =
        static_cast<QMouseEvent *>(event)->globalPosition().toPoint() -
        start_point_;
    place_from_span(start_span_ +
                    (orientation_ == Qt::Horizontal ? delta.x() : delta.y()));
    return true;
  }
  if (event->type() == QEvent::MouseButtonRelease && dragging_) {
    dragging_ = false;
    handle_->releaseMouse();
    return true;
  }
  if (event->type() == QEvent::KeyPress) {
    auto *e = static_cast<QKeyEvent *>(event);
    if (e->key() == Qt::Key_Escape && cancel_drag())
      return true;
    const int previous =
        orientation_ == Qt::Horizontal ? Qt::Key_Left : Qt::Key_Up;
    const int next =
        orientation_ == Qt::Horizontal ? Qt::Key_Right : Qt::Key_Down;
    const double total =
        std::max(1, (orientation_ == Qt::Horizontal ? width() : height()) - 12);
    const double step =
        total * (e->modifiers().testFlag(Qt::ShiftModifier) ? .05 : .01);
    const int span =
        orientation_ == Qt::Horizontal ? first_->width() : first_->height();
    if (e->key() == previous || e->key() == next) {
      place_from_span(span + (e->key() == next ? step : -step));
      return true;
    }
    if (e->key() == Qt::Key_Home || e->key() == Qt::Key_End) {
      place_from_span(e->key() == Qt::Key_Home ? 0 : total);
      return true;
    }
  }
  return QWidget::eventFilter(object, event);
}

AnalyzerFlowLayout::AnalyzerFlowLayout(QWidget *parent, int margin, int spacing)
    : QLayout(parent) {
  setContentsMargins(margin, margin, margin, margin);
  setSpacing(spacing);
}
AnalyzerFlowLayout::~AnalyzerFlowLayout() {
  while (auto *item = takeAt(0))
    delete item;
}
void AnalyzerFlowLayout::addItem(QLayoutItem *item) { items_.push_back(item); }
int AnalyzerFlowLayout::count() const { return int(items_.size()); }
QLayoutItem *AnalyzerFlowLayout::itemAt(int i) const {
  return i >= 0 && i < count() ? items_[std::size_t(i)] : nullptr;
}
QLayoutItem *AnalyzerFlowLayout::takeAt(int i) {
  if (i < 0 || i >= count())
    return nullptr;
  auto *item = items_[std::size_t(i)];
  items_.erase(items_.begin() + i);
  return item;
}
QSize AnalyzerFlowLayout::minimumSize() const {
  QSize result;
  for (auto *item : items_)
    if (!item->isEmpty())
      result = result.expandedTo(item->minimumSize());
  auto m = contentsMargins();
  return result + QSize(m.left() + m.right(), m.top() + m.bottom());
}
QSize AnalyzerFlowLayout::sizeHint() const { return minimumSize(); }
int AnalyzerFlowLayout::heightForWidth(int width) const {
  return arrange(QRect(0, 0, width, 0), false);
}
void AnalyzerFlowLayout::setGeometry(const QRect &r) {
  QLayout::setGeometry(r);
  arrange(r, true);
}
int AnalyzerFlowLayout::arrange(const QRect &rect, bool apply) const {
  const auto m = contentsMargins();
  const QRect area = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
  int x = area.x(), y = area.y(), row_height = 0;
  std::vector<std::pair<QLayoutItem *, QRect>> row;
  const auto finish_row = [&] {
    if (apply)
      for (const auto &[item, bounds] : row)
        item->setGeometry(bounds.translated(0, (row_height - bounds.height()) / 2));
    row.clear();
  };
  for (auto *item : items_) {
    if (item->isEmpty())
      continue;
    QSize size = item->sizeHint();
    size.setWidth(std::min(size.width(), std::max(0, area.width())));
    if (x > area.x() && x + size.width() > area.right() + 1) {
      finish_row();
      x = area.x();
      y += row_height + spacing();
      row_height = 0;
    }
    if (item->widget() &&
        item->widget()->property("analyzerAlignRight").toBool())
      x = std::max(x, area.right() + 1 - size.width());
    row.emplace_back(item, QRect(QPoint(x, y), size));
    x += size.width() + spacing();
    row_height = std::max(row_height, size.height());
  }
  finish_row();
  return y + row_height - rect.y() + m.bottom();
}
} // namespace hdrshot
