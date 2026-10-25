#include "platform/qt/qt_pinned_screenshot_controller.hpp"
#include <QGuiApplication>
#include "ui/qt/pinned_screenshot_preview.hpp"
#include "ui/qt/analyzer_source_view.hpp"
#include <QPointer>
#include <QScreen>
#include <exception>
namespace hdrshot {
namespace {
Error pin_error(const char *reason) {
  return {ErrorCode::invalid_input, "PinnedScreenshot",
          Retryability::after_user_action, {{"reason",reason}}};
}
QString readable(const Error &e) {
  return QStringLiteral("%1：%2").arg(QString::fromStdString(e.module),
                                     QString::fromStdString(to_string(e.code)));
}
}
struct QtPinnedScreenshotController::Window {
  PreparedPinnedScreenshot prepared;
  QPointer<PinnedScreenshotWindow> widget;
  bool error_reported{};
};
QtPinnedScreenshotController::QtPinnedScreenshotController(QObject &dispatch,
    Prepare prepare, std::shared_ptr<QtPinnedWindowPort> native_window,
    Export export_request, Analyze analyze, ReadSettings settings,
    std::shared_ptr<DiagnosticsPort> diagnostics)
    : dispatch_(dispatch), executor_(dispatch), prepare_(std::move(prepare)),
      native_(std::move(native_window)), export_(std::move(export_request)),
      analyze_(std::move(analyze)), settings_(std::move(settings)),
      diagnostics_(std::move(diagnostics)) {}
QtPinnedScreenshotController::~QtPinnedScreenshotController() { shutdown(); }
void QtPinnedScreenshotController::open(ExportSnapshot snapshot, Completed completed) {
  if (shutting_down_) {
    completed(Result<bool,Error>::failure(pin_error("shutting_down"))); return;
  }
  const auto defaults = settings_().pinned_screenshot_defaults;
  auto *screen = QGuiApplication::primaryScreen();
  // Display IDs are platform-owned; use the capture display's logical center
  // rather than interpreting a native display identifier in shared code.
  if (snapshot.frozen_desktop) {
    for (const auto &segment : snapshot.frozen_desktop->canonical_segments) {
      const auto &display = segment;
      if (display.display_id == snapshot.target_display_id) {
        const auto &r=display.desktop_frame_points;
        if (auto *candidate=QGuiApplication::screenAt(QPoint(int(r.x+r.width/2), int(r.y+r.height/2))))
          screen=candidate;
      }
    }
  }
  const QRect available = screen ? screen->availableGeometry() : QRect(0,0,1024,768);
  const QPointer<QtPinnedScreenshotController> guard(this);
  auto prepare=prepare_; auto diagnostics=diagnostics_;
  auto *dispatch=&dispatch_;
  ++pending_;
  record_diagnostic_stage(diagnostics.get(),snapshot.session_id,snapshot.operation_id,
                          "pin","prepare","begin");
  executor_.background([guard,dispatch,prepare,diagnostics,defaults,available,
      snapshot=std::move(snapshot),completed=std::move(completed)]() mutable {
    auto result=[&] {
      try { return prepare(snapshot); }
      catch(const std::exception &) { return Result<PreparedPinnedScreenshot,Error>::failure(pin_error("prepare_exception")); }
    }();
    if (!result) record_diagnostic_failure(diagnostics.get(),"pin.prepare",result.error());
    else record_diagnostic_stage(diagnostics.get(),snapshot.session_id,snapshot.operation_id,"pin","prepare","success");
    snapshot={};
    QMetaObject::invokeMethod(dispatch,[guard,defaults,available,result=std::move(result),
        completed=std::move(completed)]() mutable {
      if (guard) --guard->pending_;
      if (!guard || guard->shutting_down_) {
        completed(Result<bool,Error>::failure(pin_error("shutting_down"))); return;
      }
      if (!result) { completed(Result<bool,Error>::failure(result.error())); return; }
      auto runtime=std::make_shared<Window>();
      runtime->prepared=std::move(result.value());
      auto installed=guard->install(runtime,defaults,available);
      if (!installed) {
        record_diagnostic_failure(guard->diagnostics_.get(),"pin.install",installed.error());
        if (runtime->widget) delete runtime->widget.data();
        completed(Result<bool,Error>::failure(installed.error())); return;
      }
      completed(Result<bool,Error>::success(true));
      if (guard && runtime->widget && !guard->shutting_down_) {
        runtime->widget->show();
        guard->native_->set_always_on_top(*runtime->widget,defaults.always_on_top);
        guard->native_->bring_forward(*runtime->widget);
      }
    },Qt::QueuedConnection);
  });
}
Result<bool, Error> QtPinnedScreenshotController::install(std::shared_ptr<Window> runtime,
    PinnedScreenshotDefaults defaults, QRect available) {
  std::erase_if(windows_,[](auto &w){return w.expired();});
  windows_.push_back(runtime);
  auto *window=new PinnedScreenshotWindow(runtime->prepared.image.input,defaults);
  runtime->widget=window;
  window->setAttribute(Qt::WA_DeleteOnClose);
  const auto size=runtime->prepared.image.input.source->size_px();
  window->setGeometry(PinnedScreenshotWindow::initial_geometry(
      {int(size.width),int(size.height)},defaults.short_edge,available));
  const QPointer<QtPinnedScreenshotController> guard(this);
  window->topmost_changed=[guard,runtime](bool topmost) {
    if(guard && runtime->widget) guard->native_->set_always_on_top(*runtime->widget,topmost);
  };
  window->front_requested=[guard,runtime] {
    if(guard && runtime->widget) guard->native_->bring_forward(*runtime->widget);
  };
  window->present_requested=[guard,runtime](analysis::SourceView view,std::uintptr_t surface) {
    if(!guard || !runtime->widget || runtime->widget->is_closed()) return;
    auto result=runtime->prepared.presenter->present(runtime->prepared.image.input,view,surface);
    if(!result && result.error().safe_context.contains("reason") &&
       result.error().safe_context.at("reason")=="analysis_work_pending") return;
    if(!result && !runtime->error_reported) {
      runtime->error_reported=true;
      record_diagnostic_failure(guard->diagnostics_.get(),"pin.present",result.error());
      runtime->widget->show_display_error(readable(result.error()));
    } else if(result && !runtime->prepared.observe_recovered) {
      runtime->error_reported=false;
    }
  };
  const std::weak_ptr<Window> weak=runtime;
  if(runtime->prepared.observe_errors) runtime->prepared.observe_errors([guard,weak](Error error) {
    auto current=weak.lock();
    if(!guard || !current || !current->widget || current->widget->is_closed()) return;
    record_diagnostic_failure(guard->diagnostics_.get(),"pin.present.async",error);
    if(error.safe_context.contains("reason") &&
       error.safe_context.at("reason")=="drawable_retry_exhausted") return;
    current->error_reported=true;
    current->widget->show_display_error(readable(error));
  });
  if(runtime->prepared.observe_recovered) runtime->prepared.observe_recovered([guard,weak] {
    auto current=weak.lock();
    if(!guard || !current || !current->widget || current->widget->is_closed()) return;
    current->error_reported=false;
    current->widget->clear_display_error();
  });
  auto *preview=window->preview();
  preview->configure_window=[guard,weak](QWidget &card) {
    const auto current=weak.lock();
    if(guard && current && current->widget)
      guard->native_->configure_preview(card,*current->widget);
  };
  preview->present=[guard,weak](int index,analysis::SourceView view,std::uintptr_t surface) {
    const auto current=weak.lock();
    if(!guard || !current || !current->widget || current->widget->is_closed())return false;
    auto &target=current->prepared.auxiliary[std::size_t(index)];
    if(!target.presenter)return false;
    auto result=target.presenter->present(current->prepared.image.input,view,surface);
    if(!result) {
      record_diagnostic_failure(guard->diagnostics_.get(),"pin.preview.present",result.error());
      current->widget->preview()->dismiss();
    }
    return bool(result);
  };
  for(int index=0;index<2;++index) {
    auto &target=runtime->prepared.auxiliary[std::size_t(index)];
    auto failure=[guard,weak](Error error) {
      const auto current=weak.lock();
      if(!guard || !current || !current->widget || current->widget->is_closed())return;
      record_diagnostic_failure(guard->diagnostics_.get(),"pin.preview.native",error);
      current->widget->preview()->dismiss();
    };
    if(target.observe_errors)target.observe_errors(failure);
    if(target.attach) {
      auto result=target.attach(preview->host(index),*preview->source(index).presentation_surface(),
          [weak,index] {
            const auto current=weak.lock();
            if(current && current->widget && !current->widget->is_closed())current->widget->preview()->refresh(index);
          },failure);
      if(!result)return result;
    }
  }
  window->action_requested=[guard,runtime](UiCommand command) {
    if(guard && !guard->shutting_down_) guard->action(runtime,command);
  };
  QObject::connect(window,&QObject::destroyed,this,[runtime] {runtime->widget=nullptr;});
  if (runtime->prepared.attach_window)
    return runtime->prepared.attach_window(*window);
  return Result<bool,Error>::success(true);
}
void QtPinnedScreenshotController::action(std::shared_ptr<Window> runtime,UiCommand command) {
  if(!runtime->widget || runtime->widget->is_closed()) return;
  auto snapshot=runtime->prepared.image.original;
  const auto settings=settings_();
  snapshot.operation_id=OperationId{next_operation_++};
  snapshot.default_folder=settings.default_save_folder;
  snapshot.save_format=settings.save_format;
  snapshot.pq_diffuse_white=settings.pq_diffuse_white;
  snapshot.hdr_pq_precision=settings.hdr_pq_precision;
  snapshot.ultra_hdr_jpeg_quality=settings.ultra_hdr_jpeg_quality;
  runtime->widget->set_busy(true);
  if(command==UiCommand::analyze) {
    analyze_(std::move(snapshot),[runtime](Result<bool,Error> result) {
      if(!runtime->widget || runtime->widget->is_closed()) return;
      runtime->widget->set_busy(false);
      if(!result) runtime->widget->show_error(readable(result.error()));
    });
  } else {
    const bool submitted=export_(command,std::move(snapshot),[runtime](auto result) {
      if(!runtime->widget || runtime->widget->is_closed()) return;
      runtime->widget->set_busy(false);
      if(!result) runtime->widget->show_error(readable(result.error()));
    }, runtime->widget);
    if(!submitted && runtime->widget && !runtime->widget->is_closed()) runtime->widget->set_busy(false);
  }
}
bool QtPinnedScreenshotController::has_windows() const {
  if(pending_) return true;
  for(auto &weak:windows_)
    if(auto w=weak.lock(); w && w->widget && !w->widget->is_closed()) return true;
  return false;
}
void QtPinnedScreenshotController::shutdown() {
  if(shutting_down_) return;
  shutting_down_=true; executor_.drain();
  for(auto &weak:windows_) if(auto w=weak.lock(); w && w->widget) delete w->widget.data();
  windows_.clear();
}
} // namespace hdrshot
