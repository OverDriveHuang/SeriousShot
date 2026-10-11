#pragma once
#include <QApplication>
#include <QCheckBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionComboBox>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <QWidgetAction>
#include <array>
#include <functional>
#include <vector>
namespace hdrshot {
// Cocoa draws combo boxes at a fixed height. Reuse its fill for the wrapping
// selector, without stretching its arrows or introducing a separate palette.
inline QColor settings_combo_fill(const QWidget* widget) {
  QImage sample(80, 32, QImage::Format_ARGB32_Premultiplied);
  sample.fill(Qt::transparent);
  QPainter painter(&sample);
  QStyleOptionComboBox option;
  option.initFrom(widget);
  option.rect = QRect(0, 0, 80, 32);
  option.state &= ~(QStyle::State_MouseOver | QStyle::State_HasFocus);
  widget->style()->drawComplexControl(QStyle::CC_ComboBox, &option, &painter, widget);
  painter.end();
  return sample.pixelColor(20, 16);
}

class ReadingChip final : public QWidget {
public:
  explicit ReadingChip(QWidget *parent) : QWidget(parent) {}

protected:
  void mousePressEvent(QMouseEvent *event) override {
    // Label/body clicks belong to the chip; only the surrounding blank area
    // should open the selector. The remove button keeps its own click handler.
    if (event->button() == Qt::LeftButton)
      event->accept();
    else
      QWidget::mousePressEvent(event);
  }
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(palette().brush(QPalette::Window));
    auto border = palette().color(QPalette::WindowText);
    border.setAlphaF(.45);
    p.setPen(border);
    p.drawRoundedRect(QRectF(.5, .5, width() - 1., height() - 1.), 5, 5);
  }
};

class ReadingChipFlow final : public QLayout {
public:
  ReadingChipFlow(QWidget *parent) : QLayout(parent) {
    setContentsMargins(0, 0, 0, 0);
    setSpacing(6);
  }
  ~ReadingChipFlow() {
    while (auto *i = takeAt(0))
      delete i;
  }
  void addItem(QLayoutItem *i) override { items_.push_back(i); }
  int count() const override { return int(items_.size()); }
  QLayoutItem *itemAt(int i) const override {
    return i >= 0 && i < count() ? items_[i] : nullptr;
  }
  QLayoutItem *takeAt(int i) override {
    if (i < 0 || i >= count())
      return nullptr;
    auto *r = items_[i];
    items_.erase(items_.begin() + i);
    return r;
  }
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int w) const override {
    return arrange(QRect(0, 0, w, 0), true);
  }
  QSize minimumSize() const override { return {80, items_.empty() ? 0 : 28}; }
  QSize sizeHint() const override { return minimumSize(); }
  Qt::Orientations expandingDirections() const override { return {}; }
  void setGeometry(const QRect &r) override {
    QLayout::setGeometry(r);
    arrange(r, false);
  }

private:
  int arrange(QRect rect, bool test) const {
    int x = rect.x(), y = rect.y(), h = 0;
    for (auto *item : items_) {
      QSize size = item->sizeHint();
      size.setWidth(std::min(size.width(), rect.width()));
      if (item->hasHeightForWidth())
        size.setHeight(
            std::max(size.height(), item->heightForWidth(size.width())));
      if (x > rect.x() && x + size.width() > rect.right() + 1) {
        x = rect.x();
        y += h + spacing();
        h = 0;
      }
      if (!test)
        item->setGeometry(QRect(QPoint(x, y), size));
      x += size.width() + spacing();
      h = std::max(h, size.height());
    }
    return y - rect.y() + h;
  }
  std::vector<QLayoutItem *> items_;
};
class ReadingSelector final : public QWidget {
public:
  ReadingSelector(QStringList names, const QString &name, QWidget *parent)
      : QWidget(parent), names_(std::move(names)), flags_(names_.size()) {
    setObjectName(name);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(7, 7, 7, 7);
    root->setSpacing(6);
    chips_ = new QWidget(this);
    chips_->setObjectName(name + "Chips");
    flow_ = new ReadingChipFlow(chips_);
    root->addWidget(chips_, 1);
    auto *button = new QToolButton(this);
    button->setText("⌄");
    button->setStyleSheet(
        "QToolButton {border:0;background:transparent;} "
        "QToolButton::menu-indicator {image:none;width:0px;}");
    button->setAutoRaise(true);
    button->setPopupMode(QToolButton::InstantPopup);
    button->setObjectName(name + "Button");
    button->setAccessibleName("选择默认读数");
    root->addWidget(button, 0, Qt::AlignTop);
    menu_ = new QMenu(button);
    button->setMenu(menu_);
    for (int i = 0; i < names_.size(); ++i) {
      auto *action = new QWidgetAction(menu_);
      auto *check = new QCheckBox(names_[i], menu_);
      check->setObjectName(name + "Option" + QString::number(i));
      check->setContentsMargins(10, 6, 10, 6);
      action->setDefaultWidget(check);
      menu_->addAction(action);
      checks_.push_back(check);
      connect(check, &QCheckBox::toggled, this, [this, i](bool b) {
        if (syncing_)
          return;
        flags_[i] = b;
        rebuild();
        if (changed)
          changed();
      });
    }
  }
  template <std::size_t N> void set_selected(const std::array<bool, N> &flags) {
    bool same = true;
    for (int i = 0; i < int(N); ++i)
      same = same && (flags_[i] == flags[i]);
    if (same && initialized_)
      return;
    initialized_ = true;
    syncing_ = true;
    for (int i = 0; i < int(N); ++i) {
      flags_[i] = flags[i];
      checks_[i]->setChecked(flags[i]);
    }
    syncing_ = false;
    rebuild();
  }
  template <std::size_t N> std::array<bool, N> selected() const {
    std::array<bool, N> r{};
    for (int i = 0; i < int(N); ++i)
      r[i] = flags_[i];
    return r;
  }
  std::function<void()> changed;

protected:
  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton && isEnabled()) {
      menu_->popup(mapToGlobal(QPoint(0, height())));
      event->accept();
    } else {
      QWidget::mousePressEvent(event);
    }
  }
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(settings_combo_fill(this));
    painter.drawRoundedRect(QRectF(rect()), 6, 6);
  }

private:
  void rebuild() {
    while (auto *i = flow_->takeAt(0)) {
      delete i->widget();
      delete i;
    }
    for (int i = 0; i < names_.size(); ++i)
      if (flags_[i]) {
        auto *w = new ReadingChip(chips_);
        w->setObjectName(objectName() + "Chip" + QString::number(i));
        w->setStyleSheet(
            "QLabel {border:none;background:transparent;} QToolButton "
            "{border:none;background:transparent;padding:0px;}");
        auto *h = new QHBoxLayout(w);
        h->setContentsMargins(8, 4, 4, 4);
        auto *label = new QLabel(names_[i], w);
        label->setWordWrap(true);
        h->addWidget(label);
        auto *x = new QToolButton(w);
        x->setText("×");
        x->setObjectName(objectName() + "Remove" + QString::number(i));
        h->addWidget(x);
        connect(x, &QToolButton::clicked, this,
                [this, i] { checks_[i]->setChecked(false); });
        flow_->addWidget(w);
      }
    if (std::none_of(flags_.begin(), flags_.end(), [](bool b) { return b; })) {
      auto *empty = new QLabel("选择读数", chips_);
      empty->setObjectName(objectName() + "EmptyHint");
      empty->setContentsMargins(4, 4, 4, 4);
      flow_->addWidget(empty);
    }
    flow_->invalidate();
    chips_->updateGeometry();
    updateGeometry();
  }
  QStringList names_;
  std::vector<bool> flags_;
  std::vector<QCheckBox *> checks_;
  QMenu *menu_{};
  QWidget *chips_{};
  ReadingChipFlow *flow_{};
  bool syncing_{}, initialized_{};
};
} // namespace hdrshot
