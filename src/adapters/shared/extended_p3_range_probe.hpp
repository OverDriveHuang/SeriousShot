#pragma once
#include "ports/source_range_probe_port.hpp"

namespace hdrshot {
// Reference backend for the Extended P3 source contract used by macOS.
// Other capture encodings implement the same port, not a second classifier.
class ExtendedP3RangeProbe final : public SourceRangeProbePort {
 public:
  Result<RangeFitResult, Error> probe(
      const SelectionRoiView& source, const AnnotationPixelPlan& plan,
      RangeProbeOptimization optimization) override {
    return SourceRangeProbe::probe(source, plan, optimization);
  }
};
}  // namespace hdrshot
