#include "ui/qt/analyzer_swatch_panel.hpp"
#include "ui/qt/analyzer_control_style.hpp"

#include "domain/analysis/math.hpp"

#include <QApplication>
#include <QClipboard>
#include <QBoxLayout>
#include <QEvent>
#include <QFocusEvent>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QStyle>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace hdrshot {
namespace {
constexpr qint64 pair_success_hold_ms = 120;
constexpr qint64 pair_success_fade_ms = 350;
constexpr qint64 pair_success_total_ms = pair_success_hold_ms + pair_success_fade_ms;

class TransientOverlay final : public QWidget {
public:
  TransientOverlay(QWidget *parent, std::function<void(QPainter &)> paint)
      : QWidget(parent), paint_(std::move(paint)) {
    setObjectName("analyzerSwatchTransientOverlay");
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    if (!paint_)
      return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    paint_(painter);
  }

private:
  std::function<void(QPainter &)> paint_;
};

QColor color_of(const std::optional<std::array<float, 3>> &value) {
  if (!value)
    return QColor("#444950");
  return QColor::fromRgbF(float(std::clamp(double((*value)[0]), 0., 1.)),
                          float(std::clamp(double((*value)[1]), 0., 1.)),
                          float(std::clamp(double((*value)[2]), 0., 1.)));
}

class PairHandle final : public QToolButton {
public:
  explicit PairHandle(std::uint64_t id, QWidget *parent) : QToolButton(parent), id_(id) {
    setProperty("swatchId", QVariant::fromValue<qulonglong>(id));
    QPixmap symbol(40, 40);
    symbol.setDevicePixelRatio(2);
    symbol.fill(Qt::transparent);
    QPainter painter(&symbol);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor("#d9e7f6"), 1.8, Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin));
    painter.drawPolyline(QPolygonF{{2.5, 16}, {9, 3}, {15.5, 16}, {2.5, 16}});
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#79c7ff"));
    painter.drawEllipse(QPointF(17, 9.5), 2, 2);
    painter.end();
    setIcon(QIcon(symbol));
    analyzer_control_style::set_icon_button(this);
    setObjectName("analyzerSwatchPairHandle" + QString::number(id));
    setAccessibleName("配对色样 #" + QString::number(id));
    setToolTip("Δ 配对：拖动或点击，再选择目标 Δ");
    setCursor(Qt::CrossCursor);
    setFocusPolicy(Qt::StrongFocus);
  }
  std::function<void(std::uint64_t, QPoint)> dropped;
  std::function<void(std::uint64_t)> clickedPair;
  std::function<void(QWidget *)> pressStarted;
  std::function<void(std::uint64_t, QPoint)> dragStarted;
  std::function<void(std::uint64_t, QPoint)> dragMoved;

protected:
  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() != Qt::LeftButton)
      return QToolButton::mousePressEvent(event);
    pressed_ = event->position().toPoint();
    dragging_ = false;
    canceled_ = false;
    if (pressStarted)
      pressStarted(this);
    event->accept();
  }
  void mouseMoveEvent(QMouseEvent *event) override {
    if (canceled_) {
      event->accept();
      return;
    }
    if (!(event->buttons() & Qt::LeftButton))
      return QToolButton::mouseMoveEvent(event);
    if (!dragging_ && (event->position().toPoint() - pressed_).manhattanLength() >=
                           QApplication::startDragDistance()) {
      dragging_ = true;
      if (dragStarted)
        dragStarted(id_, event->globalPosition().toPoint());
    }
    if (dragging_ && dragMoved)
      dragMoved(id_, event->globalPosition().toPoint());
    event->accept();
  }
  void mouseReleaseEvent(QMouseEvent *event) override {
    if (event->button() != Qt::LeftButton)
      return;
    if (canceled_) {
      canceled_ = false;
      event->accept();
      return;
    }
    const bool was_dragging = dragging_;
    dragging_ = false;
    if (was_dragging) {
      if (dropped)
        dropped(id_, event->globalPosition().toPoint());
    } else if (clickedPair) {
      clickedPair(id_);
    }
    event->accept();
  }
public:
  void cancel_drag() {
    dragging_ = false;
    canceled_ = true;
    if (QWidget::mouseGrabber() == this)
      releaseMouse();
  }

private:
  std::uint64_t id_{};
  QPoint pressed_;
  bool dragging_{};
  bool canceled_{};
};

QLabel *color_chip(const QString &name, const QColor &color, QWidget *parent) {
  auto *chip = new QLabel(parent);
  chip->setObjectName(name);
  chip->setFixedSize(20, 20);
  chip->setStyleSheet(QString("background:%1;border-radius:3px;").arg(color.name()));
  return chip;
}
QIcon unlink_icon() {
  QPixmap pixmap(analyzer_control_style::icon_canvas_side * 2,
                 analyzer_control_style::icon_canvas_side * 2);
  pixmap.setDevicePixelRatio(2);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(QPen(QColor(analyzer_control_style::danger_foreground), 2, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
  painter.drawRoundedRect(QRect(2, 6, 9, 6), 3, 3);
  painter.drawRoundedRect(QRect(9, 8, 9, 6), 3, 3);
  painter.drawLine(QPoint(6, 15), QPoint(14, 5));
  return QIcon(pixmap);
}
bool same_request(const analysis::SampleRequest &a,
                  const analysis::SampleRequest &b) {
  return a.id == b.id && a.x == b.x && a.y == b.y && a.side == b.side &&
         a.respect_mask == b.respect_mask;
}
QLabel *find_color(QWidget *card, const QString &name) {
  return card->findChild<QLabel *>(name);
}
const analysis::SampleResult *sample_for(const analysis::ResultRef &result,
                                         const analysis::SampleRequest &request) {
  if (!result)
    return nullptr;
  for (const auto &sample : result->samples)
    if (same_request(sample.request, request))
      return &sample;
  return nullptr;
}
} // namespace

AnalyzerSwatchPanel::AnalyzerSwatchPanel(QWidget *parent) : QWidget(parent) {
  pair_success_clock_.start();
  pair_success_timer_ = new QTimer(this);
  pair_success_timer_->setInterval(16);
  connect(pair_success_timer_, &QTimer::timeout, this,
          [this] { update_pair_success(); });
  setObjectName("analyzerSwatchPanel");
  setAttribute(Qt::WA_OpaquePaintEvent, false);
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  scroll_ = new QScrollArea(this);
  scroll_->setObjectName("analyzerSwatchScroll");
  scroll_->setWidgetResizable(true);
  scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  content_ = new QWidget;
  content_->setObjectName("analyzerSwatchContent");
  content_->setStyleSheet("QWidget#analyzerSwatchContent{background:#292b2f;}");
  layout_ = new QVBoxLayout(content_);
  layout_->setContentsMargins(7, 7, 7, 7);
  layout_->setSpacing(6);
  layout_->addStretch();
  scroll_->setWidget(content_);
  auto *viewport = scroll_->viewport();
  viewport->installEventFilter(this);
  scroll_->verticalScrollBar()->installEventFilter(this);
  transient_overlay_ = new TransientOverlay(
      viewport, [this](QPainter &painter) { draw_transient(painter); });
  transient_overlay_->setGeometry(viewport->rect());
  transient_overlay_->raise();
  connect(scroll_->verticalScrollBar(), &QScrollBar::valueChanged,
          transient_overlay_, qOverload<>(&QWidget::update));
  root->addWidget(scroll_, 1);
}

QString AnalyzerSwatchPanel::pair_key(const analysis::SwatchPair &pair) {
  return QString::number(std::min(pair.source_id, pair.target_id)) + "_" +
         QString::number(std::max(pair.source_id, pair.target_id));
}

void AnalyzerSwatchPanel::set_samples_and_result(const analysis::Request &request,
                                                 analysis::ResultRef result) {
  request_ = request;
  result_ = std::move(result);
  // Deleting a swatch invalidates all incident pairs. The model does not own
  // requests, so prune here before local rendering.
  std::vector<std::uint64_t> ids;
  for (const auto &sample : request_.samples)
    if (sample.id)
      ids.push_back(sample.id);
  std::vector<std::uint64_t> remove_ids;
  for (const auto &pair : pairs_.pairs())
    if (std::find(ids.begin(), ids.end(), pair.source_id) == ids.end() ||
        std::find(ids.begin(), ids.end(), pair.target_id) == ids.end())
      remove_ids.push_back(std::find(ids.begin(), ids.end(), pair.source_id) == ids.end()
                               ? pair.source_id : pair.target_id);
  for (const auto id : remove_ids)
    pairs_.remove_swatch(id);
  prune_pair_success();
  if (pending_source_ && std::find(ids.begin(), ids.end(), *pending_source_) == ids.end())
    cancel_pair_gesture();
  std::vector<analysis::SampleRequest> fixed;
  for (const auto &sample : request_.samples)
    if (sample.id)
      fixed.push_back(sample);
  bool same_shape = fixed.size() == rendered_fixed_.size() &&
                    pairs_.pairs() == rendered_pairs_;
  if (same_shape)
    for (std::size_t i = 0; i < fixed.size(); ++i)
      if (!same_request(fixed[i], rendered_fixed_[i])) {
        same_shape = false;
        break;
      }
  if (same_shape)
    update_existing();
  else
    rebuild();
  update();
}

void AnalyzerSwatchPanel::set_reading_formatter(
    std::function<QString(const analysis::Readout &, bool)> formatter) {
  reading_formatter_ = std::move(formatter);
  if (layout_)
    update_existing();
}

void AnalyzerSwatchPanel::remove_swatch(std::uint64_t id) {
  pairs_.remove_swatch(id);
  prune_pair_success();
  if (pending_source_ && *pending_source_ == id)
    cancel_pair_gesture();
  if (removeSwatch)
    removeSwatch(id);
}

void AnalyzerSwatchPanel::set_transient_hidden(bool hidden) {
  transient_hidden_ = hidden;
  apply_pair_success_opacity();
  if (hidden && transient_target_) {
    transient_target_->setProperty("swatchPairTarget", false);
    transient_target_->style()->unpolish(transient_target_);
    transient_target_->style()->polish(transient_target_);
    transient_target_ = nullptr;
  }
  update_transient_overlay();
  update();
}

bool AnalyzerSwatchPanel::cancel_pair_gesture() {
  if (!pending_source_ && !drag_handle_)
    return false;
  if (auto *handle = dynamic_cast<PairHandle *>(drag_handle_.data()))
    handle->cancel_drag();
  drag_handle_ = nullptr;
  pending_source_.reset();
  transient_cursor_ = {};
  transient_cursor_valid_ = false;
  transient_pair_.reset();
  if (transient_target_) {
    transient_target_->setProperty("swatchPairTarget", false);
    transient_target_->style()->unpolish(transient_target_);
    transient_target_->style()->polish(transient_target_);
    transient_target_ = nullptr;
  }
  update_transient_overlay();
  update();
  return true;
}

QWidget *AnalyzerSwatchPanel::card_at(const QPoint &global) const {
  if (!scroll_ ||
      !scroll_->viewport()->rect().contains(scroll_->viewport()->mapFromGlobal(global)))
    return nullptr;
  // Do not reinterpret another top-level analyzer window's card coordinates
  // in this panel's local geometry when a drag crosses between windows.
  QWidget *under_pointer = QApplication::widgetAt(global);
  if (!under_pointer)
    return nullptr;
  QWidget *ancestor = under_pointer;
  while (ancestor && ancestor != this)
    ancestor = ancestor->parentWidget();
  if (!ancestor)
    return nullptr;
  const QPoint content_point = content_->mapFromGlobal(global);
  for (auto *card : content_->findChildren<QFrame *>())
    if (card->property("swatchId").isValid() && card->geometry().contains(content_point))
      return card;
  return nullptr;
}

QWidget *AnalyzerSwatchPanel::handle_for(std::uint64_t id) const {
  return findChild<QWidget *>("analyzerSwatchPairHandle" + QString::number(id));
}

void AnalyzerSwatchPanel::try_pair(std::uint64_t source, std::uint64_t target) {
  if (!source || !target)
    return;
  if (source == target) {
    cancel_pair_gesture();
    return;
  }
  const auto outcome = pairs_.add_pair(source, target, request_.samples);
  drag_handle_ = nullptr;
  pending_source_.reset();
  transient_cursor_ = {};
  transient_cursor_valid_ = false;
  transient_pair_.reset();
  if (transient_target_) {
    transient_target_->setProperty("swatchPairTarget", false);
    transient_target_->style()->unpolish(transient_target_);
    transient_target_->style()->polish(transient_target_);
    transient_target_ = nullptr;
  }
  if (outcome == analysis::PairAddResult::added)
    pair_successes_.push_back({{source, target}, pair_success_clock_.elapsed()});
  if (outcome == analysis::PairAddResult::added) {
    pair_success_timer_->start();
    QTimer::singleShot(0, this, [this] { rebuild(); });
  }
  update_transient_overlay();
  update();
}

void AnalyzerSwatchPanel::set_transient_target(QWidget *target) {
  if (transient_target_ == target)
    return;
  if (transient_target_) {
    transient_target_->setProperty("swatchPairTarget", false);
    transient_target_->style()->unpolish(transient_target_);
    transient_target_->style()->polish(transient_target_);
  }
  transient_target_ = target;
  if (transient_target_) {
    transient_target_->setProperty("swatchPairTarget", true);
    transient_target_->style()->unpolish(transient_target_);
    transient_target_->style()->polish(transient_target_);
  }
  update_transient_overlay();
}

void AnalyzerSwatchPanel::update_transient_overlay() {
  if (transient_overlay_)
    transient_overlay_->update();
}

void AnalyzerSwatchPanel::prune_pair_success() {
  const auto &pairs = pairs_.pairs();
  std::erase_if(pair_successes_, [&](const PairSuccess &success) {
    return std::find(pairs.begin(), pairs.end(), success.pair) == pairs.end();
  });
  if (pair_successes_.empty())
    pair_success_timer_->stop();
  apply_pair_success_opacity();
  update_transient_overlay();
}

void AnalyzerSwatchPanel::apply_pair_success_opacity() {
  if (!content_)
    return;
  const auto now = pair_success_clock_.elapsed();
  for (auto *row : content_->findChildren<QFrame *>()) {
    if (!row->property("pairSource").isValid())
      continue;
    const auto source = row->property("pairSource").toULongLong();
    const auto target = row->property("pairTarget").toULongLong();
    const auto it = std::find_if(pair_successes_.begin(), pair_successes_.end(),
                                 [&](const PairSuccess &success) {
                                   return success.pair.source_id == source &&
                                          success.pair.target_id == target;
                                 });
    const qreal progress = it == pair_successes_.end() || transient_hidden_
                               ? 1.0
                               : std::clamp(qreal(now - it->started_ms - pair_success_hold_ms) /
                                                qreal(pair_success_fade_ms),
                                            0.0, 1.0);
    if (progress >= 1.0) {
      if (row->graphicsEffect())
        row->setGraphicsEffect(nullptr);
    } else {
      auto *effect = qobject_cast<QGraphicsOpacityEffect *>(row->graphicsEffect());
      if (!effect) {
        effect = new QGraphicsOpacityEffect(row);
        row->setGraphicsEffect(effect);
      }
      effect->setOpacity(progress);
    }
  }
}

void AnalyzerSwatchPanel::update_pair_success() {
  const auto now = pair_success_clock_.elapsed();
  std::erase_if(pair_successes_, [&](const PairSuccess &success) {
    return now - success.started_ms >= pair_success_total_ms;
  });
  if (pair_successes_.empty())
    pair_success_timer_->stop();
  apply_pair_success_opacity();
  update_transient_overlay();
}

void AnalyzerSwatchPanel::rebuild() {
  if (!layout_)
    return;
  prune_pair_success();
  const int scroll = scroll_restore_pending_ ? scroll_restore_target_
                                             : scroll_->verticalScrollBar()->value();
  scroll_restore_pending_ = false;
  const auto restore_generation = ++scroll_restore_generation_;
  set_transient_target(nullptr);
  transient_pair_.reset();
  while (layout_->count() > 1) {
    auto *item = layout_->takeAt(0);
    delete item->widget();
    delete item;
  }
  std::vector<analysis::SampleRequest> fixed;
  for (const auto &sample : request_.samples)
    if (sample.id)
      fixed.push_back(sample);
  const auto evaluations = pairs_.evaluate(request_, result_);
  for (const auto &sample : fixed) {
    const analysis::SampleResult *data = nullptr;
    if (result_ && result_->settings == request_.settings)
      for (const auto &value : result_->samples)
        if (value.request.id == sample.id && value.request.x == sample.x &&
            value.request.y == sample.y && value.request.side == sample.side &&
            value.request.respect_mask == sample.respect_mask) {
          data = &value;
          break;
        }
    auto *card = new QFrame(content_);
    card->setProperty("swatchId", QVariant::fromValue<qulonglong>(sample.id));
    card->setObjectName("analyzerSwatch" + QString::number(sample.id));
    card->setProperty("swatchCard", true);
    card->setStyleSheet("QFrame[swatchCard=\"true\"]{border:1px solid #424750;"
                        "border-radius:5px;background:#292b2f;}"
                        "QFrame[swatchCard=\"true\"][swatchPairTarget=\"true\"]"
                        "{border-color:#79c7ff;}");
    auto *body = new QVBoxLayout(card);
    body->setContentsMargins(7, 6, 7, 6);
    body->setSpacing(5);
    auto *header = new QWidget(card);
    auto *row = new QHBoxLayout(header);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(7);
    const std::optional<std::array<float, 3>> color =
        data && data->mean.valid_count ? std::optional(data->mean.display_rgb)
                                       : std::nullopt;
    auto *primary_color = color_chip("analyzerSwatchColor", color_of(color), header);
    primary_color->setFixedSize(44, 44);
    row->addWidget(primary_color);
    auto *title = new QLabel(QString("#%1 · %2, %3 · %4×%4")
                                 .arg(sample.id).arg(sample.x).arg(sample.y).arg(sample.side),
                             header);
    title->setObjectName("analyzerSwatchLabel" + QString::number(sample.id));
    title->setWordWrap(true);
    title->setStyleSheet("font-size:11px;");
    row->addWidget(title, 1);
    auto *copy = new QToolButton(header);
    copy->setObjectName("analyzerCopySwatch" + QString::number(sample.id));
    copy->setIcon(copy_icon_);
    copy->setToolTip("复制色样 #" + QString::number(sample.id) + " 的读数");
    copy->setAccessibleName(copy->toolTip());
    analyzer_control_style::set_icon_button(copy);
    row->addWidget(copy, 0, Qt::AlignVCenter);
    auto *handle = new PairHandle(sample.id, header);
    handle->pressStarted = [this](QWidget *source) { drag_handle_ = source; };
    handle->clickedPair = [this](std::uint64_t id) {
      drag_handle_ = nullptr;
      if (pending_source_) {
        try_pair(*pending_source_, id);
        return;
      }
      pending_source_ = id;
      transient_cursor_valid_ = false;
      transient_pair_.reset();
      update_transient_overlay();
      update();
    };
    handle->dropped = [this](std::uint64_t id, QPoint global) {
      if (auto *target = card_at(global))
        try_pair(id, target->property("swatchId").toULongLong());
      else {
        cancel_pair_gesture();
      }
    };
    handle->dragStarted = [this](std::uint64_t id, QPoint global) {
      pending_source_ = id;
      transient_cursor_ = mapFromGlobal(global);
      transient_cursor_valid_ = true;
      transient_pair_.reset();
      update_transient_overlay();
      update();
    };
    handle->dragMoved = [this](std::uint64_t, QPoint global) {
      transient_cursor_ = mapFromGlobal(global);
      transient_cursor_valid_ = true;
      set_transient_target(card_at(global));
      update_transient_overlay();
      update();
    };
    row->addWidget(handle, 0, Qt::AlignVCenter);
    auto *remove = new QToolButton(header);
    remove->setObjectName("analyzerRemoveSwatch" + QString::number(sample.id));
    remove->setIcon(analyzer_control_style::danger_glyph(false));
    remove->setToolTip("删除色样 #" + QString::number(sample.id));
    remove->setAccessibleName("删除色样 " + QString::number(sample.id));
    analyzer_control_style::set_icon_button(remove, true);
    row->addWidget(remove, 0, Qt::AlignVCenter);
    const auto id = sample.id;
    connect(remove, &QToolButton::clicked, this, [this, id] { remove_swatch(id); });
    header->installEventFilter(this);
    card->installEventFilter(this);
    body->addWidget(header);
    auto *readout = new QLabel(card);
    readout->setObjectName("analyzerSwatchReadout" + QString::number(sample.id));
    readout->setWordWrap(false);
    readout->setMinimumWidth(0);
    readout->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    readout->setStyleSheet("font-size:11px;");
    if (data && result_ && result_->settings == request_.settings &&
        data->mean.valid_count)
      readout->setText(reading_formatter_ ? reading_formatter_(data->mean, true)
                                          : "—");
    else
      readout->setText("—");
    readout->setToolTip(readout->text());
    connect(copy, &QToolButton::clicked, this, [title, readout] {
      if (auto *clipboard = QGuiApplication::clipboard())
        clipboard->setText(title->text() + "\n" + readout->text());
    });
    body->addWidget(readout);
    for (const auto &evaluation : evaluations) {
      if (evaluation.pair.target_id != sample.id)
        continue;
      auto *pair_row = new QFrame(card);
      pair_row->setObjectName("analyzerSwatchPairRow" + pair_key(evaluation.pair));
      pair_row->setProperty("pairSource", QVariant::fromValue<qulonglong>(evaluation.pair.source_id));
      pair_row->setProperty("pairTarget", QVariant::fromValue<qulonglong>(evaluation.pair.target_id));
      pair_row->setAccessibleName("配对色样 #" + QString::number(evaluation.pair.source_id) +
                                  " 与 #" + QString::number(evaluation.pair.target_id));
      pair_row->setProperty("swatchPairRow", true);
      pair_row->setStyleSheet("QFrame[swatchPairRow=\"true\"]"
                              "{border:0;background:#202328;}");
      auto *pair_layout = new QBoxLayout(QBoxLayout::TopToBottom, pair_row);
      pair_layout->setContentsMargins(4, 4, 4, 4);
      pair_layout->setSpacing(2);
      const auto suffix = pair_key(evaluation.pair);
      auto *endpoints = new QWidget(pair_row);
      endpoints->setObjectName("analyzerSwatchPairEndpoints" + suffix);
      auto *endpoint_layout = new QHBoxLayout(endpoints);
      endpoint_layout->setContentsMargins(0, 0, 0, 0);
      endpoint_layout->setSpacing(4);
      endpoint_layout->setAlignment(Qt::AlignLeft);
      auto *color_a = color_chip("analyzerPairColorA_" + suffix,
                                 color_of(evaluation.source_color), endpoints);
      auto *color_b = color_chip("analyzerPairColorB_" + suffix,
                                 color_of(evaluation.target_color), endpoints);
      color_a->setAccessibleName("配对色样 #" + QString::number(evaluation.pair.source_id) + " 颜色");
      color_b->setAccessibleName("配对色样 #" + QString::number(evaluation.pair.target_id) + " 颜色");
      endpoint_layout->addWidget(color_a);
      endpoint_layout->addWidget(new QLabel("#" + QString::number(evaluation.pair.source_id), endpoints));
      endpoint_layout->addWidget(new QLabel("↔", endpoints));
      endpoint_layout->addWidget(color_b);
      endpoint_layout->addWidget(new QLabel("#" + QString::number(evaluation.pair.target_id), endpoints));
      pair_layout->addWidget(endpoints);
      auto *result_line = new QWidget(pair_row);
      result_line->setObjectName("analyzerSwatchPairResultLine" + suffix);
      auto *result_layout = new QHBoxLayout(result_line);
      result_layout->setContentsMargins(0, 0, 0, 0);
      result_layout->setSpacing(4);
      auto *value = new QLabel(evaluation.value ? QString::number(*evaluation.value, 'f', 2) : "—", pair_row);
      value->setObjectName("analyzerSwatchPairValue" + suffix);
      value->setToolTip(evaluation.state == analysis::PairState::pending
                            ? "配对结果等待当前采样完成"
                            : evaluation.state == analysis::PairState::unavailable
                                ? "配对样本无有效值"
                                : evaluation.metric == analysis::PairMetric::itp ? "ΔE_ITP" : "ΔE00");
      auto *metric = new QLabel(evaluation.metric == analysis::PairMetric::itp ? "ΔE_ITP" : "ΔE00", result_line);
      metric->setObjectName("analyzerSwatchPairMetric" + suffix);
      auto *separator = new QLabel("│", result_line);
      separator->setObjectName("analyzerSwatchPairSeparator" + suffix);
      result_layout->addWidget(separator);
      result_layout->addWidget(metric);
      result_layout->addWidget(value, 0, Qt::AlignLeft);
      result_layout->addStretch(1);
      auto *unlink = new analyzer_control_style::CenteredTextToolButton(pair_row);
      unlink->setObjectName("analyzerUnlinkSwatchPair" + suffix);
      unlink->setIcon(unlink_icon());
      unlink->setIconSize(QSize(16, 16));
      unlink->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
      unlink->setText("解除");
      unlink->setToolTip("解除 #" + QString::number(evaluation.pair.source_id) +
                         " ↔ #" + QString::number(evaluation.pair.target_id));
      unlink->setAccessibleName(unlink->toolTip());
      analyzer_control_style::set_danger_text_button(unlink);
      result_layout->addWidget(unlink);
      pair_layout->addWidget(result_line);
      const auto source_id = evaluation.pair.source_id;
      const auto target_id = evaluation.pair.target_id;
      connect(unlink, &QToolButton::clicked, this, [this, source_id, target_id] {
        if (pairs_.remove_pair(source_id, target_id)) {
          QTimer::singleShot(0, this, [this] {
            rebuild();
            update();
          });
        }
      });
      pair_row->setFocusPolicy(Qt::StrongFocus);
      pair_row->installEventFilter(this);
      body->addWidget(pair_row);
    }
    layout_->insertWidget(layout_->count() - 1, card);
    for (auto *child : card->findChildren<QWidget *>())
      child->installEventFilter(this);
  }
  update_pair_row_directions();
  content_->layout()->activate();
  content_->adjustSize();
  scroll_->verticalScrollBar()->setValue(scroll);
  scroll_restore_target_ = scroll;
  scroll_restore_pending_ = true;
  const int value_after_rebuild = scroll_->verticalScrollBar()->value();
  QTimer::singleShot(0, this, [this, restore_generation, value_after_rebuild] {
    if (!scroll_restore_pending_ || restore_generation != scroll_restore_generation_)
      return;
    auto *bar = scroll_->verticalScrollBar();
    if (bar->value() != value_after_rebuild) {
      scroll_restore_pending_ = false;
      return;
    }
    content_->layout()->activate();
    content_->adjustSize();
    scroll_restore_pending_ = false;
    bar->setValue(std::min(scroll_restore_target_, bar->maximum()));
  });
  rendered_fixed_.clear();
  for (const auto &sample : request_.samples)
    if (sample.id)
      rendered_fixed_.push_back(sample);
  rendered_pairs_ = pairs_.pairs();
  apply_pair_success_opacity();
}

void AnalyzerSwatchPanel::update_pair_row_directions() {
  if (!scroll_)
    return;
  const auto direction = scroll_->viewport()->width() < 400
                             ? QBoxLayout::TopToBottom
                             : QBoxLayout::LeftToRight;
  for (auto *row : content_->findChildren<QFrame *>()) {
    if (!row->objectName().startsWith("analyzerSwatchPairRow"))
      continue;
    if (auto *layout = qobject_cast<QBoxLayout *>(row->layout())) {
      layout->setDirection(direction);
      layout->setStretch(0, 0);
      layout->setStretch(1, direction == QBoxLayout::LeftToRight ? 1 : 0);
    }
  }
}

void AnalyzerSwatchPanel::update_existing() {
  const bool compatible = result_ && result_->settings == request_.settings;
  const auto evaluations = pairs_.evaluate(request_, result_);
  for (const auto &sample : rendered_fixed_) {
    auto *card = content_->findChild<QFrame *>("analyzerSwatch" +
                                                QString::number(sample.id));
    if (!card)
      continue;
    const auto *data = compatible ? sample_for(result_, sample) : nullptr;
    const std::optional<std::array<float, 3>> color =
        data && data->mean.valid_count ? std::optional(data->mean.display_rgb)
                                       : std::nullopt;
    if (auto *chip = find_color(card, "analyzerSwatchColor"))
      chip->setStyleSheet(QString("background:%1;border-radius:4px;")
                              .arg(color_of(color).name()));
    if (auto *readout = card->findChild<QLabel *>("analyzerSwatchReadout" +
                                                  QString::number(sample.id))) {
      readout->setText(data && data->mean.valid_count
                           ? (reading_formatter_ ? reading_formatter_(data->mean, true)
                                                 : QString("—"))
                           : "—");
      readout->setToolTip(readout->text());
    }
  }
  for (const auto &evaluation : evaluations) {
    const auto suffix = pair_key(evaluation.pair);
    auto *row = content_->findChild<QFrame *>("analyzerSwatchPairRow" + suffix);
    if (!row)
      continue;
    if (auto *chip = find_color(row, "analyzerPairColorA_" + suffix))
      chip->setStyleSheet(QString("background:%1;border-radius:3px;")
                              .arg(color_of(evaluation.source_color).name()));
    if (auto *chip = find_color(row, "analyzerPairColorB_" + suffix))
      chip->setStyleSheet(QString("background:%1;border-radius:3px;")
                              .arg(color_of(evaluation.target_color).name()));
    if (auto *value = row->findChild<QLabel *>("analyzerSwatchPairValue" + suffix)) {
      value->setText(evaluation.value ? QString::number(*evaluation.value, 'f', 2) : "—");
      value->setToolTip(evaluation.state == analysis::PairState::pending
                            ? "配对结果等待当前采样完成"
                            : evaluation.state == analysis::PairState::unavailable
                                ? "配对样本无有效值"
                                : evaluation.metric == analysis::PairMetric::itp ? "ΔE_ITP" : "ΔE00");
    }
    if (auto *metric = row->findChild<QLabel *>("analyzerSwatchPairMetric" + suffix))
      metric->setText(evaluation.metric == analysis::PairMetric::itp ? "ΔE_ITP" : "ΔE00");
  }
}

void AnalyzerSwatchPanel::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  painter.fillRect(rect(), QColor("#292b2f"));
}

void AnalyzerSwatchPanel::draw_transient(QPainter &painter) const {
  if (transient_hidden_ || !transient_overlay_)
    return;
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setClipRect(transient_overlay_->rect());
  const auto port = [&](QWidget *handle) {
    return QPointF(transient_overlay_->mapFromGlobal(handle->mapToGlobal(
        QPoint(handle->rect().right(), handle->rect().center().y()))));
  };
  const auto curve = [&](QPointF start, QPointF end, std::uint64_t source_id,
                         std::uint64_t target_id) {
    const std::uint64_t low = std::min(source_id, target_id);
    const std::uint64_t high = std::max(source_id, target_id);
    const int lane = target_id ? int((low * 7 + high * 11) % 3) : 0;
    const qreal edge = std::min(qreal(transient_overlay_->width() - 4),
                                std::max(start.x(), end.x()) + 12 + lane * 5);
    QPainterPath path(start);
    path.cubicTo(QPointF(edge, start.y()), QPointF(edge, end.y()), end);
    painter.drawPath(path);
    painter.setBrush(QColor("#79c7ff"));
    painter.drawEllipse(start, 2.5, 2.5);
    painter.drawEllipse(end, 2.5, 2.5);
  };
  const auto now = pair_success_clock_.elapsed();
  for (const auto &success : pair_successes_) {
    auto *source = handle_for(success.pair.source_id);
    auto *target = handle_for(success.pair.target_id);
    if (!source || !target)
      continue;
    const qreal remaining = std::clamp(
        1.0 - qreal(now - success.started_ms - pair_success_hold_ms) /
                  qreal(pair_success_fade_ms), 0.0, 1.0);
    if (remaining <= 0)
      continue;
    painter.save();
    painter.setOpacity(remaining);
    painter.setPen(QPen(QColor("#79c7ff"), 2.3, Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    for (auto *handle : {source, target}) {
      const QPoint top_left = transient_overlay_->mapFromGlobal(handle->mapToGlobal(QPoint(0, 0)));
      painter.drawRoundedRect(QRectF(top_left, handle->size()).adjusted(.8, .8, -.8, -.8),
                              5, 5);
    }
    curve(port(source), port(target), success.pair.source_id, success.pair.target_id);
    painter.restore();
  }
  painter.setPen(QPen(QColor("#79c7ff"), 1.5, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
  QPointF start;
  QPointF end;
  std::uint64_t source_id = 0;
  std::uint64_t target_id = 0;
  if (pending_source_) {
    source_id = *pending_source_;
    QWidget *source = handle_for(*pending_source_);
    if (!source)
      return;
    start = port(source);
    if (!transient_cursor_valid_) {
      painter.setBrush(QColor("#79c7ff"));
      painter.drawEllipse(start, 3, 3);
      return;
    }
    end = transient_cursor_valid_
              ? transient_overlay_->mapFromGlobal(mapToGlobal(transient_cursor_))
              : start;
  } else if (transient_pair_) {
    source_id = transient_pair_->source_id;
    target_id = transient_pair_->target_id;
    QWidget *source = handle_for(transient_pair_->source_id);
    QWidget *target = handle_for(transient_pair_->target_id);
    if (!source || !target)
      return;
    start = port(source);
    end = port(target);
  } else
    return;
  // The same lane calculation serves live gestures, row hover and success.
  curve(start, end, source_id, target_id);
}

bool AnalyzerSwatchPanel::eventFilter(QObject *object, QEvent *event) {
  if (scroll_restore_pending_ &&
      (object == scroll_->viewport() || object == scroll_->verticalScrollBar()) &&
      (event->type() == QEvent::Wheel || event->type() == QEvent::MouseButtonPress ||
       event->type() == QEvent::KeyPress || event->type() == QEvent::TouchBegin ||
       event->type() == QEvent::NativeGesture))
    scroll_restore_pending_ = false;
  if (object == scroll_->viewport() && event->type() == QEvent::Resize) {
    transient_overlay_->setGeometry(scroll_->viewport()->rect());
    transient_overlay_->raise();
  }
  auto *widget = qobject_cast<QWidget *>(object);
  if (!widget)
    return QWidget::eventFilter(object, event);
  if (event->type() == QEvent::MouseMove && pending_source_) {
    transient_cursor_ = widget->mapTo(this, static_cast<QMouseEvent *>(event)->position().toPoint());
    transient_cursor_valid_ = true;
    set_transient_target(card_at(static_cast<QMouseEvent *>(event)->globalPosition().toPoint()));
    update_transient_overlay();
    update();
  }
  QWidget *pair_row = widget;
  while (pair_row && pair_row != content_ &&
         !pair_row->property("pairSource").isValid())
    pair_row = pair_row->parentWidget();
  if ((event->type() == QEvent::Enter || event->type() == QEvent::FocusIn) &&
      !transient_hidden_) {
    if (pair_row && pair_row->property("pairSource").isValid()) {
      const auto source = pair_row->property("pairSource").toULongLong();
      const auto target = pair_row->property("pairTarget").toULongLong();
      if (source && target)
        transient_pair_ = analysis::SwatchPair{source, target};
    }
    update_transient_overlay();
    update();
  }
  if (event->type() == QEvent::Leave && widget == pair_row &&
      !transient_hidden_) {
    if (pair_row && pair_row->property("pairSource").isValid()) {
      transient_pair_.reset();
      update_transient_overlay();
      update();
    }
  }
  if (event->type() == QEvent::FocusOut && widget == pair_row &&
      pair_row->property("pairSource").isValid() && !transient_hidden_) {
    QPointer<QWidget> guarded_row(pair_row);
    QTimer::singleShot(0, this, [this, guarded_row] {
      QWidget *focus = QApplication::focusWidget();
      while (focus && focus != guarded_row)
        focus = focus->parentWidget();
      if (!focus && guarded_row && transient_pair_ &&
          transient_pair_->source_id ==
              guarded_row->property("pairSource").toULongLong() &&
          transient_pair_->target_id ==
              guarded_row->property("pairTarget").toULongLong()) {
        transient_pair_.reset();
        update_transient_overlay();
        update();
      }
    });
  }
  if (event->type() == QEvent::MouseButtonPress && pending_source_ &&
      static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
    if (widget->objectName().startsWith("analyzerSwatchPairHandle") ||
        widget->objectName().startsWith("analyzerCopySwatch") ||
        widget->objectName().startsWith("analyzerUnlinkSwatchPair") ||
        widget->objectName().startsWith("analyzerRemoveSwatch"))
      return QWidget::eventFilter(object, event);
    QWidget *target = widget;
    while (target && target != content_ && !target->property("swatchId").isValid())
      target = target->parentWidget();
    if (target && target->property("swatchId").isValid()) {
      try_pair(*pending_source_, target->property("swatchId").toULongLong());
      return true;
    }
  }
  if (event->type() == QEvent::KeyPress &&
      static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape && pending_source_) {
    cancel_pair_gesture();
    return true;
  }
  return QWidget::eventFilter(object, event);
}

void AnalyzerSwatchPanel::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  update_pair_row_directions();
  update_transient_overlay();
  update();
}

void AnalyzerSwatchPanel::hideEvent(QHideEvent *event) {
  cancel_pair_gesture();
  transient_pair_.reset();
  set_transient_target(nullptr);
  QWidget::hideEvent(event);
}

} // namespace hdrshot
