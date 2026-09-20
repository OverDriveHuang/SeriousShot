#pragma once

#include "core/error.hpp"
#include "core/result.hpp"
#include "domain/annotation/annotation_render_plan.hpp"
#include "domain/frame/frame_cropper.hpp"
#include "domain/output/range_fit.hpp"

namespace hdrshot {

struct RangeProbeOptimization {
  // This is a normalized platform-capture contract, not an OS API value.
  // The caller may set it only when the current frame is proven unable to
  // contain source RGB above EDR 1.0.
  bool source_visible_within_sdr_guaranteed{};
};

class SourceRangeProbe {
 public:
  [[nodiscard]] static Result<RangeFitResult, Error> probe(
      const CanonicalFrameView& frame,
      const AnnotationPixelPlan& pixel_plan,
      RangeProbeOptimization optimization = {});

  [[nodiscard]] static Result<RangeFitResult, Error> probe(
      const SelectionRoiView& frame,
      const AnnotationPixelPlan& pixel_plan,
      RangeProbeOptimization optimization = {});
};

}  // namespace hdrshot
