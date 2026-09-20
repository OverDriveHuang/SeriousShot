#include "platform/macos/macos_qt_analyzer_controller.hpp"
#include "application/analysis_session.hpp"
#include "application/analysis_workflow.hpp"
#include "platform/macos/macos_analysis_backend.hpp"
#include "platform/macos/macos_metal_export_pixel_processor.hpp"
#include "ui/qt/analyzer_window.hpp"
#include <QApplication>
#include <QPointer>
#include <chrono>
#include <exception>
#include <dispatch/dispatch.h>

namespace hdrshot {
namespace {
Error adapter_error(const char *reason) {
  return {ErrorCode::invalid_input,
          "MacQtAnalyzerController",
          Retryability::after_user_action,
          {{"reason", reason}}};
}
QString readable_error(const Error &error) {
  return QStringLiteral("%1：%2").arg(
      QString::fromStdString(error.module),
      QString::fromStdString(to_string(error.code)));
}
} // namespace
struct MacQtAnalyzerController::Window {
  PreparedAnalysis prepared;
  MacAnalysisBackend backend;
  std::shared_ptr<AnalysisSession> session;
  QPointer<AnalyzerWindow> widget;
  bool presenter_error_reported{};
};

MacQtAnalyzerController::MacQtAnalyzerController(
    QObject &context, Export export_request, ReadSettings settings,
    SavePreferences preferences, std::shared_ptr<DiagnosticsPort> diagnostics)
    : dispatch_context_(context), prepare_executor_(context),
      export_request_(std::move(export_request)),
      read_settings_(std::move(settings)),
      save_preferences_(std::move(preferences)),
      diagnostics_(std::move(diagnostics)) {
  preference_timer_.setSingleShot(true);
  preference_timer_.setInterval(300);
  QObject::connect(&preference_timer_, &QTimer::timeout, this,
                   [this] { flush_preferences(); });
}
MacQtAnalyzerController::~MacQtAnalyzerController() { shutdown(); }

void MacQtAnalyzerController::open(ExportSnapshot snapshot,
                                   QtOverlayHost::AnalysisCompleted completed) {
  if (shutting_down_ || closing_windows_) {
    completed(Result<bool, Error>::failure(adapter_error("closing_windows")));
    return;
  }
  const QPointer<MacQtAnalyzerController> guard(this);
  QObject *dispatch = &dispatch_context_;
  auto diagnostics = diagnostics_;
  record_diagnostic_stage(diagnostics.get(), snapshot.session_id,
                          snapshot.operation_id, "analysis", "prepare",
                          "begin");
  prepare_executor_.background([guard, dispatch, diagnostics,
                                snapshot = std::move(snapshot),
                                completed = std::move(completed)]() mutable {
    const auto started = std::chrono::steady_clock::now();
    auto prepare = [&]() -> Result<std::shared_ptr<Window>, Error> {
      auto backend = make_macos_analysis_backend();
      if (!backend)
        return Result<std::shared_ptr<Window>, Error>::failure(backend.error());
      // Capture/export has its own processor and queue. Do not race that
      // mutable executor while establishing the analysis source class.
      auto range = MacMetalExportPixelProcessor::create();
      if (!range)
        return Result<std::shared_ptr<Window>, Error>::failure(range.error());
      auto prepared = AnalysisWorkflow::prepare(snapshot, *backend.value().port,
                                                range.value().get());
      if (!prepared)
        return Result<std::shared_ptr<Window>, Error>::failure(
            prepared.error());
      auto runtime = std::make_shared<Window>();
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
    QMetaObject::invokeMethod(
        dispatch,
        [guard, result = std::move(result),
         completed = std::move(completed)]() mutable {
          if (!guard || guard->shutting_down_ || guard->closing_windows_) {
            completed(
                Result<bool, Error>::failure(adapter_error("shutting_down")));
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
        },
        Qt::QueuedConnection);
  });
}

void MacQtAnalyzerController::install(std::shared_ptr<Window> runtime) {
  std::erase_if(windows_, [](const auto &weak) { return weak.expired(); });
  windows_.push_back(runtime);
  auto *window = new AnalyzerWindow(runtime->prepared.input,
                                    read_settings_().analyzer_preferences_json);
  runtime->widget = window;
  window->setAttribute(Qt::WA_DeleteOnClose);
  const QPointer<MacQtAnalyzerController> guard(this);
  QObject *dispatch = &dispatch_context_;
  auto diagnostics = diagnostics_;
  const std::weak_ptr<Window> weak_runtime = runtime;
  set_macos_analysis_presenter_error_callback(
      *runtime->backend.presenter,
      [guard, weak_runtime](Error error) {
        // The detached presenter can finish after the Qt dispatcher is gone.
        // Enqueue without dereferencing a QObject; inspect weak lifetimes only
        // on the main queue, where the controller and widgets are destroyed.
        dispatch_async(dispatch_get_main_queue(), ^{
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
  window->set_request_handler([runtime, dispatch,
                               diagnostics](analysis::Request request) {
    const auto revision = request.revision;
    const auto started = std::chrono::steady_clock::now();
    record_diagnostic_stage(
        diagnostics.get(), runtime->prepared.original.session_id,
        OperationId{revision}, "analysis", "request", "accepted",
        {{"diffuseWhite",
          std::to_string(request.settings.reference_white_nits)}});
    runtime->session->request(
        std::move(request),
        [runtime, dispatch, revision, diagnostics, started](auto result) {
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
          QMetaObject::invokeMethod(
              dispatch,
              [runtime, revision, result = std::move(result)] {
                if (!runtime->widget)
                  return;
                if (result)
                  runtime->widget->accept_result(result.value());
                else
                  runtime->widget->accept_error(revision,
                                                readable_error(result.error()));
              },
              Qt::QueuedConnection);
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

void MacQtAnalyzerController::export_from(
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
  const QPointer<MacQtAnalyzerController> guard(this);
  QObject *dispatch = &dispatch_context_;
  const auto queued = runtime->session->report(
      std::move(plan), [runtime, guard, dispatch, copy,
                        original = std::move(original)](auto result) {
        auto snapshot =
            result ? AnalysisWorkflow::report_snapshot(original, result.value())
                   : Result<ExportSnapshot, Error>::failure(result.error());
        QMetaObject::invokeMethod(
            dispatch,
            [runtime, guard, copy, snapshot = std::move(snapshot)]() mutable {
              if (!guard || !runtime->widget)
                return;
              if (snapshot)
                guard->submit_export(runtime, copy,
                                     std::move(snapshot.value()));
              else {
                runtime->widget->set_export_busy(false);
                runtime->widget->show_error(readable_error(snapshot.error()));
                record_diagnostic_failure(guard->diagnostics_.get(),
                                          "analysis.report", snapshot.error());
              }
            },
            Qt::QueuedConnection);
      });
  if (!queued) {
    runtime->widget->set_export_busy(false);
    runtime->widget->show_error(
        QStringLiteral("分析图任务未能加入队列，请重试。"));
  }
}

void MacQtAnalyzerController::submit_export(
    const std::shared_ptr<Window> &runtime, bool copy,
    ExportSnapshot snapshot) {
  export_request_(copy ? UiCommand::copy_and_close : UiCommand::save_default,
                  std::move(snapshot),
                  [runtime](ExportCompletionCoordinator::Outcome result) {
                    if (!runtime->widget)
                      return;
                    runtime->widget->set_export_busy(false);
                    if (!result)
                      runtime->widget->show_error(
                          readable_error(result.error()));
                  });
}

void MacQtAnalyzerController::flush_preferences() {
  preference_timer_.stop();
  if (!pending_preferences_)
    return;
  auto value = std::move(*pending_preferences_);
  pending_preferences_.reset();
  save_preferences_(std::move(value));
}
bool MacQtAnalyzerController::close_windows() {
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
void MacQtAnalyzerController::shutdown() {
  if (shutting_down_)
    return;
  shutting_down_ = true;
  flush_preferences();
  prepare_executor_.drain();
  for (const auto &weak : windows_)
    if (auto runtime = weak.lock()) {
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
