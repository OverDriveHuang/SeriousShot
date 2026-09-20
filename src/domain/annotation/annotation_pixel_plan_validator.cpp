#include "domain/annotation/annotation_pixel_plan_validator.hpp"
#include <algorithm>
#include <cmath>
#include <type_traits>

namespace hdrshot {
namespace {
template <typename Span> bool valid_samples(const Span& span) {
  if constexpr (std::is_same_v<Span, AnnotationOwnedSpan>) {
    if (span.native_samples) return span.clean_composited && span.edge_samples.empty() &&
        span.length > 0 && span.native_sample_offset <= span.native_samples->sample_count() &&
        static_cast<std::size_t>(span.length) <= span.native_samples->sample_count() - span.native_sample_offset;
    if (span.edge_samples.empty()) return !span.clean_composited;
    if (span.edge_samples.size() != static_cast<std::size_t>(span.length)) return false;
    for (const auto& sample : span.edge_samples) {
      if (!std::isfinite(sample[3]) || sample[3] < 0.0F || sample[3] >= 1.0F) return false;
      if (span.clean_composited && sample[3] != 0.0F) return false;
      for (std::size_t c = 0; c < 3; ++c)
        if (!std::isfinite(sample[c]) || sample[c] < 0.0F ||
            (!span.clean_composited && sample[c] > 1.0F - sample[3] + 0.000001F)) return false;
    }
  }
  return true;
}
}
bool AnnotationPixelPlanValidator::valid_row(
    const AnnotationPixelPlan& plan, const std::int32_t y) {
  if (y < 0 || y >= plan.output_size_px.height || plan.output_size_px.width <= 0)
    return false;
  const auto lower_row = [y](const auto& spans) {
    return std::lower_bound(spans.begin(), spans.end(), y,
        [](const auto& span, auto row) { return span.y < row; });
  };
  auto source = lower_row(plan.source_visible_spans);
  auto annotation = lower_row(plan.annotation_owned_spans);
  std::int32_t next_x = 0;
  const auto accept = [&](const auto& span) {
    if (!valid_samples(span) || span.x != next_x || span.length <= 0 ||
        span.length > plan.output_size_px.width - next_x) return false;
    next_x += span.length;
    return true;
  };
  while (true) {
    const bool has_source = source != plan.source_visible_spans.end() && source->y == y;
    const bool has_annotation =
        annotation != plan.annotation_owned_spans.end() && annotation->y == y;
    if (!has_source && !has_annotation) break;
    if (has_source && (!has_annotation || source->x <= annotation->x)) {
      if (!accept(*source++)) return false;
    } else if (!accept(*annotation++)) return false;
  }
  return next_x == plan.output_size_px.width;
}

bool AnnotationPixelPlanValidator::valid(const AnnotationPixelPlan& plan) {
  if (plan.output_size_px.width <= 0 || plan.output_size_px.height <= 0) return false;
  auto source = plan.source_visible_spans.begin();
  auto annotation = plan.annotation_owned_spans.begin();
  // Merge the two ordered span streams. Exact adjacency proves that every
  // output pixel has exactly one owner, without allocating a full-image mask.
  for (std::int32_t y = 0; y < plan.output_size_px.height; ++y) {
    std::int32_t next_x = 0;
    const auto accept = [&](const auto& span) {
      if (!valid_samples(span) || span.x != next_x || span.length <= 0 ||
          span.length > plan.output_size_px.width - next_x) return false;
      next_x += span.length;
      return true;
    };
    while (true) {
      const bool has_source = source != plan.source_visible_spans.end() && source->y == y;
      const bool has_annotation =
          annotation != plan.annotation_owned_spans.end() && annotation->y == y;
      if (!has_source && !has_annotation) break;
      if (has_source && (!has_annotation || source->x <= annotation->x)) {
        if (!accept(*source++)) return false;
      } else {
        if (!accept(*annotation++)) return false;
      }
    }
    if (next_x != plan.output_size_px.width) return false;
  }
  return source == plan.source_visible_spans.end() &&
      annotation == plan.annotation_owned_spans.end();
}
}  // namespace hdrshot
