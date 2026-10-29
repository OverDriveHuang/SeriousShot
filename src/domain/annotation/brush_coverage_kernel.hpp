#pragma once
#include "domain/annotation/annotation_coverage.hpp"
#include <map>
namespace hdrshot {
struct StrokeSegment {
  StrokePoint start, end;
};
// Rows contain immutable published descriptors and sparse mutable boundary
// state.
struct BrushCoverageAccumulator {
  struct Row {
    std::vector<std::pair<int, int>> opaque;
    std::vector<std::pair<int, std::uint8_t>> aa;
    std::vector<CoverageSpan> spans;
  };
  std::map<int, Row> rows;
  BrushCoverageDelta counters;
};
Result<BrushCoverageDelta, Error>
accumulate_brush_segments(BrushCoverageAccumulator &,
                          std::span<const StrokeSegment>, double radius_px,
                          PixelRect clip_px);
AnnotationCoverageLayer freeze_brush_coverage(const BrushCoverageAccumulator &,
                                              ObjectId,
                                              std::uint32_t color_srgb_rgb,
                                              PixelRect clip_px);
} // namespace hdrshot
