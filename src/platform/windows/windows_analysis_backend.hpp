#pragma once

#include "ports/analysis_port.hpp"
#include <functional>
#include <memory>

namespace hdrshot {
class DiagnosticsPort;
struct WindowsDisplayState;
struct WindowsAnalysisBackend {
  std::shared_ptr<AnalysisPort> port;
  std::shared_ptr<AnalysisPresenterPort> presenter;
};

struct WindowsAnalysisPresentationStatus {
  std::uint64_t requested{}, submitted{}, completed{}, failed{}, dropped{};
  std::uint64_t completed_generation{};
  std::uint64_t display_generation{}, completed_display_generation{};
  bool worker_active{}, pending{};
};

Result<WindowsAnalysisBackend, Error> make_windows_analysis_backend(
    std::shared_ptr<DiagnosticsPort> diagnostics = {});
Result<std::shared_ptr<AnalysisPresenterPort>, Error>
make_windows_analysis_presenter(std::shared_ptr<DiagnosticsPort> diagnostics = {});
WindowsAnalysisPresentationStatus
windows_analysis_presentation_status(const AnalysisPresenterPort &presenter);
// Runs on the presenter worker. Handlers must only dispatch short, nonblocking
// work. Clearing the handler synchronizes with an in-progress delivery.
void set_windows_analysis_presenter_error_callback(
    AnalysisPresenterPort &presenter, std::function<void(Error)> callback);
// Event-driven target invalidation; the worker refreshes only its display LUT.
void invalidate_windows_analysis_display(AnalysisPresenterPort &presenter);
// UI-side binding publishes immutable display state. A null snapshot suspends
// submission until initialization completes; it never causes per-window queries.
void set_windows_analysis_display_target(AnalysisPresenterPort &presenter,
    std::uintptr_t surface, std::uintptr_t monitor, std::uint64_t binding_generation,
    std::shared_ptr<const WindowsDisplayState> state);
Result<LinearSourceRef, Error>
windows_analysis_render_offscreen(const analysis::Input &input,
                                  const analysis::SourceView &view);
} // namespace hdrshot
