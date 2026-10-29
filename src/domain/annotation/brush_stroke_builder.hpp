#pragma once
#include "domain/annotation/brush_coverage_kernel.hpp"
namespace hdrshot {
struct CompletedBrushStroke {
  AnnotationObject object;
  BrushPreview preview;
  ReusableAnnotationLayer reusable;
};
class BrushStrokeBuilder {
public:
  Result<bool, Error> begin(ObjectId, BrushStyle, StrokePoint, PixelRect,
                            AnnotationScale);
  Result<BrushCoverageDelta, Error> append(std::span<const StrokePoint>);
  Result<BrushPreview, Error> snapshot() const;
  Result<CompletedBrushStroke, Error> finish(StrokePoint);
  void cancel();
  bool sealed() const { return sealed_.has_value(); }
  const BrushCoverageDelta &counters() const { return accumulator_.counters; }
  std::size_t path_freezes() const { return path_freezes_; }

private:
  std::optional<BrushPreview> metadata_;
  std::vector<StrokePoint> points_;
  StrokePoint last_{};
  BrushCoverageAccumulator accumulator_;
  std::optional<CompletedBrushStroke> sealed_;
  std::size_t path_freezes_{};
};
} // namespace hdrshot
