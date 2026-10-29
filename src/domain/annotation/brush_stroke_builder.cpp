#include "domain/annotation/brush_stroke_builder.hpp"
#include "domain/annotation/annotation_allocation_checkpoint.hpp"
#include "domain/annotation/annotation_geometry.hpp"
#include <bit>
namespace hdrshot {
namespace {
Error invalid(ErrorCode code = ErrorCode::invalid_input) {
  return {code, "BrushStrokeBuilder"};
}
} // namespace
Result<bool, Error> BrushStrokeBuilder::begin(ObjectId id, BrushStyle style,
                                              StrokePoint p, PixelRect clip,
                                              AnnotationScale scale) {
  if (!valid_style(AnnotationKind::brush, style) ||
      !valid_annotation_clip(clip) || !std::isfinite(p.x) ||
      !std::isfinite(p.y) || std::abs(p.x) > 1e9 || std::abs(p.y) > 1e9 ||
      p.x < clip.x || p.y < clip.y || p.x > clip.right() || p.y > clip.bottom())
    return Result<bool, Error>::failure(invalid());
  try {
    BrushStrokeBuilder candidate;
    candidate.metadata_ = BrushPreview{{}, id, style, clip, scale, 1};
    annotation_allocation_checkpoint(AnnotationAllocationStage::point_growth);
    candidate.points_.push_back(p);
    auto local = StrokePoint{p.x - clip.x, p.y - clip.y};
    StrokeSegment segment{local, local};
    auto delta = accumulate_brush_segments(
        candidate.accumulator_, std::span(&segment, 1),
        style.diameter_dip * scale.value() * .5,
        {0, 0, clip.width, clip.height});
    if (!delta)
      return Result<bool, Error>::failure(delta.error());
    candidate.last_ = p;
    *this = std::move(candidate);
    return Result<bool, Error>::success(true);
  } catch (const std::bad_alloc &) {
    return Result<bool, Error>::failure(invalid(ErrorCode::state_inconsistent));
  }
}

Result<BrushCoverageDelta, Error>
BrushStrokeBuilder::append(std::span<const StrokePoint> points) {
  if (!metadata_ || sealed_)
    return Result<BrushCoverageDelta, Error>::failure(
        invalid(ErrorCode::precondition_failed));
  auto clip = metadata_->clip;
  if (!valid_annotation_clip(clip))
    return Result<BrushCoverageDelta, Error>::failure(invalid());
  for (auto p : points)
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || std::abs(p.x) > 1e9 ||
        std::abs(p.y) > 1e9 || p.x < clip.x || p.y < clip.y ||
        p.x > clip.right() || p.y > clip.bottom())
      return Result<BrushCoverageDelta, Error>::failure(invalid());
  try {
    std::vector<StrokeSegment> segments;
    auto last = last_;
    for (auto p : points)
      if (p != last) {
        segments.push_back(
            {{last.x - clip.x, last.y - clip.y}, {p.x - clip.x, p.y - clip.y}});
        last = p;
      }
    if (segments.size() > points_.max_size() - points_.size())
      return Result<BrushCoverageDelta, Error>::failure(invalid());
    annotation_allocation_checkpoint(AnnotationAllocationStage::point_growth);
    // Prepare every throwing operation before coverage publication.
    const auto needed = points_.size() + segments.size();
    if (needed > points_.capacity()) {
      const auto limit = std::size_t{1}
                         << (std::numeric_limits<std::size_t>::digits - 1);
      points_.reserve(needed <= limit ? std::bit_ceil(needed) : needed);
    }
    auto delta = accumulate_brush_segments(accumulator_, segments,
                                           metadata_->style.diameter_dip *
                                               metadata_->scale.value() * .5,
                                           {0, 0, clip.width, clip.height});
    if (!delta)
      return delta;
    for (auto p : points)
      if (p != last_) {
        points_.push_back(p);
        last_ = p;
        ++metadata_->trajectory_revision;
      }
    delta.value().trajectory_revision = metadata_->trajectory_revision;
    return delta;
  } catch (const std::bad_alloc &) {
    return Result<BrushCoverageDelta, Error>::failure(
        invalid(ErrorCode::state_inconsistent));
  }
}

Result<BrushPreview, Error> BrushStrokeBuilder::snapshot() const {
  try {
    if (!metadata_)
      return Result<BrushPreview, Error>::failure(
          invalid(ErrorCode::precondition_failed));
    if (sealed_)
      return Result<BrushPreview, Error>::success(sealed_->preview);
    auto result = *metadata_;
    result.layer = freeze_brush_coverage(
        accumulator_, result.id, result.style.color_srgb_rgb,
        {0, 0, result.clip.width, result.clip.height});
    return Result<BrushPreview, Error>::success(std::move(result));
  } catch (const std::bad_alloc &) {
    return Result<BrushPreview, Error>::failure(
        invalid(ErrorCode::state_inconsistent));
  }
}
Result<CompletedBrushStroke, Error>
BrushStrokeBuilder::finish(StrokePoint final) {
  try {
    if (sealed_)
      return final == last_
                 ? Result<CompletedBrushStroke, Error>::success(*sealed_)
                 : Result<CompletedBrushStroke, Error>::failure(
                       invalid(ErrorCode::precondition_failed));
    // A new release tail is staged separately. The usual already-appended
    // tail never copies the accumulator; path storage is frozen once below.
    if (final != last_) {
      auto candidate = *this;
      auto appended = candidate.append(std::span(&final, 1));
      if (!appended)
        return Result<CompletedBrushStroke, Error>::failure(appended.error());
      auto result = candidate.finish(final);
      if (!result)
        return result;
      *this = std::move(candidate);
      return result;
    }
    auto preview = snapshot();
    if (!preview)
      return Result<CompletedBrushStroke, Error>::failure(preview.error());
    annotation_allocation_checkpoint(AnnotationAllocationStage::finish_path);
    auto path =
        std::make_shared<const BrushPath>(std::vector<StrokePoint>(points_));
    AnnotationObject object;
    object.id = metadata_->id;
    object.kind = AnnotationKind::brush;
    object.style = metadata_->style;
    object.brush_geometry = BrushGeometry{path};
    auto bounds = brush_geometry_bounds(*object.brush_geometry);
    if (!bounds)
      return Result<CompletedBrushStroke, Error>::failure(bounds.error());
    object.bounds = bounds.value();
    ReusableAnnotationLayer reusable{object, preview.value().layer,
                                     metadata_->clip, metadata_->scale,
                                     metadata_->trajectory_revision};
    annotation_allocation_checkpoint(AnnotationAllocationStage::finish_proof);
    reusable.seal();
    CompletedBrushStroke completed{
        std::move(object), std::move(preview.value()), std::move(reusable)};
    auto result = Result<CompletedBrushStroke, Error>::success(completed);
    sealed_ = std::move(completed);
    ++path_freezes_;
    return result;
  } catch (const std::bad_alloc &) {
    return Result<CompletedBrushStroke, Error>::failure(
        invalid(ErrorCode::state_inconsistent));
  }
}

void BrushStrokeBuilder::cancel() {
  metadata_.reset();
  points_.clear();
  accumulator_ = {};
  sealed_.reset();
  path_freezes_ = 0;
}
} // namespace hdrshot
