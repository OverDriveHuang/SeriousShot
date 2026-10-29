#include "annotation_raster_reference.hpp"
#include "application/annotation_edit_service.hpp"
#include "domain/annotation/annotation_allocation_checkpoint.hpp"
#include "domain/annotation/annotation_compositing.hpp"
#include "domain/annotation/brush_stroke_builder.hpp"
#include "test_support.hpp"
using namespace hdrshot;
namespace {
template <class T> T take(Result<T, Error> value) {
  HDRSHOT_CHECK(value.has_value());
  return std::move(value.value());
}
void allocation_failure_is_atomic() {
  for (auto stage : {AnnotationAllocationStage::dirty_row,
                     AnnotationAllocationStage::sparse_insert,
                     AnnotationAllocationStage::boundary_growth,
                     AnnotationAllocationStage::aa_merge_growth,
                     AnnotationAllocationStage::row_payload,
                     AnnotationAllocationStage::row_publication,
                     AnnotationAllocationStage::point_growth}) {
    BrushStrokeBuilder builder;
    take(builder.begin({1}, {0xFF4D67, 24}, {30.2, 30.3}, {0, 0, 300, 200},
                       take(AnnotationScale::from(1))));
    auto before = take(builder.snapshot());
    auto counters = builder.counters();
    StrokePoint next[]{{70.2, 90.4}, {200.3, 140.1}};
    annotation_allocation_failure = stage;
    HDRSHOT_CHECK(!builder.append(next));
    HDRSHOT_CHECK(annotation_allocation_failure ==
                  AnnotationAllocationStage::none);
    auto after = take(builder.snapshot());
    HDRSHOT_CHECK(same_coverage_source(before.layer, after.layer));
    HDRSHOT_CHECK(before.trajectory_revision == after.trajectory_revision);
    HDRSHOT_CHECK(builder.counters().segments_processed ==
                  counters.segments_processed);
    HDRSHOT_CHECK(builder.counters().sample_evaluations ==
                  counters.sample_evaluations);
    HDRSHOT_CHECK(builder.counters().copied_samples == counters.copied_samples);
    HDRSHOT_CHECK(builder.counters().frozen_rows == counters.frozen_rows);
    HDRSHOT_CHECK(builder.counters().opaque_pixels_skipped ==
                  counters.opaque_pixels_skipped);
    take(builder.append(next));
  }
  for (auto stage : {AnnotationAllocationStage::finish_path,
                     AnnotationAllocationStage::finish_proof}) {
    BrushStrokeBuilder builder;
    take(builder.begin({1}, {0xFF4D67, 24}, {30.2, 30.3}, {0, 0, 300, 200},
                       take(AnnotationScale::from(1))));
    auto before = take(builder.snapshot());
    annotation_allocation_failure = stage;
    HDRSHOT_CHECK(!builder.finish({160.2, 120.4}));
    HDRSHOT_CHECK(annotation_allocation_failure ==
                  AnnotationAllocationStage::none);
    HDRSHOT_CHECK(!builder.sealed() && builder.path_freezes() == 0);
    HDRSHOT_CHECK(
        same_coverage_source(before.layer, take(builder.snapshot()).layer));
    auto completed = take(builder.finish({160.2, 120.4}));
    HDRSHOT_CHECK(completed.reusable.has_valid_provenance());
    HDRSHOT_CHECK(builder.path_freezes() == 1);
    const auto original = AnnotationDocument::empty();
    annotation_allocation_failure =
        AnnotationAllocationStage::history_candidate;
    HDRSHOT_CHECK(!AnnotationDocument::apply(
        original, CreateAnnotation{completed.object}));
    HDRSHOT_CHECK(original.snapshot().objects.empty() &&
                  !original.snapshot().undo_available);
    HDRSHOT_CHECK(take(AnnotationDocument::apply(
                           original, CreateAnnotation{completed.object}))
                      .snapshot()
                      .undo_available);
  }
}
void opaque_and_aa_over_opaque() {
  AnnotationRenderPlan p{1,
                         {6, 1},
                         {{{1},
                           AnnotationKind::brush,
                           {0, 0, 6, 1},
                           0xFF4D67,
                           {CoverageSpan::opaque(0, 0, 6)},
                           "opaque"},
                          {{2},
                           AnnotationKind::brush,
                           {1, 0, 3, 1},
                           0x36A3FF,
                           {{0, 1, {64, 128, 192}}},
                           "aa"}},
                         {0, 0, 6, 1},
                         take(AnnotationScale::from(1))};
  auto pixels = take(AnnotationRenderPlanner::build_pixel_plan(p));
  HDRSHOT_CHECK(pixels.annotation_owned_spans.size() == 3);
  const auto &edge = pixels.annotation_owned_spans[1];
  HDRSHOT_CHECK(edge.edge_samples.size() == 3);
  for (auto sample : edge.edge_samples)
    HDRSHOT_CHECK(sample[3] == 0);
  HDRSHOT_CHECK(edge.edge_samples[0] != edge.edge_samples[1]);
  HDRSHOT_CHECK(pixels.annotation_owned_spans.front().edge_samples.empty());
  auto forged = CoverageSpan::opaque(0, 0, 4);
  forged.coverage_u8 = {1};
  HDRSHOT_CHECK(!forged.valid_view());
  HDRSHOT_CHECK(!CoverageSpan::opaque(0, 0, 4).slice(3, 2).valid_view());
}
void opaque_provenance_cannot_be_relabelled() {
  BrushStrokeBuilder builder;
  take(builder.begin({1}, {0xFF4D67, 24}, {30, 30}, {0, 0, 100, 100},
                     take(AnnotationScale::from(1))));
  const auto completed = take(builder.finish({30, 30}));
  HDRSHOT_CHECK(completed.reusable.has_valid_provenance());
  auto changed = completed.reusable;
  auto opaque =
      std::find_if(changed.layer.spans.begin(), changed.layer.spans.end(),
                   [](const auto &span) { return span.is_opaque(); });
  HDRSHOT_CHECK(opaque != changed.layer.spans.end());
  --opaque->opaque_length;
  HDRSHOT_CHECK(!changed.has_valid_provenance());
  changed = completed.reusable;
  opaque = std::find_if(changed.layer.spans.begin(), changed.layer.spans.end(),
                        [](const auto &span) { return span.is_opaque(); });
  opaque->coverage_u8 = std::vector<std::uint8_t>(opaque->length(), 255);
  opaque->opaque_length = 0;
  HDRSHOT_CHECK(!changed.has_valid_provenance());
}

void event_sweep_matches_frozen_scalar_and_fails_atomically() {
  AnnotationRenderPlan plan{
      7,
      {18, 4},
      {{{90},
        AnnotationKind::brush,
        {0, 0, 18, 3},
        0xFF4D67,
        {CoverageSpan::opaque(0, 0, 6),
         {0, 6, {64, 128, 255, 0}},
         CoverageSpan::opaque(0, 10, 3),
         CoverageSpan::opaque(2, 0, 18)},
        "base"},
       {{2},
        AnnotationKind::text,
        {0, 0, 18, 2},
        0x36A3FF,
        {{0, 1, {0, 64, 128, 192, 255, 254, 1, 0, 128, 255, 0, 64}},
         CoverageSpan::opaque(0, 13, 5),
         {1, 0, {1, 64, 128, 192, 254, 255}}},
        "text"},
       {{1},
        AnnotationKind::brush,
        {0, 0, 18, 3},
        0x25C06D,
        {CoverageSpan::opaque(0, 0, 2),
         {0, 2, {1, 254}},
         CoverageSpan::opaque(0, 4, 2),
         {2,
          0,
          {1, 254, 128, 64, 192, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}}},
        "top"}},
      {50, 60, 18, 4},
      take(AnnotationScale::from(1))};
  const auto before = plan.ordered_layers;
  auto expected = take(test_reference::build_pixel_plan(plan));
  auto actual = take(AnnotationRenderPlanner::build_pixel_plan(plan));
  HDRSHOT_CHECK(AnnotationPixelPlanValidator::valid(actual));
  const auto expand = [](const AnnotationPixelPlan &p) {
    std::vector<std::pair<ObjectId, std::array<float, 4>>> result(
        72, {ObjectId{}, {0, 0, 0, 1}});
    for (const auto &span : p.annotation_owned_spans)
      for (int i = 0; i < span.length; ++i)
        result[std::size_t(span.y) * 18 + std::size_t(span.x + i)] = {
            span.object_id, annotation_linear_sample(span, std::size_t(i))};
    return result;
  };
  auto a = expand(actual), b = expand(expected);
  for (std::size_t i = 0; i < a.size(); ++i) {
    HDRSHOT_CHECK(a[i].first == b[i].first);
    for (std::size_t c = 0; c < 4; ++c)
      HDRSHOT_CHECK_NEAR(a[i].second[c], b[i].second[c], 1e-6);
  }
  HDRSHOT_CHECK(a[4].first == ObjectId{1});
  for (auto stage : {AnnotationAllocationStage::plan_events,
                     AnnotationAllocationStage::plan_active}) {
    annotation_allocation_failure = stage;
    HDRSHOT_CHECK(!AnnotationRenderPlanner::build_pixel_plan(plan));
    HDRSHOT_CHECK(annotation_allocation_failure ==
                  AnnotationAllocationStage::none);
    HDRSHOT_CHECK(plan.ordered_layers == before);
    HDRSHOT_CHECK(
        expand(take(AnnotationRenderPlanner::build_pixel_plan(plan))) == a);
  }
}

void stress_width_distance_oracle_with_negative_clip() {
  const PixelRect clip{-230, -210, 640, 600};
  const std::vector<StrokeSegment> segments{{{-60.3, -40.7}, {90.2, 75.1}},
                                            {{90.2, 75.1}, {-50.6, 70.8}},
                                            {{-50.6, 70.8}, {-50.6, 70.8}}};
  for (double scale : {1., 1.25, 1.5, 2.})
    for (double base_radius : {72., 96.}) {
      const double radius = base_radius * scale;
      BrushCoverageAccumulator accumulator;
      take(accumulate_brush_segments(accumulator, segments, radius, clip));
      auto layer = freeze_brush_coverage(accumulator, {1}, 0xFF4D67, clip);
      HDRSHOT_CHECK(
          std::any_of(layer.spans.begin(), layer.spans.end(),
                      [](const auto &span) { return !span.is_opaque(); }));
      std::vector<std::uint8_t> actual(std::size_t(clip.width) * clip.height);
      for (const auto &span : layer.spans)
        for (std::size_t i = 0; i < span.length(); ++i)
          actual[std::size_t(span.y - clip.y) * clip.width +
                 std::size_t(span.x - clip.x) + i] = span.coverage_at(i);
      // Full image/all-segment oracle, independent of capsule/interval helpers.
      for (int y = clip.y; y < clip.bottom(); ++y)
        for (int x = clip.x; x < clip.right(); ++x) {
          double nearest = INFINITY;
          for (auto segment : segments) {
            double dx = segment.end.x - segment.start.x,
                   dy = segment.end.y - segment.start.y;
            double length = dx * dx + dy * dy;
            double t = length > 0
                           ? std::clamp(((x + .5 - segment.start.x) * dx +
                                         (y + .5 - segment.start.y) * dy) /
                                            length,
                                        0., 1.)
                           : 0.;
            nearest = std::min(nearest,
                               std::hypot(x + .5 - segment.start.x - t * dx,
                                          y + .5 - segment.start.y - t * dy));
          }
          auto expected = std::uint8_t(
              std::lround(std::clamp(radius + .5 - nearest, 0., 1.) * 255));
          HDRSHOT_CHECK(actual[std::size_t(y - clip.y) * clip.width +
                               std::size_t(x - clip.x)] == expected);
        }
    }
}

void copied_samples_counts_retained_aa_merge_and_payload() {
  BrushCoverageAccumulator accumulator;
  StrokeSegment point{{.5, .5}, {.5, .5}};
  const auto first = take(accumulate_brush_segments(
      accumulator, std::span(&point, 1), .25, {0, 0, 1, 1}));
  const auto old =
      freeze_brush_coverage(accumulator, {1}, 0xFF4D67, {0, 0, 1, 1});
  const auto again = take(accumulate_brush_segments(
      accumulator, std::span(&point, 1), .25, {0, 0, 1, 1}));
  HDRSHOT_CHECK(first.copied_samples == 1); // one immutable AA payload value
  HDRSHOT_CHECK(again.copied_samples ==
                3); // dirty clone, retained merge, payload
  HDRSHOT_CHECK(old.spans.front().coverage_at(0) == 191);
  HDRSHOT_CHECK(
      freeze_brush_coverage(accumulator, {1}, 0xFF4D67, {0, 0, 1, 1}) == old);
}

void old_stress_widths_remain_kernel_inputs() {
  for (double radius : {72., 96.}) {
    BrushCoverageAccumulator accumulator;
    StrokeSegment segment{{.2, .3}, {190.4, 120.2}};
    auto delta = take(accumulate_brush_segments(
        accumulator, std::span(&segment, 1), radius, {0, 0, 256, 256}));
    HDRSHOT_CHECK(delta.opaque_pixels_skipped > 0 &&
                  delta.internal_samples_materialized == 0);
    auto layer =
        freeze_brush_coverage(accumulator, {1}, 0xFF4D67, {0, 0, 256, 256});
    for (const auto &span : layer.spans)
      if (!span.is_opaque())
        for (auto c : span.coverage_u8)
          HDRSHOT_CHECK(c > 0 && c < 255);
  }
}
} // namespace
int main() {
  return test::run(
      {{"allocation failure preserves builder and history",
        allocation_failure_is_atomic},
       {"AA above opaque preserves varying RGB with zero source weight",
        opaque_and_aa_over_opaque},
       {"opaque mode and length remain sealed provenance",
        opaque_provenance_cannot_be_relabelled},
       {"event sweep agrees with frozen scalar across event boundaries",
        event_sweep_matches_frozen_scalar_and_fails_atomically},
       {"144 and 192 four-scale independent oracle and negative clip",
        stress_width_distance_oracle_with_negative_clip},
       {"logical AA copy counters include retained merge work",
        copied_samples_counts_retained_aa_merge_and_payload},
       {"144 and 192 physical diameter stress stays available",
        old_stress_widths_remain_kernel_inputs}});
}
