#pragma once
#include "ports/analysis_port.hpp"
#include <functional>
namespace hdrshot {
struct MacAnalysisBackend {
  std::shared_ptr<AnalysisPort> port;
  std::shared_ptr<AnalysisPresenterPort> presenter;
};
Result<MacAnalysisBackend, Error> make_macos_analysis_backend();
struct MacAnalysisPresentationStatus {
  std::uint64_t requested{}, submitted{}, completed{}, failed{}, dropped{};
  std::uint64_t completed_generation{};
  bool worker_active{}, pending{};
};
// Read-only diagnostic state. GPU completion is not proof of WindowServer
// visibility; native tests and performance instrumentation keep those separate.
MacAnalysisPresentationStatus
macos_analysis_presentation_status(const AnalysisPresenterPort &presenter);
// Callback runs off the GUI thread. Native controller must marshal UI/log sink
// actions and retain only weak ownership of its window/controller.
void set_macos_analysis_presenter_error_callback(
    AnalysisPresenterPort &presenter, std::function<void(Error)> callback);
// Recovery is offered only after a visible presentation failure and an actual
// GPU completion. Marshal to the GUI, then acknowledge the generation before
// clearing display-only UI state. A stale acknowledgement leaves recovery armed.
void set_macos_analysis_presenter_recovery_callback(
    AnalysisPresenterPort &presenter,
    std::function<void(std::uint64_t)> callback);
bool acknowledge_macos_analysis_presenter_recovery(
    AnalysisPresenterPort &presenter, std::uint64_t generation);
// Same Source renderer as the on-screen CAMetalLayer, with an explicit float
// readback boundary through the returned immutable LinearSource.
Result<LinearSourceRef, Error>
macos_analysis_render_offscreen(const analysis::Input &input,
                                const analysis::SourceView &view);
} // namespace hdrshot
