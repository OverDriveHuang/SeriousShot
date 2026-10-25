#include "platform/windows/windows_qt_pinned_screenshot.hpp"
#include "platform/windows/windows_analysis_backend.hpp"
#include "platform/windows/windows_color.hpp"
#include "platform/windows/windows_d3d.hpp"
#include "platform/windows/windows_source_display_binding.hpp"
#include "platform/windows/windows_pinned_decoration.hpp"
#include "ui/qt/analyzer_source_view.hpp"
#include <QApplication>
#include <QPointer>
#include <QWindow>

namespace hdrshot {
namespace {
Error native_error(const char *operation, DWORD code,
    std::source_location origin = std::source_location::current()) {
  return {ErrorCode::presenter_failed,"WindowsPinnedWindow",
          Retryability::after_recreate,
          {{"reason",operation},{"api",operation},{"nativeCode",std::to_string(code)}},origin};
}

class WindowsPinnedWindow final : public QtPinnedWindowPort {
public:
  explicit WindowsPinnedWindow(std::shared_ptr<DiagnosticsPort> diagnostics)
      : diagnostics_(std::move(diagnostics)) {}
  void set_always_on_top(QWidget &widget, bool topmost) override {
    configure_windows_pinned_decoration(widget,diagnostics_);
    order(widget, topmost ? HWND_TOPMOST : HWND_NOTOPMOST);
  }
  void configure_preview(QWidget &widget,QWidget &owner) override {
    configure_windows_pinned_decoration(widget,diagnostics_,true);
    const auto hwnd=reinterpret_cast<HWND>(widget.winId());
    SetLastError(ERROR_SUCCESS);
    if(!SetWindowLongPtrW(hwnd,GWLP_HWNDPARENT,static_cast<LONG_PTR>(owner.winId()))) {
      const auto code=GetLastError();
      if(code!=ERROR_SUCCESS)record_diagnostic_failure(diagnostics_.get(),"pin.preview.owner",native_error("SetWindowLongPtrW",code));
    }
    // Qt WindowDoesNotAcceptFocus / WindowTransparentForInput provide the
    // passive flags; native ordering follows the owning pin's topmost band.
    const auto flags=GetWindowLongPtrW(reinterpret_cast<HWND>(owner.winId()),GWL_EXSTYLE);
    order(widget,(flags & WS_EX_TOPMOST)?HWND_TOPMOST:HWND_NOTOPMOST);
  }
  void bring_forward(QWidget &widget) override {
    // HWND_TOP preserves membership of the topmost/non-topmost band.
    order(widget, HWND_TOP);
  }
private:
  void order(QWidget &widget, HWND after) {
    if (!SetWindowPos(reinterpret_cast<HWND>(widget.winId()),after,0,0,0,0,
        SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER)) {
      const auto error=native_error("SetWindowPos",GetLastError());
      record_diagnostic_failure(diagnostics_.get(),"pin.window_order",error);
      if (auto *pin=dynamic_cast<PinnedScreenshotWindow *>(&widget))
        pin->show_error(QStringLiteral("无法调整贴图窗口层级，请重试。"));
    }
  }
  std::shared_ptr<DiagnosticsPort> diagnostics_;
};

Result<bool,Error> attach(PinnedScreenshotWindow &window,
    std::shared_ptr<AnalysisPresenterPort> presenter,
    std::shared_ptr<DiagnosticsPort> diagnostics,
    std::shared_ptr<WindowsDisplayStateService> displays) {
  const auto hwnd=reinterpret_cast<HWND>(window.source_view()->presentation_surface()->winId());
  if (!IsWindow(hwnd))
    return Result<bool,Error>::failure({ErrorCode::presenter_failed,
        "WindowsPinnedWindow",Retryability::after_recreate,{{"reason","missing_source_hwnd"}}});
  // DirectComposition content is above this child's backing store. Do not try
  // to retrofit WS_EX_NOREDIRECTIONBITMAP onto an existing Qt HWND: Windows
  // rejects that mutation with ERROR_INVALID_PARAMETER. Toolbar HWND siblings
  // remain above the source child, without a second input-routing surface.
  // Standalone hosts/tests may omit the application service. Creation belongs
  // to this UI attachment, never the background image preparation thread.
  if (!displays) displays = std::make_shared<WindowsDisplayStateService>(nullptr,diagnostics);
  const QPointer<PinnedScreenshotWindow> guard(&window);
  return bind_windows_source_display(window,*window.source_view()->presentation_surface(),
      std::move(presenter),std::move(displays),
      [guard] { if (guard && !guard->is_closed()) guard->refresh_presentation(); },
      [guard,diagnostics](Error error) {
        record_diagnostic_failure(diagnostics.get(),"pin.display_state",error);
        if (guard && !guard->is_closed()) guard->show_display_error(QStringLiteral("无法更新显示器颜色状态。"));
      }, [guard] { if (guard) guard->clear_display_error(); });
}
} // namespace

std::shared_ptr<QtPinnedWindowPort> make_windows_pinned_window_port(
    std::shared_ptr<DiagnosticsPort> diagnostics) {
  return std::make_shared<WindowsPinnedWindow>(std::move(diagnostics));
}
Result<PreparedPinnedScreenshot,Error> prepare_windows_pinned_screenshot(
    const ExportSnapshot &snapshot,std::shared_ptr<DiagnosticsPort> diagnostics,
    std::shared_ptr<WindowsDisplayStateService> displays) {
  auto backend=make_windows_analysis_backend(diagnostics);
  if (!backend) return Result<PreparedPinnedScreenshot,Error>::failure(backend.error());
  WindowsLinearP3RangeProbe range;
  auto prepared=AnalysisWorkflow::prepare(snapshot,*backend.value().port,&range);
  if (!prepared) return Result<PreparedPinnedScreenshot,Error>::failure(prepared.error());
  auto captured=snapshot.target_display_id.value==0
      ? FrameCropper::view(*snapshot.frozen_desktop,snapshot.selection)
      : FrameCropper::view_display(*snapshot.frozen_desktop,snapshot.target_display_id,snapshot.selection);
  if (!captured) return Result<PreparedPinnedScreenshot,Error>::failure(captured.error());
  auto original=windows_snapshot_with_capture_range(prepared.value().original,
                                                   captured.value().display_dynamic_range);
  if (!original) return Result<PreparedPinnedScreenshot,Error>::failure(original.error());
  prepared.value().original=std::move(original.value());
  const auto shared_displays=std::make_shared<std::shared_ptr<WindowsDisplayStateService>>(displays);
  PreparedPinnedScreenshot result;
  for(auto &aux:result.auxiliary) {
    auto extra=make_windows_analysis_presenter(diagnostics);
    if(!extra)return Result<PreparedPinnedScreenshot,Error>::failure(extra.error());
    aux.presenter=std::move(extra.value());
    const auto peer=aux.presenter;
    aux.observe_errors=[peer](std::function<void(Error)> callback) {
      const QPointer<QObject> dispatch(qApp);
      set_windows_analysis_presenter_error_callback(*peer,[dispatch,callback=std::move(callback)](Error error) {
        if(dispatch)QMetaObject::invokeMethod(dispatch,[callback,error=std::move(error)]{callback(error);},Qt::QueuedConnection);
      });
    };
    aux.attach=[peer,shared_displays,diagnostics](QWidget &owner,QWidget &surface,
        std::function<void()> refresh,std::function<void(Error)> failure) {
      if(!*shared_displays)*shared_displays=std::make_shared<WindowsDisplayStateService>(nullptr,diagnostics);
      auto service=*shared_displays;
      return bind_windows_source_display(owner,surface,peer,std::move(service),std::move(refresh),std::move(failure));
    };
  }
  result.image=std::move(prepared.value());
  result.presenter=std::move(backend.value().presenter);
  const auto presenter=result.presenter;
  result.observe_errors=[presenter](std::function<void(Error)> callback) {
    const QPointer<QObject> dispatch(qApp);
    set_windows_analysis_presenter_error_callback(*presenter,
        [dispatch,callback=std::move(callback)](Error error) {
          if (dispatch) QMetaObject::invokeMethod(dispatch,
              [callback,error=std::move(error)] { callback(error); },Qt::QueuedConnection);
        });
  };
  result.attach_window=[presenter,diagnostics,shared_displays](PinnedScreenshotWindow &window) {
    if(!*shared_displays)*shared_displays=std::make_shared<WindowsDisplayStateService>(nullptr,diagnostics);
    return attach(window,presenter,diagnostics,*shared_displays);
  };
  return Result<PreparedPinnedScreenshot,Error>::success(std::move(result));
}
} // namespace hdrshot
