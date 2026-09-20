#pragma once
#include "domain/analysis/types.hpp"
#include "domain/frame/frame_cropper.hpp"
#include "domain/annotation/annotation_render_plan.hpp"
#include "core/result.hpp"
#include "core/error.hpp"

namespace hdrshot {
// One executor per analysis window. Methods are serialized on its worker, never
// called from a pointer event. View-only requests reuse cached work/statistics.
class AnalysisPort {
 public:
  virtual ~AnalysisPort() = default;
  virtual Result<LinearSourceRef, Error> prepare(
      const SelectionRoiView& source, const AnnotationPixelPlan& annotations) = 0;
  virtual Result<analysis::ResultRef, Error> analyze(
      const analysis::Input& input, const analysis::Request& request) = 0;
  virtual Result<LinearSourceRef, Error> compose_report(
      const analysis::Input& input, const analysis::ReportPlan& plan) = 0;
};

// Native surface is an opaque handle supplied by the composition root. The
// implementation owns its platform interpretation. UI-thread-only calls.
class AnalysisPresenterPort {
 public:
  virtual ~AnalysisPresenterPort() = default;
  virtual Result<bool, Error> present(const analysis::Input& input,
      const analysis::SourceView& view, std::uintptr_t native_surface) = 0;
};
} // namespace hdrshot
