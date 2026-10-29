#include "ui/qt/annotation_size_combo.hpp"
#include "ui/qt/annotation_size_combo_paint.hpp"
#include <QApplication>
#include <QEvent>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QStyleOptionComboBox>
#include <QGuiApplication>
#include <QHideEvent>
#include <QKeyEvent>
#include <QListView>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScreen>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <algorithm>

namespace hdrshot {
namespace {
constexpr int inset = 6;
constexpr int text_left = 12 + 12 + 8;
constexpr int right_padding = 18;
struct Colors {
  QColor shell, text, border, active, active_text;
};
Colors colors(const QComboBox* combo) {
  // Same tokens as toolbar_style::sheet; selected value is a check, not a second fill.
  return combo->palette().color(QPalette::Window).lightness() < 128
      ? Colors{"#26292E", "#E7E9EC", "#44484F", "#D9D9D9", "#20252C"}
      : Colors{"#F5F6F8", "#343940", "#D5D8DD", "#3B4046", "#FFFFFF"};
}
int row_height(const QComboBox* combo) {
  return std::max(28, combo->fontMetrics().height() + 8);
}
class SizeRowDelegate final : public QStyledItemDelegate {
 public:
  explicit SizeRowDelegate(QComboBox* combo, QObject* parent)
      : QStyledItemDelegate(parent), combo_(combo) {}
  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
    auto styled = option;
    initStyleOption(&styled, index);
    return {QFontMetrics(styled.font).horizontalAdvance(styled.text) + text_left + right_padding,
            std::max(row_height(combo_), QFontMetrics(styled.font).height() + 8)};
  }
  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override {
    auto styled = option;
    initStyleOption(&styled, index);
    const auto theme = colors(combo_);
    const bool active = (option.state & QStyle::State_Selected) != 0;
    const bool enabled = (index.flags() & Qt::ItemIsEnabled) != 0;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    if (active && enabled) {
      painter->setBrush(theme.active);
      painter->drawRoundedRect(QRectF(option.rect), 6, 6);
    }
    const QColor ink = !enabled ? combo_->palette().color(QPalette::Disabled, QPalette::Text)
        : active ? theme.active_text : theme.text;
    painter->setPen(ink);
    painter->setFont(styled.font);
    const qreal y = option.rect.center().y() + .5;
    if (index.row() == combo_->currentIndex()) {
      const qreal x = option.rect.left() + 12 - inset;
      QPainterPath tick;
      tick.moveTo(x + 1, y); tick.lineTo(x + 5, y + 4); tick.lineTo(x + 12, y - 5);
      painter->setPen(QPen(ink, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      painter->drawPath(tick);
    }
    painter->setPen(ink);
    const auto label = QRectF(option.rect).adjusted(text_left - inset, 0, -(right_padding - inset), 0);
    painter->drawText(label, Qt::AlignLeft | Qt::AlignVCenter, styled.text);
    painter->restore();
  }
 private:
  QComboBox* combo_;
};
}  // namespace

class AnnotationSizePopup final : public QWidget {
 public:
  explicit AnnotationSizePopup(AnnotationSizeCombo* owner)
      : QWidget(owner, Qt::Popup | Qt::FramelessWindowHint), owner_(owner) {
    setObjectName("annotationSizePopup");
    setAttribute(Qt::WA_TranslucentBackground);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(inset, inset, inset, inset);
    layout->setSpacing(0);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    list = new QListView(this);
    list->setObjectName("annotationSizePopupView");
    list->setFrameShape(QFrame::NoFrame);
    list->setMinimumSize(0, 0);
    list->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    list->setStyleSheet("QListView { background:transparent; border:none; outline:none; padding:0; }");
    list->viewport()->setAutoFillBackground(false);
    list->setMouseTracking(true);
    list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setItemDelegate(new SizeRowDelegate(owner, list));
    list->installEventFilter(this);
    layout->addWidget(list);
    setFocusProxy(list);
    connect(list, &QListView::entered, this, [this](const QModelIndex& index) {
      if (index.flags() & Qt::ItemIsEnabled) list->setCurrentIndex(index);
    });
    connect(list, &QListView::clicked, this, [this](const QModelIndex& index) {
      if (index.flags() & Qt::ItemIsEnabled) owner_->commit_row(index.row());
    });
  }
  void open() {
    setFont(owner_->font());
    setPalette(owner_->palette());
    list->setFont(owner_->font());
    list->setModel(owner_->model());  // one shared model; selection stays local to this view
    list->setRootIndex(owner_->rootModelIndex());
    list->setModelColumn(owner_->modelColumn());
    int width = 0;
    int height = 0;
    QStyleOptionViewItem option;
    option.initFrom(list);
    option.font = list->font();
    for (int row = 0; row < owner_->count(); ++row) {
      const auto index = owner_->model()->index(row, owner_->modelColumn(), owner_->rootModelIndex());
      const auto size = list->itemDelegate()->sizeHint(option, index);
      width = std::max(width, size.width());
      height += size.height();
    }
    const QRect anchor(owner_->mapToGlobal(QPoint(0, 0)), owner_->size());
    auto* screen = QGuiApplication::screenAt(anchor.center());
    if (screen == nullptr) screen = owner_->screen();
    const auto available = screen->availableGeometry();
    width = std::min(width, available.width());
    const auto preferred_height = height + 2 * inset;
    height = std::min(preferred_height, available.height());
    // Keep every row reachable when the available screen height requires scrolling.
    if (height < preferred_height)
      width = std::min(width + list->style()->pixelMetric(QStyle::PM_ScrollBarExtent), available.width());
    int y = anchor.bottom() + 5;
    if (y + height > available.bottom() + 1) y = anchor.top() - height - 4;
    setGeometry(std::clamp(anchor.left(), available.left(), available.right() - width + 1),
                std::clamp(y, available.top(), available.bottom() - height + 1), width, height);
    list->setCurrentIndex(owner_->model()->index(owner_->currentIndex(), owner_->modelColumn(), owner_->rootModelIndex()));
    show();
    list->scrollTo(list->currentIndex());
    list->setFocus(Qt::PopupFocusReason);
  }
  QListView* list;
 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (watched == list && event->type() == QEvent::KeyPress) {
      const auto key = static_cast<QKeyEvent*>(event)->key();
      if (key == Qt::Key_Escape) { owner_->hidePopup(); return true; }
      if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) {
        const auto index = list->currentIndex();
        if (index.isValid() && (index.flags() & Qt::ItemIsEnabled)) owner_->commit_row(index.row());
        return true;
      }
    }
    return QWidget::eventFilter(watched, event);
  }
  void hideEvent(QHideEvent* event) override {
    QWidget::hideEvent(event);
    owner_->popup_dismissed();  // also clear state when Qt dismisses outside this combo
  }
  void paintEvent(QPaintEvent*) override {
    const auto theme = colors(owner_);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(theme.border, 1));
    painter.setBrush(theme.shell);
    painter.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 10, 10);
  }
 private:
  AnnotationSizeCombo* owner_;
};

AnnotationSizeCombo::AnnotationSizeCombo(QWidget* parent)
    : QComboBox(parent), popup_(new AnnotationSizePopup(this)) { setMouseTracking(true); }
AnnotationSizeCombo::~AnnotationSizeCombo() { delete popup_; }
void AnnotationSizeCombo::showPopup() {
  if (isEnabled() && count() != 0) {
    pressed_ = false;  // the popup grabs the release; never depend on it returning here
    popup_->open();
    update();
  }
}
void AnnotationSizeCombo::hidePopup() {
  popup_->hide();
  popup_dismissed();
}
void AnnotationSizeCombo::popup_dismissed() {
  pressed_ = false;
  hovered_ = false;
  QComboBox::hidePopup();
  update();
}
QSize AnnotationSizeCombo::sizeHint() const {
  return annotation_size_combo_paint::preferred_size(*this);
}
QSize AnnotationSizeCombo::minimumSizeHint() const { return sizeHint(); }
void AnnotationSizeCombo::paintEvent(QPaintEvent*) {
  using annotation_size_combo_paint::State;
  const auto state = popup_->isVisible() ? State::popup_open
      : pressed_ ? State::pressed : hovered_ ? State::hover : State::normal;
  QPainter painter(this);
  annotation_size_combo_paint::paint(painter, *this, state);
  QStyleOptionComboBox option;
  initStyleOption(&option);
  if (hasFocus() && (option.state & QStyle::State_KeyboardFocusChange)) {
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(palette().color(QPalette::Highlight), 1));
    painter.drawRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -2), 4, 4);
  }
}
void AnnotationSizeCombo::enterEvent(QEnterEvent* event) {
  hovered_ = isEnabled();
  update();
  QComboBox::enterEvent(event);
}
void AnnotationSizeCombo::leaveEvent(QEvent* event) {
  hovered_ = false;
  update();
  QComboBox::leaveEvent(event);
}
void AnnotationSizeCombo::mousePressEvent(QMouseEvent* event) {
  pressed_ = isEnabled() && event->button() == Qt::LeftButton;
  QComboBox::mousePressEvent(event);
  if (popup_->isVisible()) pressed_ = false;
  update();
}
void AnnotationSizeCombo::mouseReleaseEvent(QMouseEvent* event) {
  pressed_ = false;
  QComboBox::mouseReleaseEvent(event);
  update();
}
void AnnotationSizeCombo::mouseMoveEvent(QMouseEvent* event) {
  hovered_ = isEnabled() && rect().contains(event->position().toPoint());
  QComboBox::mouseMoveEvent(event);
  update();
}
void AnnotationSizeCombo::hideEvent(QHideEvent* event) {
  hidePopup();
  QComboBox::hideEvent(event);
}
void AnnotationSizeCombo::changeEvent(QEvent* event) {
  if (event->type() == QEvent::EnabledChange && !isEnabled()) hidePopup();
  QComboBox::changeEvent(event);
  if (event->type() == QEvent::FontChange) updateGeometry();
  update();
}
void AnnotationSizeCombo::commit_row(const int row) {
  // Existing currentTextChanged callbacks can clear/repopulate the same model.
  // Hide before committing, and retain only a row number across those callbacks.
  hidePopup();
  QPointer<AnnotationSizeCombo> self(this);
  setCurrentIndex(row);
  if (!self) return;
  const auto text = currentText();
  emit activated(row);
  if (self) emit textActivated(text);
}
}  // namespace hdrshot
