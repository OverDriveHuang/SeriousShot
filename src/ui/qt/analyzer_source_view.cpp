#include "ui/qt/analyzer_source_view.hpp"

#include <QEvent>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTouchEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace hdrshot {
namespace {
class SourceOverlay final : public QWidget {
public:
  std::function<void(QPainter &)> paint;
  explicit SourceOverlay(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    if (paint)
      paint(painter);
  }
};
} // namespace

AnalyzerSourceView::AnalyzerSourceView(QWidget *parent) : QWidget(parent) {
  setObjectName("analyzerSourceView");
  setAccessibleName("Source Signal：缩放、平移、吸管与分析 Mask");
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  setMinimumSize(1, 1);
  setAttribute(Qt::WA_AcceptTouchEvents);
  surface_ = new QWidget(this);
  surface_->setObjectName("analyzerSourceSurface");
  // A native NSView is a distinct input receiver. Transparent Qt hit testing
  // alone does not enable its mouse-moved delivery (hover used to need a click).
  surface_->setMouseTracking(true);
  surface_->setAttribute(Qt::WA_AcceptTouchEvents);
  surface_->installEventFilter(this);
  surface_->setAttribute(Qt::WA_NoSystemBackground);
  auto *overlay = new SourceOverlay(this);
  overlay_ = overlay;
  overlay_->setObjectName("analyzerSourceOverlay");
  overlay->paint = [this](QPainter &p) { draw_overlay(p); };
  setCursor(Qt::OpenHandCursor);
}
AnalyzerSourceView::~AnalyzerSourceView() = default;
QWidget *AnalyzerSourceView::presentation_surface() {
  // Native Qt children share the window backing store: a nominally transparent
  // sibling can carry an opaque copy of its parent's background. Native output
  // therefore owns ONE surface, including the explicit transparent marks image.
  // The QWidget overlay remains only for fixture/report raster rendering.
  native_presentation_ = true;
  overlay_->hide();
  surface_->setAttribute(Qt::WA_NativeWindow);
  (void)surface_->winId();
  return surface_;
}
void AnalyzerSourceView::set_source_size(QSize size) {
  source_size_ = size.expandedTo(QSize(1, 1));
  notify_view();
}
void AnalyzerSourceView::set_tools(bool picker, bool mask, bool ellipse) {
  if (picker_ == picker && mask_armed_ == mask && ellipse_ == ellipse)
    return;
  cancel_gesture();
  picker_ = picker;
  mask_armed_ = mask && !picker;
  ellipse_ = ellipse;
  setCursor(mask || picker ? Qt::CrossCursor : Qt::OpenHandCursor);
  invalidate_overlay();
}
void AnalyzerSourceView::set_sample_size(int size) {
  if (sample_size_ == size)
    return;
  sample_size_ = size;
  invalidate_overlay();
}
void AnalyzerSourceView::set_mask(std::optional<AnalyzerSourceMask> mask) {
  mask_ = mask;
  invalidate_overlay();
}
void AnalyzerSourceView::set_pins(std::vector<AnalyzerSourcePin> pins) {
  if (pins.size() == pins_.size() &&
      std::equal(pins.begin(), pins.end(), pins_.begin(), [](const auto &a, const auto &b) {
        return a.id == b.id && a.source == b.source && a.sample_size == b.sample_size;
      }))
    return;
  pins_ = std::move(pins);
  invalidate_overlay();
}
void AnalyzerSourceView::set_transform(AnalyzerSourceTransform transform) {
  transform_ = transform;
  if (!std::isfinite(transform_.scale))
    transform_.scale = 1.;
  transform_.scale = std::clamp(transform_.scale, .01, 64.);
  if (!std::isfinite(transform_.pan.x()) || !std::isfinite(transform_.pan.y()))
    transform_.pan = {};
  notify_view();
}
double AnalyzerSourceView::effective_scale() const {
  return transform_.fit ? std::min(double(width()) / source_size_.width(),
                                   double(height()) / source_size_.height())
                        : transform_.scale;
}
QRectF AnalyzerSourceView::image_rect() const {
  const double scale = effective_scale();
  const QSizeF extent(source_size_.width() * scale,
                      source_size_.height() * scale);
  return QRectF(QPointF((width() - extent.width()) * .5,
                        (height() - extent.height()) * .5) +
                    transform_.pan,
                extent);
}
QPointF AnalyzerSourceView::source_at(QPointF local) const {
  return (local - image_rect().topLeft()) / std::max(.00001, effective_scale());
}
QPointF AnalyzerSourceView::local_at(QPointF source) const {
  return image_rect().topLeft() + source * effective_scale();
}
QRectF AnalyzerSourceView::sample_bounds(QPointF source, int size) const {
  const int half = (size - 1) / 2;
  return QRectF(std::floor(source.x()) - half, std::floor(source.y()) - half,
                size, size)
      .intersected(QRectF(QPointF(), source_size_));
}
bool AnalyzerSourceView::valid(QPointF source) const {
  return source.x() >= 0 && source.y() >= 0 &&
         source.x() < source_size_.width() &&
         source.y() < source_size_.height();
}
QPointF AnalyzerSourceView::bounded(QPointF source) const {
  return {std::clamp(source.x(), 0., source_size_.width() - .001),
          std::clamp(source.y(), 0., source_size_.height() - .001)};
}
void AnalyzerSourceView::fit() {
  transform_ = {};
  notify_view();
}
void AnalyzerSourceView::zoom_at(QPointF local, double factor) {
  const QPointF anchor = source_at(local);
  transform_.scale = std::clamp(effective_scale() * factor, .01, 64.);
  transform_.fit = false;
  transform_.pan += local - local_at(anchor);
  notify_view();
}
void AnalyzerSourceView::notify_view() {
  update();
  invalidate_overlay();
  if (view_changed)
    view_changed();
}
bool AnalyzerSourceView::transient_visible() const {
  return !report_mode_ && !multi_gesture_ && hover_.has_value() && !(pressed_ && mask_armed_);
}
void AnalyzerSourceView::set_report_mode(bool active) {
  report_mode_ = active;
  update();
  invalidate_overlay();
}
void AnalyzerSourceView::set_fixture_image(QImage image) {
  fixture_image_ = std::move(image);
  update();
}
void AnalyzerSourceView::resizeEvent(QResizeEvent *) {
  surface_->setGeometry(rect());
  overlay_->setGeometry(rect());
  if (!native_presentation_)
    overlay_->raise();
  notify_view();
}
void AnalyzerSourceView::paintEvent(QPaintEvent *) {
  QPainter p(this);
  if (report_mode_) {
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), Qt::transparent);
    return;
  }
  p.fillRect(rect(), QColor("#171b21"));
  if (!fixture_image_.isNull()) {
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(image_rect(), fixture_image_);
  }
}
void AnalyzerSourceView::draw_overlay(QPainter &p) {
  p.setFont(font());
  p.setRenderHint(QPainter::Antialiasing);
  p.setClipRect(rect());
  if (mask_) {
    QPainterPath shape;
    const QRectF bounds(local_at(mask_->bounds.topLeft()),
                        local_at(mask_->bounds.bottomRight()));
    if (mask_->ellipse)
      shape.addEllipse(bounds);
    else
      shape.addRect(bounds);
    QPainterPath outside;
    outside.addRect(rect());
    outside.addPath(shape);
    outside.setFillRule(Qt::OddEvenFill);
    if (!fixture_image_.isNull() && !report_mode_)
      p.fillPath(outside, QColor(0, 0, 0, 160));
    p.setPen(QPen(QColor("#f4f8fc"), 1.3, Qt::DashLine));
    p.drawPath(shape);
  }
  if (picker_ && transient_visible()) {
    const QRectF b = sample_bounds(*hover_, sample_size_);
    QRectF r(local_at(b.topLeft()), local_at(b.bottomRight()));
    p.save();
    p.setClipRect(image_rect(), Qt::IntersectClip);
    p.setPen(QPen(QColor("#17202a"), 3));
    p.drawRect(r);
    p.setPen(QPen(QColor("#ffdb7b"), 1));
    p.drawRect(r);
    const auto center = r.center();
    p.drawLine(center - QPointF(7, 0), center + QPointF(7, 0));
    p.drawLine(center - QPointF(0, 7), center + QPointF(0, 7));
    p.restore();
  }
  for (const auto &pin : pins_) {
    const QPointF anchor = local_at(QPointF(std::floor(pin.source.x()) + .5,
                                            std::floor(pin.source.y()) + .5));
    QPointF label;
    if (pin.sample_size == 1) {
      // The needle ends exactly at the source pixel centre. A cap/stem rather
      // than a map-marker silhouette keeps it identifiable at any source zoom.
      QPainterPath path;
      path.moveTo(anchor);
      path.lineTo(anchor + QPointF(0, -7));
      path.moveTo(anchor + QPointF(-6, -7));
      path.lineTo(anchor + QPointF(6, -7));
      path.lineTo(anchor + QPointF(3, -11));
      path.lineTo(anchor + QPointF(3, -17));
      path.lineTo(anchor + QPointF(5, -19));
      path.lineTo(anchor + QPointF(-5, -19));
      path.lineTo(anchor + QPointF(-3, -17));
      path.lineTo(anchor + QPointF(-3, -11));
      path.closeSubpath();
      p.setPen(QPen(QColor("#17202a"), 4));
      p.drawPath(path);
      p.setPen(QPen(QColor("#f4f8fc"), 1.5));
      p.drawPath(path);
      label = anchor + QPointF(9, -19);
    } else {
      const QRectF b = sample_bounds(pin.source, pin.sample_size);
      const QRectF r(local_at(b.topLeft()), local_at(b.bottomRight()));
      p.setPen(QPen(QColor("#17202a"), 3));
      p.drawRect(r);
      p.setPen(QPen(QColor("#f4f8fc"), 1));
      p.drawRect(r);
      label = r.topLeft() + QPointF(0, -18);
    }
    const QString text = "#" + QString::number(pin.id);
    const QRectF label_rect(
        label, QSizeF(p.fontMetrics().horizontalAdvance(text) + 6, 17));
    p.fillRect(label_rect, QColor(23, 32, 42, 230));
    p.setPen(QColor("#f4f8fc"));
    p.drawText(label_rect, Qt::AlignCenter, text);
  }
}
QImage AnalyzerSourceView::retained_overlay(double pixel_ratio) {
  const bool previous = report_mode_;
  report_mode_ = true;
  QImage image(QSize(int(std::lround(width() * pixel_ratio)),
                     int(std::lround(height() * pixel_ratio))),
               QImage::Format_RGBA8888);
  image.fill(Qt::transparent);
  image.setDevicePixelRatio(pixel_ratio);
  QPainter p(&image);
  draw_overlay(p);
  p.end();
  report_mode_ = previous;
  return image;
}
QImage AnalyzerSourceView::presentation_overlay(double pixel_ratio) {
  const QSize size(int(std::lround(width() * pixel_ratio)),
                   int(std::lround(height() * pixel_ratio)));
  if (overlay_dirty_ || presentation_overlay_.size() != size ||
      presentation_overlay_.devicePixelRatio() != pixel_ratio) {
    presentation_overlay_ = QImage(size, QImage::Format_RGBA8888);
    presentation_overlay_.fill(Qt::transparent);
    presentation_overlay_.setDevicePixelRatio(pixel_ratio);
    QPainter p(&presentation_overlay_);
    draw_overlay(p);
    overlay_dirty_ = false;
  }
  return presentation_overlay_;
}
void AnalyzerSourceView::invalidate_overlay() {
  overlay_dirty_ = true;
  overlay_->update();
  if (overlay_changed)
    overlay_changed();
}
void AnalyzerSourceView::update_hover(QPointF local) {
  const QPointF source = source_at(local);
  hover_ = valid(source) ? std::optional<QPointF>(source) : std::nullopt;
  if (hover_changed)
    hover_changed((pressed_ && mask_armed_) || multi_gesture_ ? std::nullopt
                                                              : hover_);
  // Ordinary pointer hover changes readouts/scopes only. Do not regenerate and
  // upload a viewport-sized marks image when there is no picker box to draw.
  if (picker_)
    invalidate_overlay();
}
void AnalyzerSourceView::mousePressEvent(QMouseEvent *e) {
  if (e->button() != Qt::LeftButton)
    return;
  // A fresh physical press is an explicit new sequence, including when an OS
  // native gesture was interrupted without delivering End. Do not roll back
  // the completed pan/zoom, or leave tools permanently latched off.
  if (multi_gesture_ && e->source() != Qt::MouseEventNotSynthesized) {
    e->accept();
    return;
  }
  if (multi_gesture_)
    finish_multi_gesture(e->position(), false);
  pressed_ = true;
  moved_ = false;
  press_point_ = last_point_ = e->position();
  press_source_ = source_at(press_point_);
  old_mask_ = mask_;
  old_transform_ = transform_;
  setFocus();
  grabMouse();
  if (mask_armed_ && valid(source_at(e->position()))) {
    const auto source = bounded(source_at(e->position()));
    mask_ = AnalyzerSourceMask{QRectF(source, source), ellipse_};
  }
  if (gesture_changed)
    gesture_changed(true);
  update_hover(e->position());
  e->accept();
}
void AnalyzerSourceView::mouseMoveEvent(QMouseEvent *e) {
  if (pressed_ && !e->buttons().testFlag(Qt::LeftButton))
    finish_gesture(last_point_, true);
  if (pressed_ && !multi_gesture_) {
    moved_ = moved_ || QLineF(press_point_, e->position()).length() > 4;
    if (mask_armed_ && valid(press_source_) && mask_) {
      mask_->bounds = QRectF(bounded(press_source_),
                             bounded(source_at(e->position())))
                          .normalized();
      if (moved_ && mask_changed)
        mask_changed(mask_);
    } else if (!picker_ && !mask_armed_) {
      if (transform_.fit) {
        transform_.scale = effective_scale();
        transform_.fit = false;
      }
      transform_.pan += e->position() - last_point_;
      notify_view();
    }
    last_point_ = e->position();
  }
  update_hover(e->position());
  e->accept();
}
void AnalyzerSourceView::finish_gesture(QPointF local, bool cancel) {
  if (!pressed_)
    return;
  const bool pin = !cancel && !moved_ && picker_ && !multi_gesture_;
  if (cancel) {
    transform_ = old_transform_;
    mask_ = old_mask_;
  } else if (mask_armed_ && !moved_)
    mask_ = old_mask_;
  pressed_ = false;
  releaseMouse();
  if (mask_changed && (mask_armed_ || cancel))
    mask_changed(mask_);
  if (pin && valid(source_at(local)) && pin_requested)
    pin_requested(bounded(source_at(local)), sample_size_);
  if (gesture_changed)
    gesture_changed(false);
  update_hover(local);
  notify_view();
}
void AnalyzerSourceView::mouseReleaseEvent(QMouseEvent *e) {
  if (e->button() == Qt::LeftButton)
    finish_gesture(e->position(), false);
  e->accept();
}
bool AnalyzerSourceView::cancel_gesture() {
  if (!pressed_ && !multi_gesture_)
    return false;
  if (multi_gesture_)
    finish_multi_gesture(last_point_, true);
  else
    finish_gesture(last_point_, true);
  return true;
}
void AnalyzerSourceView::begin_multi_gesture(QPointF local) {
  cancel_gesture();
  old_transform_ = transform_;
  multi_gesture_ = true;
  touch_distance_ = 0.;
  last_point_ = local;
  hover_.reset();
  if (hover_changed)
    hover_changed({});
  if (gesture_changed)
    gesture_changed(true);
  invalidate_overlay();
}
void AnalyzerSourceView::finish_multi_gesture(QPointF local, bool cancel) {
  if (!multi_gesture_)
    return;
  if (cancel)
    transform_ = old_transform_;
  multi_gesture_ = false;
  touch_distance_ = 0.;
  if (gesture_changed)
    gesture_changed(false);
  update_hover(local);
  notify_view();
}
void AnalyzerSourceView::leaveEvent(QEvent *) {
  if (!pressed_) {
    hover_.reset();
    if (hover_changed)
      hover_changed({});
    if (picker_)
      invalidate_overlay();
  }
}
void AnalyzerSourceView::wheelEvent(QWheelEvent *e) {
  if (pressed_)
    finish_gesture(last_point_, true);
  if (e->phase() == Qt::ScrollBegin && !multi_gesture_)
    begin_multi_gesture(e->position());
  const bool moving = !e->pixelDelta().isNull() || !e->angleDelta().isNull();
  if (moving && (e->phase() != Qt::NoScrollPhase || !e->pixelDelta().isNull()) &&
      !e->modifiers().testFlag(Qt::ControlModifier)) {
    if (transform_.fit) {
      transform_.scale = effective_scale();
      transform_.fit = false;
    }
    transform_.pan += e->pixelDelta().isNull() ? QPointF(e->angleDelta()) / 8.
                                             : QPointF(e->pixelDelta());
    notify_view();
  } else if (moving) {
    const double delta = e->pixelDelta().isNull() ? e->angleDelta().y()
                                                  : e->pixelDelta().y() * 8.;
    zoom_at(e->position(), std::exp(delta * .0018));
  }
  last_point_ = e->position();
  if (e->phase() == Qt::ScrollEnd)
    finish_multi_gesture(e->position(), false);
  else
    update_hover(e->position());
  e->accept();
}
bool AnalyzerSourceView::eventFilter(QObject *object, QEvent *event) {
  if (object == surface_) {
    // The surface fills rect(), so native-child local coordinates are exactly
    // the Source coordinates. Consume here to avoid duplicate parent bubbling.
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseMove:
    case QEvent::Wheel:
    case QEvent::NativeGesture:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
    case QEvent::Leave:
      QCoreApplication::sendEvent(this, event);
      return true;
    default:
      break;
    }
  }
  return QWidget::eventFilter(object, event);
}
bool AnalyzerSourceView::event(QEvent *event) {
  if (event->type() == QEvent::UngrabMouse && pressed_)
    cancel_gesture();
  if (event->type() == QEvent::NativeGesture) {
    auto *e = static_cast<QNativeGestureEvent *>(event);
    if (e->gestureType() == Qt::BeginNativeGesture)
      begin_multi_gesture(e->position());
    if (e->gestureType() == Qt::ZoomNativeGesture) {
      if (!multi_gesture_)
        begin_multi_gesture(e->position());
      zoom_at(e->position(), 1. + e->value());
    }
    if (e->gestureType() == Qt::PanNativeGesture) {
      if (!multi_gesture_)
        begin_multi_gesture(e->position());
      if (transform_.fit) {
        transform_.scale = effective_scale();
        transform_.fit = false;
      }
      transform_.pan += e->delta();
      notify_view();
    }
    last_point_ = e->position();
    if (e->gestureType() == Qt::EndNativeGesture)
      finish_multi_gesture(e->position(), false);
    e->accept();
    return true;
  }
  if (event->type() == QEvent::TouchBegin ||
      event->type() == QEvent::TouchUpdate ||
      event->type() == QEvent::TouchEnd ||
      event->type() == QEvent::TouchCancel) {
    auto *e = static_cast<QTouchEvent *>(event);
    const auto &points = e->points();
    if (event->type() == QEvent::TouchCancel ||
        event->type() == QEvent::TouchEnd) {
      finish_multi_gesture(touch_center_, event->type() == QEvent::TouchCancel);
      e->accept();
      return true;
    }
    if (points.size() >= 2) {
      const QPointF center = (points[0].position() + points[1].position()) * .5;
      const double distance =
          QLineF(points[0].position(), points[1].position()).length();
      if (!multi_gesture_) {
        begin_multi_gesture(center);
      } else if (touch_distance_ > 0.) {
        zoom_at(touch_center_, distance / touch_distance_);
        transform_.pan += center - touch_center_;
        notify_view();
      }
      touch_center_ = center;
      last_point_ = center;
      touch_distance_ = std::max(1., distance);
      hover_.reset();
      if (hover_changed)
        hover_changed({});
    } else if (multi_gesture_)
      finish_multi_gesture(touch_center_, false);
    e->accept();
    return true;
  }
  if (event->type() == QEvent::WindowDeactivate)
    cancel_gesture();
  return QWidget::event(event);
}
} // namespace hdrshot
