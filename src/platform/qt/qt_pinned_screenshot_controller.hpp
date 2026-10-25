#pragma once
#include "application/analysis_workflow.hpp"
#include "application/export_completion_coordinator.hpp"
#include "application/input_mapper.hpp"
#include "platform/qt/qt_export_task_executor.hpp"
#include "ui/qt/pinned_screenshot_window.hpp"
#include <QObject>
#include <array>
#include <memory>
#include <vector>

namespace hdrshot {
// The platform prepares the existing Source Signal backend; the shared
// controller owns snapshot/action/window lifetime policy.
struct PreparedPinnedAuxiliary {
  std::shared_ptr<AnalysisPresenterPort> presenter;
  std::function<void(std::function<void(Error)>)> observe_errors;
  std::function<Result<bool,Error>(QWidget &, QWidget &, std::function<void()>,
                                   std::function<void(Error)>)> attach;
};
struct PreparedPinnedScreenshot {
  PreparedAnalysis image;
  std::shared_ptr<AnalysisPresenterPort> presenter;
  // Adapter marshals late native errors onto the UI thread.
  std::function<void(std::function<void(Error)>)> observe_errors;
  // Optional native surface setup, completed before creation is acknowledged.
  std::function<Result<bool, Error>(PinnedScreenshotWindow &)> attach_window;
  // Optional actual-completion recovery, delivered on the UI thread. An absent
  // observer does not imply that present() waits for native completion.
  std::function<void(std::function<void()>)> observe_recovered{};
  std::array<PreparedPinnedAuxiliary,2> auxiliary{};
};
class QtPinnedScreenshotController final : public QObject {
public:
  using Completed = std::function<void(Result<bool, Error>)>;
  using Prepare = std::function<Result<PreparedPinnedScreenshot, Error>(const ExportSnapshot &)>;
  using Export = std::function<bool(UiCommand, ExportSnapshot, ExportCompletionCoordinator::Completed, QWidget*)>;
  using Analyze = std::function<void(ExportSnapshot, Completed)>;
  using ReadSettings = std::function<SettingsSnapshot()>;
  QtPinnedScreenshotController(QObject &dispatch, Prepare prepare,
      std::shared_ptr<QtPinnedWindowPort> native_window, Export export_request,
      Analyze analyze, ReadSettings settings, std::shared_ptr<DiagnosticsPort> diagnostics);
  ~QtPinnedScreenshotController() override;
  void open(ExportSnapshot, Completed);
  bool has_windows() const;
  void shutdown();
private:
  struct Window;
  Result<bool, Error> install(std::shared_ptr<Window>, PinnedScreenshotDefaults, QRect);
  void action(std::shared_ptr<Window>, UiCommand);
  QObject &dispatch_;
  QtExportTaskExecutor executor_;
  Prepare prepare_;
  std::shared_ptr<QtPinnedWindowPort> native_;
  Export export_;
  Analyze analyze_;
  ReadSettings settings_;
  std::shared_ptr<DiagnosticsPort> diagnostics_;
  std::vector<std::weak_ptr<Window>> windows_;
  std::uint64_t next_operation_{100000};
  std::size_t pending_{};
  bool shutting_down_{};
};
} // namespace hdrshot
