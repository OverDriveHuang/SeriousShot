#include "platform/windows/windows_source_display_binding.hpp"
#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QTimer>
#include <QWindow>
#include <windows.h>

namespace hdrshot {
namespace {
class Binding final : public QObject, public QAbstractNativeEventFilter {
 public:
  Binding(QWidget &owner, QWidget &surface,
      std::shared_ptr<AnalysisPresenterPort> presenter,
      std::shared_ptr<WindowsDisplayStateService> displays,
      std::function<void()> redraw, std::function<void(Error)> error,
      std::function<void()> recovered)
      : QObject(&owner), owner_(&owner), surface_(&surface),
        presenter_(std::move(presenter)), displays_(std::move(displays)),
        redraw_(std::move(redraw)), error_(std::move(error)), recovered_(std::move(recovered)) {
    setObjectName("windowsSourceDisplayBinding");
    target_timer_.setSingleShot(true);
    connect(&target_timer_, &QTimer::timeout, this, [this] { bind_target(); });
    owner.installEventFilter(this);
    surface.installEventFilter(this);
    qApp->installNativeEventFilter(this);
    bind_target();
  }
  ~Binding() override {
    target_timer_.stop();
    if (qApp) qApp->removeNativeEventFilter(this);
    if (token_) displays_->unsubscribe(token_);
    set_windows_analysis_display_target(*presenter_, 0, 0, ++binding_, {});
  }
  bool nativeEventFilter(const QByteArray &, void *message, qintptr *) override {
    const auto *msg = static_cast<MSG *>(message);
    // The application service alone handles the topology notification. Each
    // binding only reassesses where its content surface now resides.
    if (msg->message == WM_DISPLAYCHANGE) schedule_target();
    return false;
  }
 protected:
  bool eventFilter(QObject *object, QEvent *event) override {
    if (event->type() == QEvent::PlatformSurface &&
        object == surface_window_ &&
        static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() ==
            QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
      target_timer_.stop();
      if (token_) { displays_->unsubscribe(token_); token_ = 0; }
      hwnd_ = 0; monitor_ = 0;
      set_windows_analysis_display_target(*presenter_, 0, 0, ++binding_, {});
      return false;
    }
    switch (event->type()) {
      case QEvent::Move:
      case QEvent::Resize:
      case QEvent::Show:
      case QEvent::WinIdChange:
      case QEvent::ScreenChangeInternal:
      case QEvent::DevicePixelRatioChange:
      case QEvent::PlatformSurface: schedule_target(); break;
      default: break;
    }
    // Close can be vetoed by an analyzer. QObject destruction owns revocation.
    return false;
  }
 private:
  void schedule_target() {
    if (!target_timer_.isActive()) target_timer_.start(0);
  }
  void bind_target() {
    if (!owner_ || !surface_) return;
    if (surface_window_ != surface_->windowHandle()) {
      if (surface_window_) surface_window_->removeEventFilter(this);
      surface_window_ = surface_->windowHandle();
      if (surface_window_) surface_window_->installEventFilter(this);
    }
    const auto hwnd = std::uintptr_t(surface_->internalWinId());
    if (!hwnd || !IsWindow(reinterpret_cast<HWND>(hwnd))) return;
    const auto monitor = reinterpret_cast<std::uintptr_t>(
        MonitorFromWindow(reinterpret_cast<HWND>(hwnd), MONITOR_DEFAULTTONEAREST));
    if (hwnd == hwnd_ && monitor == monitor_ && token_) return;
    hwnd_ = hwnd; monitor_ = monitor;
    set_windows_analysis_display_target(*presenter_, hwnd_, monitor_, ++binding_, {});
    // Keep the old subscription until the new one receives the cached monitor
    // state; recreating a surface must not discard the last shared cache entry.
    const auto previous_token = token_;
    const auto expected = binding_;
    const QPointer<Binding> guard(this);
    token_ = displays_->subscribe(monitor_, [guard, expected](auto result) {
      if (!guard || expected != guard->binding_) return;
      if (!result) {
        // Keep the last successful swapchain image, but suspend new frames
        // until a later notification recovers a valid target state.
        set_windows_analysis_display_target(*guard->presenter_, guard->hwnd_,
            guard->monitor_, expected, {});
        if (guard->error_) guard->error_(result.error());
        return;
      }
      if (!result.value() || result.value()->display.monitor != guard->monitor_) return;
      set_windows_analysis_display_target(*guard->presenter_, guard->hwnd_,
          guard->monitor_, expected, std::move(result.value()));
      if (guard->recovered_) guard->recovered_();
      if (guard->redraw_) guard->redraw_();
    });
    if (previous_token) displays_->unsubscribe(previous_token);
  }
  QPointer<QWidget> owner_, surface_;
  QPointer<QWindow> surface_window_;
  std::shared_ptr<AnalysisPresenterPort> presenter_;
  std::shared_ptr<WindowsDisplayStateService> displays_;
  std::function<void()> redraw_;
  std::function<void(Error)> error_;
  std::function<void()> recovered_;
  QTimer target_timer_;
  WindowsDisplayStateService::Token token_{};
  std::uintptr_t hwnd_{}, monitor_{};
  std::uint64_t binding_{};
};
} // namespace

Result<bool, Error> bind_windows_source_display(QWidget &owner, QWidget &surface,
    std::shared_ptr<AnalysisPresenterPort> presenter,
    std::shared_ptr<WindowsDisplayStateService> displays,
    std::function<void()> request_present, std::function<void(Error)> report_error,
    std::function<void()> clear_display_error) {
  if (!presenter || !displays || !IsWindow(reinterpret_cast<HWND>(surface.winId())))
    return Result<bool, Error>::failure({ErrorCode::presenter_failed,
        "WindowsSourceDisplayBinding", Retryability::after_recreate,
        {{"reason", "invalid_surface_or_service"}}});
  new Binding(owner, surface, std::move(presenter), std::move(displays),
      std::move(request_present), std::move(report_error), std::move(clear_display_error));
  return Result<bool, Error>::success(true);
}
} // namespace hdrshot
