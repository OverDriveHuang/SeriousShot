#include "ui/qt/source_presentation_controller.hpp"
#include "ui/qt/analyzer_source_view.hpp"
#include <QApplication>
#include <QPlatformSurfaceEvent>
#include <QScreen>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace hdrshot {
namespace {
analysis::UiImage overlay_image(const QImage &image) {
  const auto rgba = image.convertToFormat(QImage::Format_RGBA8888);
  analysis::UiImage result;
  result.size = {rgba.width(), rgba.height()};
  const auto row_bytes = std::size_t(rgba.width()) * 4;
  result.rgba.resize(row_bytes * std::size_t(rgba.height()));
  for (int y = 0; y < rgba.height(); ++y)
    std::memcpy(result.rgba.data() + std::size_t(y) * row_bytes,
                rgba.constScanLine(y), row_bytes);
  return result;
}
}
SourcePresentationController::SourcePresentationController(
    QWidget &owner, AnalyzerSourceView &source, OptionsProvider options, Submit submit,
    Ready ready)
    : QObject(&owner), owner_(&owner), source_(&source),
      surface_(source.presentation_widget()), options_(std::move(options)),
      submit_(std::move(submit)), ready_(std::move(ready)) {
  timer_.setSingleShot(true);
  timer_.setTimerType(Qt::PreciseTimer);
  connect(&timer_, &QTimer::timeout, this, [this] { dispatch(); });
  // QWindow instances can be created/replaced after this controller. Compare
  // the current handles on delivery instead of retaining an obsolete handle.
  qApp->installEventFilter(this);
}
SourcePresentationController::~SourcePresentationController() { stop(); }
void SourcePresentationController::stop() {
  stopped_ = true;
  dirty_ = false;
  timer_.stop();
  if (qApp)
    qApp->removeEventFilter(this);
  options_ = {};
  submit_ = {};
  ready_ = {};
  operation_overlay_.reset();
}
bool SourcePresentationController::visible() const {
  return owner_ && source_ && surface_ && owner_->isVisible() &&
         source_->isVisible() && surface_->isVisible() &&
         !native_surface_unavailable_ && !owner_surface_unavailable_;
}
void SourcePresentationController::request_present() {
  if (stopped_)
    return;
  dirty_ = true;
  if (timer_.isActive() || !visible())
    return;
  const auto *screen = source_->screen();
  const double hz = screen ? screen->refreshRate() : 60.;
  timer_.start(int(std::ceil(1000. / std::clamp(hz, 30., 120.))));
}
analysis::SourceView SourcePresentationController::view() const {
  analysis::SourceView result;
  if (!source_ || !options_)
    return result;
  const auto options = options_();
  const double dpr = source_->devicePixelRatioF();
  const auto rect = source_->image_rect();
  result.target_size = {int(std::lround(source_->width() * dpr)),
                        int(std::lround(source_->height() * dpr))};
  result.scale = source_->effective_scale() * dpr;
  result.offset_x = rect.x() * dpr;
  result.offset_y = rect.y() * dpr;
  result.settings = options.settings;
  result.false_color = options.false_color;
  result.mask = options.mask;
  result.mask_outside_factor = options.mask_outside_factor;
  return result;
}
void SourcePresentationController::dispatch() {
  if (stopped_ || !dirty_ || !visible() || !submit_ || (ready_ && !ready_()))
    return;
  // Make the native surface before sampling the viewport. Its creation may
  // synchronously deliver resize/DPR events; this frame uses their final state.
  auto *surface = source_->presentation_surface();
  if (!visible())
    return;
  auto frame = view();
  if (options_().include_operation_overlay) {
    const auto marks = source_->presentation_overlay(source_->devicePixelRatioF());
    if (!operation_overlay_ || operation_overlay_key_ != marks.cacheKey()) {
      operation_overlay_ = std::make_shared<analysis::UiImage>(overlay_image(marks));
      operation_overlay_key_ = marks.cacheKey();
    }
    frame.operation_overlay = operation_overlay_;
  }
  // Creation events above have already been folded into this frame. A request
  // made by the consumer below must remain pending for the subsequent frame.
  timer_.stop();
  dirty_ = false;
  const auto submit = submit_;
  const QPointer<SourcePresentationController> alive(this);
  const bool submitted = submit(std::move(frame), std::uintptr_t(surface->winId()));
  if (alive && !stopped_ && !submitted)
    dirty_ = true;
}
bool SourcePresentationController::eventFilter(QObject *object, QEvent *event) {
  if (stopped_ || !owner_ || !source_ || !surface_)
    return false;
  const bool native_surface = object == surface_ || object == surface_->windowHandle();
  const bool owner_surface = object == owner_ || object == owner_->windowHandle();
  if (!native_surface && !owner_surface && object != source_)
    return false;
  if (event->type() == QEvent::PlatformSurface) {
    const bool destroyed = static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() ==
                           QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed;
    if (native_surface)
      native_surface_unavailable_ = destroyed;
    if (owner_surface)
      owner_surface_unavailable_ = destroyed;
    if (destroyed) {
      dirty_ = true;
      timer_.stop();
      return false;
    }
    request_present();
  } else if (event->type() == QEvent::WinIdChange) {
    if (native_surface && surface_->internalWinId())
      native_surface_unavailable_ = false;
    if (owner_surface && owner_->internalWinId())
      owner_surface_unavailable_ = false;
    request_present();
  } else {
    switch (event->type()) {
    case QEvent::Hide:
      dirty_ = true;
      timer_.stop();
      break;
    case QEvent::Show:
    case QEvent::ShowToParent:
    case QEvent::Expose:
    case QEvent::Resize:
    case QEvent::DevicePixelRatioChange:
    case QEvent::ScreenChangeInternal:
      request_present();
      break;
    default:
      break;
    }
  }
  return false;
}
} // namespace hdrshot
