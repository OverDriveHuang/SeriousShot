#pragma once
#include "domain/output/source_range_probe.hpp"

namespace hdrshot {
class SourceRangeProbePort {
 public:
  virtual ~SourceRangeProbePort() = default;
  // Interpret native pixels in target P3 EDR units. No OS value reaches D7.
  virtual Result<RangeFitResult, Error> probe(
      const SelectionRoiView& source, const AnnotationPixelPlan& plan,
      RangeProbeOptimization optimization) = 0;
};
}  // namespace hdrshot
