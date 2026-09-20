#include "application/clean_content_cache.hpp"
#include "domain/annotation/annotation_compositing.hpp"
#include <algorithm>
#include <cmath>

namespace hdrshot {
namespace {
Error content_error(const char* reason) {
  return {ErrorCode::state_inconsistent, "CleanContentCache", Retryability::never,
          {{"reason", reason}}};
}
}
Result<std::vector<std::array<float, 4>>, Error> CpuCleanComposition::compose(
    std::span<const CleanCompositionSample> samples) {
  std::vector<std::array<float, 4>> result;
  result.reserve(samples.size());
  for (const auto& s : samples) {
    std::array<float, 4> value{s.annotation[0], s.annotation[1], s.annotation[2], 0};
    for (std::size_t c = 0; c < 3; ++c) {
      if (s.annotation[3] > 0) value[c] += s.annotation[3] * std::max(0.0F, s.source[c]);
      if (!std::isfinite(value[c])) return decltype(compose(samples))::failure(content_error("nonfinite_composition"));
    }
    result.push_back(value);
  }
  return Result<std::vector<std::array<float, 4>>, Error>::success(std::move(result));
}

Result<AnnotationPixelPlan, Error> CleanContentSnapshot::roi_plan(
    PixelRect selection, DocumentRevision revision) const {
  if (!source) return Result<AnnotationPixelPlan, Error>::failure(content_error("missing_source"));
  const auto segment = std::find_if(source->canonical_segments.begin(),source->canonical_segments.end(),
      [&](const auto& s) { return s.display_id == display_id; });
  if (segment == source->canonical_segments.end() || selection.x < 0 || selection.y < 0 ||
      selection.right() > segment->size_px.width || selection.bottom() > segment->size_px.height || selection.empty()) return Result<AnnotationPixelPlan, Error>::failure(content_error("empty_roi"));
  AnnotationPixelPlan result{revision, {selection.width, selection.height}, {}, {}};
  for (const auto& span : owned) {
    if (span.y < selection.y || span.y >= selection.bottom()) continue;
    const auto left = std::max(span.x, selection.x);
    const auto right = std::min(span.x + span.length, selection.right());
    if (left >= right) continue;
    auto copy = span;
    copy.x = left - selection.x; copy.y -= selection.y; copy.length = right - left;
    if (copy.native_samples) copy.native_sample_offset += static_cast<std::size_t>(left - span.x);
    if (!span.edge_samples.empty()) copy.edge_samples = {
        span.edge_samples.begin() + (left - span.x), span.edge_samples.begin() + (right - span.x)};
    result.annotation_owned_spans.push_back(std::move(copy));
  }
  auto it = result.annotation_owned_spans.begin();
  for (std::int32_t y = 0; y < selection.height; ++y) {
    std::int32_t x = 0;
    while (it != result.annotation_owned_spans.end() && it->y == y) {
      if (it->x > x) result.source_visible_spans.push_back({y, x, it->x - x});
      x = it->x + it->length; ++it;
    }
    if (x < selection.width) result.source_visible_spans.push_back({y, x, selection.width - x});
  }
  return Result<AnnotationPixelPlan, Error>::success(std::move(result));
}
CleanContentCache::CleanContentCache(std::shared_ptr<CleanCompositionPort> executor)
    : executor_(executor ? std::move(executor) : std::make_shared<CpuCleanComposition>()) {}
CleanContentCounters CleanContentCache::counters() const {
  std::scoped_lock lock(mutex_); return counters_;
}
Result<CleanContentRef, Error> CleanContentCache::get(FrozenDesktopRef source, DisplayId display,
    std::shared_ptr<const AnnotationRenderPlan> plan) {
  if (!plan) return Result<CleanContentRef, Error>::failure(content_error("missing_plan"));
  {
    std::scoped_lock lock(mutex_);
    if (current_ && current_->source == source && current_->display_id == display && last_plan_ == plan) {
      ++counters_.cache_hits;
      return Result<CleanContentRef, Error>::success(current_);
    }
  }
  auto result = get(std::move(source), display, *plan);
  if (result) {
    std::scoped_lock lock(mutex_);
    if (current_ == result.value()) last_plan_ = std::move(plan);
  }
  return result;
}
Result<CleanContentRef, Error> CleanContentCache::get(
    FrozenDesktopRef source, DisplayId display, const AnnotationRenderPlan& plan) {
  std::scoped_lock lock(mutex_);
  if (!source) return Result<CleanContentRef, Error>::failure(content_error("missing_source"));
  const auto segment = std::find_if(source->canonical_segments.begin(), source->canonical_segments.end(),
      [display](const auto& s) { return s.display_id == display; });
  if (segment == source->canonical_segments.end()) return Result<CleanContentRef, Error>::failure(content_error("missing_display"));
  if (plan.source_selection_rect_px.empty() && !plan.ordered_layers.empty())
    return Result<CleanContentRef, Error>::failure(content_error("layers_without_selection"));
  const PixelRect full{0, 0, segment->size_px.width, segment->size_px.height};
  auto global = plan.source_selection_rect_px.empty()
      ? Result<AnnotationRenderPlan, Error>::success(AnnotationRenderPlan{plan.source_document_revision, segment->size_px, {}, full})
      : AnnotationRenderPlanner::rebase(plan, full);
  if (!global) return Result<CleanContentRef, Error>::failure(global.error());
  if (current_ && current_->source == source && current_->display_id == display &&
      layers_ == global.value().ordered_layers) {
    ++counters_.cache_hits;
    return Result<CleanContentRef, Error>::success(current_);
  }
  // Resolve only the coverage bounding rectangle, not every selected screen pixel.
  int left=full.width, top=full.height, right=0, bottom=0;
  for (const auto& layer : global.value().ordered_layers) for (const auto& span : layer.spans) {
    if (span.coverage_u8.empty()) continue;
    left=std::min(left,span.x);top=std::min(top,span.y);
    right=std::max(right,span.x+static_cast<int>(span.coverage_u8.size()));bottom=std::max(bottom,span.y+1);
  }
  const PixelRect content_rect{left,top,right-left,bottom-top};
  auto pixels = Result<AnnotationPixelPlan, Error>::success(AnnotationPixelPlan{});
  if (!content_rect.empty()) {
    auto local = AnnotationRenderPlanner::rebase(global.value(), content_rect);
    if (!local) return Result<CleanContentRef, Error>::failure(local.error());
    pixels = AnnotationRenderPlanner::build_pixel_plan(local.value());
  }
  if (!pixels) return Result<CleanContentRef, Error>::failure(pixels.error());
  auto next = std::make_shared<CleanContentSnapshot>();
  next->source = source; next->display_id = display;
  next->content_revision = counters_.content_builds + 1;
  next->owned = std::move(pixels.value().annotation_owned_spans);
  auto frame = FrameCropper::view_display(*source, display, {0, full});
  if (!frame) return Result<CleanContentRef, Error>::failure(frame.error());
  std::vector<CleanCompositionSample> work;
  std::vector<PositionedCleanSample> native_work;
  for (auto& span : next->owned) {
    span.x += content_rect.x; span.y += content_rect.y;
    for (std::int32_t i = 0; i < span.length; ++i) {
      CleanCompositionSample sample{annotation_linear_sample(span, static_cast<std::size_t>(i)), {}};
      if (frame.value().linear_source) {
        native_work.push_back({sample.annotation, static_cast<std::uint32_t>(span.x + i),
            static_cast<std::uint32_t>(span.y)});
        continue;
      }
      if (sample.annotation[3] > 0) {
        const auto offset = static_cast<std::size_t>(span.y) * frame.value().row_stride_samples +
                            static_cast<std::size_t>(span.x + i) * 4;
        for (std::size_t c = 0; c < 3; ++c) {
          auto value = frame.value().sample(offset + c);
          if (!value) return Result<CleanContentRef, Error>::failure(value.error());
          sample.source[c] = ExtendedP3Mapper::source_linear(value.value(), frame.value().encoding.transfer);
        }
      }
      work.push_back(sample);
    }
  }
  std::size_t offset = 0;
  if (!native_work.empty()) {
    auto composed = executor_->compose_native(frame.value().linear_source, native_work);
    if (!composed) return Result<CleanContentRef, Error>::failure(composed.error());
    if (!composed.value() || composed.value()->sample_count() != native_work.size())
      return Result<CleanContentRef, Error>::failure(content_error("native_executor_shape_mismatch"));
    for (auto& span : next->owned) {
      span.clean_composited = true;
      span.native_samples = composed.value();
      span.native_sample_offset = offset;
      std::vector<std::array<float, 4>>().swap(span.edge_samples);
      offset += static_cast<std::size_t>(span.length);
    }
  } else {
    auto composed = executor_->compose(work);
    if (!composed) return Result<CleanContentRef, Error>::failure(composed.error());
    if (composed.value().size() != work.size()) return Result<CleanContentRef, Error>::failure(content_error("executor_shape_mismatch"));
    for (const auto& sample : composed.value()) {
      if (sample[3] != 0 || !std::isfinite(sample[3]))
        return Result<CleanContentRef, Error>::failure(content_error("executor_source_weight_not_zero"));
      for (std::size_t c=0;c<3;++c) if (!std::isfinite(sample[c]) || sample[c]<0)
        return Result<CleanContentRef, Error>::failure(content_error("executor_invalid_linear_sample"));
    }
    for (auto& span : next->owned) {
      span.clean_composited = true;
      span.edge_samples.assign(composed.value().begin() + static_cast<std::ptrdiff_t>(offset),
                               composed.value().begin() + static_cast<std::ptrdiff_t>(offset) + span.length);
      offset += static_cast<std::size_t>(span.length);
    }
  }
  counters_.composed_pixels += offset;
  counters_.retained_sample_bytes = offset * sizeof(std::array<float, 4>);
  ++counters_.content_builds;
  layers_ = std::move(global.value().ordered_layers);
  last_plan_.reset();
  current_ = next;
  return Result<CleanContentRef, Error>::success(current_);
}
} // namespace hdrshot
