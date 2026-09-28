#include "platform/windows/windows_qt_analyzer_controller.hpp"
#include "application/analysis_session.hpp"
#include "application/analysis_workflow.hpp"
#include "platform/windows/windows_analysis_backend.hpp"
#include "platform/windows/windows_color.hpp"
#include "ui/qt/analyzer_source_view.hpp"
#include "ui/qt/analyzer_window.hpp"
#include <QApplication>
#include <QPointer>
#include <chrono>
#include <exception>
#include <mutex>
#include <windows.h>

namespace hdrshot {
namespace {
Error adapter_error(const char *reason) {
  return {ErrorCode::invalid_input,
          "WindowsQtAnalyzerController",
          Retryability::after_user_action,
          {{"reason", reason}}};
}
QString readable_error(const Error &error) {
  return QStringLiteral("%1：%2").arg(
      QString::fromStdString(error.module),
      QString::fromStdString(to_string(error.code)));
}
} // namespace
struct WindowsQtAnalyzerController::Window {
  PreparedAnalysis prepared;
  DisplayDynamicRange captured_range{DisplayDynamicRange::sdr};
  WindowsAnalysisBackend backend;
  std::shared_ptr<AnalysisSession> session;
  QPointer<AnalyzerWindow> widget;
  bool presenter_error_reported{};
};
struct WindowsQtAnalyzerController::DispatchGate {
  explicit DispatchGate(QObject *target) : target(target) {}
  template <typename Callback> void post(Callback callback) {
    const std::scoped_lock lock(mutex);
    if (active)
      QMetaObject::invokeMethod(target, std::move(callback),
                                Qt::QueuedConnection);
  }
  void close() {
    const std::scoped_lock lock(mutex);
    active = false;
  }
  std::mutex mutex;
  QObject *target;
  bool active{true};
};

WindowsQtAnalyzerController::WindowsQtAnalyzerController(
    QObject &context, Export export_request, ReadSettings settings,
    SavePreferences preferences, std::shared_ptr<DiagnosticsPort> diagnostics)
    : dispatch_context_(context),
      dispatch_gate_(std::make_shared<DispatchGate>(&context)),
      prepare_executor_(context), export_request_(std::move(export_request)),
      read_settings_(std::move(settings)),
      save_preferences_(std::move(preferences)),
      diagnostics_(std::move(diagnostics)) {
  preference_timer_.setSingleShot(true);
  preference_timer_.setInterval(300);
  QObject::connect(&preference_timer_, &QTimer::timeout, this,
                   [this] { flush_preferences(); });
}
WindowsQtAnalyzerController::~WindowsQtAnalyzerController() { shutdown(); }

void WindowsQtAnalyzerController::open(
    ExportSnapshot snapshot, QtOverlayHost::AnalysisCompleted completed) {
  if (shutting_down_ || closing_windows_) {
    completed(Result<bool, Error>::failure(adapter_error("closing_windows")));
    return;
  }
  const QPointer<WindowsQtAnalyzerController> guard(this);
  auto gate = dispatch_gate_;
  auto diagnostics = diagnostics_;
  record_diagnostic_stage(diagnostics.get(), snapshot.session_id,
                          snapshot.operation_id, "analysis", "prepare",
                          "begin");
  prepare_executor_.background([guard, gate, diagnostics,
                                snapshot = std::move(snapshot),
                                completed = std::move(completed)]() mutable {
    const auto started = std::chrono::steady_clock::now();
    auto prepare = [&]() -> Result<std::shared_ptr<Window>, Error> {
      auto backend = make_windows_analysis_backend(diagnostics);
      if (!backend)
        return Result<std::shared_ptr<Window>, Error>::failure(backend.error());
      // Capture/export has its own processor and queue. Do not race that
      // mutable executor while establishing the analysis source class.
      WindowsLinearP3RangeProbe range;
      auto prepared =
          AnalysisWorkflow::prepare(snapshot, *backend.value().port, &range);
      if (!prepared)
        return Result<std::shared_ptr<Window>, Error>::failure(
            prepared.error());
      // Shared analysis compacts original/report snapshots with a generic HDR
      // marker. Keep Windows' capture-time mode separately for both exports.
      auto captured_view = snapshot.target_display_id.value == 0
          ? FrameCropper::view(*snapshot.frozen_desktop, snapshot.selection)
          : FrameCropper::view_display(*snapshot.frozen_desktop,
                                      snapshot.target_display_id,
                                      snapshot.selection);
      if (!captured_view)
        return Result<std::shared_ptr<Window>, Error>::failure(
            captured_view.error());
      auto runtime = std::make_shared<Window>();
      runtime->captured_range = captured_view.value().display_dynamic_range;
      runtime->prepared = std::move(prepared.value());
      runtime->backend = std::move(backend.value());
      runtime->session = std::make_shared<AnalysisSession>(
          runtime->prepared.input, runtime->backend.port);
      return Result<std::shared_ptr<Window>, Error>::success(
          std::move(runtime));
    };
    auto result = [&]() {
      try {
        return prepare();
      } catch (const std::exception &) {
        return Result<std::shared_ptr<Window>, Error>::failure(
            adapter_error("prepare_exception"));
      }
    }();
    if (!result)
      record_diagnostic_failure(diagnostics.get(), "analysis.prepare",
                                result.error());
    else
      record_diagnostic_stage(
          diagnostics.get(), snapshot.session_id, snapshot.operation_id,
          "analysis", "prepare", "success",
          {{"elapsedMs",
            std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started)
                    .count())}});
    // Only the independent ROI crosses to the UI; release the full desktop
    // and sparse capture resources before the new window is installed.
    snapshot = {};
    gate->post([guard, result = std::move(result),
                completed = std::move(completed)]() mutable {
      if (!guard || guard->shutting_down_ || guard->closing_windows_) {
        completed(Result<bool, Error>::failure(adapter_error("shutting_down")));
        return;
      }
      if (!result) {
        completed(Result<bool, Error>::failure(result.error()));
        return;
      }
      auto runtime = std::move(result.value());
      guard->install(runtime);
      // Hand off first, so the capture overlays cannot remain above the new
      // analyzer. Its immutable ROI is already independently owned.
      completed(Result<bool, Error>::success(true));
      if (runtime->widget) {
        runtime->widget->show();
        runtime->widget->raise();
        runtime->widget->activateWindow();
      }
    });
  });
}

void WindowsQtAnalyzerController::install(std::shared_ptr<Window> runtime) {
  std::erase_if(windows_, [](const auto &weak) { return weak.expired(); });
  windows_.push_back(runtime);
  auto *window = new AnalyzerWindow(runtime->prepared.input,
                                    read_settings_().analyzer_preferences_json);
  // The Qt native child is the DComp HWND. Suppress its ordinary redirection
  // bitmap so Qt backing-store pixels cannot cover the FP16 Source visual.
  const auto surface = reinterpret_cast<HWND>(
      window->source_widget()->presentation_surface()->winId());
  SetWindowLongPtrW(surface, GWL_EXSTYLE,
                    GetWindowLongPtrW(surface, GWL_EXSTYLE) |
                        WS_EX_NOREDIRECTIONBITMAP);
  SetWindowPos(surface, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                   SWP_FRAMECHANGED);
  runtime->widget = window;
  window->setAttribute(Qt::WA_DeleteOnClose);
  const QPointer<WindowsQtAnalyzerController> guard(this);
  auto gate = dispatch_gate_;
  auto diagnostics = diagnostics_;
  const std::weak_ptr<Window> weak_runtime = runtime;
  set_windows_analysis_presenter_error_callback(
      *runtime->backend.presenter, [guard, weak_runtime, gate](Error error) {
        gate->post([guard, weak_runtime, error] {
          const auto current = weak_runtime.lock();
          if (!guard || !current || !current->widget)
            return;
          record_diagnostic_failure(guard->diagnostics_.get(),
                                    "analysis.present.async", error);
          // Hidden/locked surfaces can exhaust the bounded drawable retry;
          // preserve the old frame and let exposure retry without a banner.
          const auto reason = error.safe_context.find("reason");
          if (reason != error.safe_context.end() &&
              reason->second == "drawable_retry_exhausted")
            return;
          current->widget->show_error(readable_error(error));
        });
      });
  window->set_request_handler(
      [runtime, gate, diagnostics](analysis::Request request) {
        const auto revision = request.revision;
        const auto started = std::chrono::steady_clock::now();
        record_diagnostic_stage(
            diagnostics.get(), runtime->prepared.original.session_id,
            OperationId{revision}, "analysis", "request", "accepted",
            {{"diffuseWhite",
              std::to_string(request.settings.reference_white_nits)}});
        runtime->session->request(
            std::move(request),
            [runtime, gate, revision, diagnostics, started](auto result) {
              if (!result)
                record_diagnostic_failure(diagnostics.get(), "analysis.compute",
                                          result.error());
              else
                record_diagnostic_stage(
                    diagnostics.get(), runtime->prepared.original.session_id,
                    OperationId{revision}, "analysis", "compute", "success",
                    {{"elapsedMs",
                      std::to_string(
                          std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count())}});
              gate->post([runtime, revision, result = std::move(result)] {
                if (!runtime->widget)
                  return;
                if (result)
                  runtime->widget->accept_result(result.value());
                else
                  runtime->widget->accept_error(revision,
                                                readable_error(result.error()));
              });
            });
      });
  window->set_present_handler([runtime, guard](analysis::SourceView view,
                                               std::uintptr_t surface) {
    if (!guard || !runtime->widget || !surface)
      return;
    auto result = runtime->backend.presenter->present(runtime->prepared.input,
                                                      view, surface);
    if (!result && result.error().safe_context.contains("reason") &&
        result.error().safe_context.at("reason") == "analysis_work_pending")
      return;
    if (!result && !runtime->presenter_error_reported) {
      runtime->presenter_error_reported = true;
      record_diagnostic_failure(guard->diagnostics_.get(), "analysis.present",
                                result.error());
      runtime->widget->show_error(readable_error(result.error()));
    } else if (result)
      runtime->presenter_error_reported = false;
  });
  window->set_preferences_changed([guard](std::string json) {
    if (!guard || guard->shutting_down_)
      return;
    guard->pending_preferences_ = std::move(json);
    guard->preference_timer_.start();
  });
  window->set_export_handler([runtime, guard](auto action, auto plan) {
    if (guard && !guard->shutting_down_)
      guard->export_from(runtime, action, std::move(plan));
  });
  QObject::connect(window, &QObject::destroyed, this, [runtime, guard] {
    runtime->session->stop();
    runtime->widget = nullptr;
    if (guard)
      guard->flush_preferences();
  });
}

void WindowsQtAnalyzerController::export_from(
    const std::shared_ptr<Window> &runtime, analysis::ExportAction action,
    analysis::ReportPlan plan) {
  if (!runtime->widget)
    return;
  const bool copy = action == analysis::ExportAction::copy_analysis ||
                    action == analysis::ExportAction::copy_original;
  auto original = runtime->prepared.original;
  const auto settings = read_settings_();
  original.operation_id = OperationId{next_operation_++};
  original.default_folder = settings.default_save_folder;
  original.save_format = settings.save_format;
  original.pq_diffuse_white = settings.pq_diffuse_white;
  original.hdr_pq_precision = settings.hdr_pq_precision;
  original.ultra_hdr_jpeg_quality = settings.ultra_hdr_jpeg_quality;
  runtime->widget->set_export_busy(true);
  if (action == analysis::ExportAction::copy_original ||
      action == analysis::ExportAction::save_original) {
    submit_export(runtime, copy, std::move(original));
    return;
  }
  const QPointer<WindowsQtAnalyzerController> guard(this);
  auto gate = dispatch_gate_;
  const auto queued = runtime->session->report(
      std::move(plan), [runtime, guard, gate, copy,
                        original = std::move(original)](auto result) {
        auto snapshot =
            result ? AnalysisWorkflow::report_snapshot(original, result.value())
                   : Result<ExportSnapshot, Error>::failure(result.error());
        gate->post([runtime, guard, copy,
                    snapshot = std::move(snapshot)]() mutable {
          if (!guard || !runtime->widget)
            return;
          if (snapshot)
            guard->submit_export(runtime, copy, std::move(snapshot.value()));
          else {
            runtime->widget->set_export_busy(false);
            runtime->widget->show_error(readable_error(snapshot.error()));
            record_diagnostic_failure(guard->diagnostics_.get(),
                                      "analysis.report", snapshot.error());
          }
        });
      });
  if (!queued) {
    runtime->widget->set_export_busy(false);
    runtime->widget->show_error(
        QStringLiteral("分析图任务未能加入队列，请重试。"));
  }
}

void WindowsQtAnalyzerController::submit_export(
    const std::shared_ptr<Window> &runtime, bool copy,
    ExportSnapshot snapshot) {
  auto adapted = windows_snapshot_with_capture_range(snapshot,
                                                     runtime->captured_range);
  if (!adapted) {
    runtime->widget->set_export_busy(false);
    runtime->widget->show_error(readable_error(adapted.error()));
    record_diagnostic_failure(diagnostics_.get(), "analysis.export",
                              adapted.error());
    return;
  }
  const bool accepted = export_request_(
      copy ? UiCommand::copy_and_close : UiCommand::save_default,
      std::move(adapted.value()),
      [runtime](ExportCompletionCoordinator::Outcome result) {
        if (!runtime->widget)
          return;
        runtime->widget->set_export_busy(false);
        if (!result)
          runtime->widget->show_error(readable_error(result.error()));
      });
  if (!accepted && runtime->widget) {
    runtime->widget->set_export_busy(false);
    runtime->widget->show_error(
        QStringLiteral("导出任务未能加入队列，请重试。"));
  }
}

void WindowsQtAnalyzerController::flush_preferences() {
  preference_timer_.stop();
  if (!pending_preferences_)
    return;
  auto value = std::move(*pending_preferences_);
  pending_preferences_.reset();
  save_preferences_(std::move(value));
}
bool WindowsQtAnalyzerController::close_windows() {
  if (closing_windows_)
    return false;
  closing_windows_ = true;
  // close() runs a nested modal loop. Keep a stable strong snapshot and reject
  // incoming installations until every user decision in this pass is complete.
  std::vector<std::shared_ptr<Window>> current;
  for (const auto &weak : windows_)
    if (auto runtime = weak.lock())
      current.push_back(std::move(runtime));
  for (const auto &runtime : current) {
    if (runtime->widget && !runtime->widget->close()) {
      closing_windows_ = false;
      return false;
    }
  }
  closing_windows_ = false;
  return true;
}
void WindowsQtAnalyzerController::shutdown() {
  if (shutting_down_)
    return;
  shutting_down_ = true;
  dispatch_gate_->close();
  flush_preferences();
  prepare_executor_.drain();
  for (const auto &weak : windows_)
    if (auto runtime = weak.lock()) {
      set_windows_analysis_presenter_error_callback(*runtime->backend.presenter,
                                                    {});
      runtime->session->stop();
      // Destruction is used only after application-level confirmation, or the
      // process event loop has ended. Never force-close an interactive window.
      if (runtime->widget)
        delete runtime->widget.data();
      (void)runtime->session->wait_for_stopped(std::chrono::milliseconds(2000));
    }
  windows_.clear();
}
} // namespace hdrshot
