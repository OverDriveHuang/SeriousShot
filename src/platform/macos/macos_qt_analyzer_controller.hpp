#pragma once
#include "application/export_completion_coordinator.hpp"
#include "domain/analysis/types.hpp"
#include "platform/qt/qt_export_task_executor.hpp"
#include "ports/export_ports.hpp"
#include "ui/qt/qt_overlay_host.hpp"
#include <QObject>
#include <QTimer>
#include <memory>
#include <optional>

namespace hdrshot {
// Native composition root only. Math, scheduling and snapshot semantics live
// outside this adapter; export continues through the existing coordinator.
class MacQtAnalyzerController final : public QObject {
public:
  using Export = std::function<bool(UiCommand, ExportSnapshot,
                                    ExportCompletionCoordinator::Completed)>;
  using ReadSettings = std::function<SettingsSnapshot()>;
  using SavePreferences = std::function<void(std::string)>;
  MacQtAnalyzerController(QObject &dispatch_context, Export export_request,
                          ReadSettings read_settings,
                          SavePreferences save_preferences,
                          std::shared_ptr<DiagnosticsPort> diagnostics);
  ~MacQtAnalyzerController() override;
  void open(ExportSnapshot snapshot,
            QtOverlayHost::AnalysisCompleted completed);
  // Returns false if the user elects to retain an analysis window.
  bool close_windows();
  void shutdown();

private:
  struct Window;
  void install(std::shared_ptr<Window> runtime);
  void export_from(const std::shared_ptr<Window> &, analysis::ExportAction,
                   analysis::ReportPlan);
  void submit_export(const std::shared_ptr<Window> &, bool copy,
                     ExportSnapshot);
  void flush_preferences();
  QObject &dispatch_context_;
  QtExportTaskExecutor prepare_executor_;
  Export export_request_;
  ReadSettings read_settings_;
  SavePreferences save_preferences_;
  std::shared_ptr<DiagnosticsPort> diagnostics_;
  std::vector<std::weak_ptr<Window>> windows_;
  std::optional<std::string> pending_preferences_;
  QTimer preference_timer_;
  std::uint64_t next_operation_{1000};
  bool shutting_down_{}, closing_windows_{};
};
} // namespace hdrshot
