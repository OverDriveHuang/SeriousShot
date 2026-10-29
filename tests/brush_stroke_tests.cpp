#include "domain/annotation/annotation_geometry.hpp"
#include "domain/annotation/annotation_render_plan.hpp"
#include "domain/annotation/brush_stroke_builder.hpp"
#include "test_support.hpp"
#include <cmath>
#include <limits>
using namespace hdrshot;
namespace {
template <class T> T take(Result<T, Error> value) {
  HDRSHOT_CHECK(bool(value));
  return std::move(value.value());
}
std::vector<std::uint8_t> pixels(const AnnotationCoverageLayer &layer, int w,
                                 int h) {
  std::vector<std::uint8_t> result(std::size_t(w) * h);
  for (auto &span : layer.spans)
    for (std::size_t i = 0; i < span.length(); ++i)
      result[std::size_t(span.y) * w + span.x + i] = span.coverage_at(i);
  return result;
}
void oracle_and_immutable_prefixes() {
  for (double factor : {1., 1.25, 1.5, 2.})
    for (auto diameter : {4, 48, 72}) {
      auto scale = take(AnnotationScale::from(factor));
      BrushStrokeBuilder builder;
      const PixelRect clip{0, 0, 160, 120};
      std::vector<StrokePoint> points{
          {0, 0}, {150.4, 2.1}, {80.2, 100.9}, {20.2, 20.3}, {159.7, 119.8}};
      HDRSHOT_CHECK(bool(builder.begin({1}, {0xFF4D67, std::uint16_t(diameter)},
                                       points.front(), clip, scale)));
      auto old = take(builder.snapshot());
      auto old_pixels = pixels(old.layer, 160, 120);
      HDRSHOT_CHECK(bool(builder.append(std::span(points).subspan(1))));
      HDRSHOT_CHECK(bool(pixels(old.layer, 160, 120) == old_pixels));
      HDRSHOT_CHECK(bool(builder.path_freezes() == 0));
      auto completed = take(builder.finish(points.back()));
      HDRSHOT_CHECK(bool(builder.path_freezes() == 1));
      auto repeated = take(builder.finish(points.back()));
      HDRSHOT_CHECK(bool(completed.object.brush_geometry->path ==
                         repeated.object.brush_geometry->path));
      HDRSHOT_CHECK(bool(!builder.append(std::span(points).first(1))));
      auto full = take(AnnotationRenderPlanner::rasterize_shape(
          completed.object, {160, 120}, scale));
      auto got = pixels(full, 160, 120);
      HDRSHOT_CHECK(bool(got == pixels(completed.preview.layer, 160, 120)));
      // Independent slow distance oracle: full image, all segments, no
      // production helper.
      for (int y = 0; y < 120; ++y)
        for (int x = 0; x < 160; ++x) {
          double nearest = INFINITY;
          for (std::size_t i = 1; i < points.size(); ++i) {
            auto a = points[i - 1], b = points[i];
            double dx = b.x - a.x, dy = b.y - a.y;
            double t = std::clamp(((x + .5 - a.x) * dx + (y + .5 - a.y) * dy) /
                                      (dx * dx + dy * dy),
                                  0., 1.);
            nearest = std::min(nearest, std::hypot(x + .5 - a.x - t * dx,
                                                   y + .5 - a.y - t * dy));
          }
          auto expected = std::uint8_t(std::lround(
              std::clamp(diameter * factor * .5 + .5 - nearest, 0., 1.) * 255));
          HDRSHOT_CHECK(bool(got[std::size_t(y) * 160 + x] == expected));
        }
    }
}
void model_history_and_boundaries() {
  auto scale = take(AnnotationScale::from(1.5));
  BrushStrokeBuilder builder;
  HDRSHOT_CHECK(bool(
      builder.begin({7}, {0x111111, 48}, {100, 80}, {0, 0, 100, 80}, scale)));
  auto completed = take(builder.finish({100, 80}));
  auto object = completed.object;
  HDRSHOT_CHECK(
      bool(brush_centerline_inside(*object.brush_geometry, {0, 0, 100, 80})));
  HDRSHOT_CHECK(
      bool(AnnotationRenderPlanner::rasterize_shape(object, {100, 80}, scale)));
  auto doc = take(AnnotationDocument::apply(AnnotationDocument::empty(),
                                            CreateAnnotation{object}));
  auto before = doc.snapshot();
  auto moved = *object.brush_geometry;
  moved.origin_x = -.25;
  moved.origin_y = -.75;
  auto next =
      take(AnnotationDocument::apply(doc, SetBrushGeometry{object.id, moved}));
  HDRSHOT_CHECK(bool(next.snapshot().objects.front() != object));
  HDRSHOT_CHECK(bool(next.snapshot().objects.front().brush_geometry->path ==
                     object.brush_geometry->path));
  auto undone = take(AnnotationDocument::apply(next, UndoAnnotation{}));
  HDRSHOT_CHECK(bool(undone.snapshot().objects == before.objects));
  auto redone = take(AnnotationDocument::apply(undone, RedoAnnotation{}));
  HDRSHOT_CHECK(bool(redone.snapshot().objects == next.snapshot().objects));
  HDRSHOT_CHECK(bool(!AnnotationDocument::apply(
      doc, ResizeAnnotation{object.id, {1, 1, 20, 20}})));
  moved.scale_x = INFINITY;
  HDRSHOT_CHECK(bool(
      !AnnotationDocument::apply(doc, SetBrushGeometry{object.id, moved})));
  HDRSHOT_CHECK(bool(doc.snapshot() == before));
  auto styled = take(AnnotationDocument::apply(
      doc, RestyleAnnotation{object.id, BrushStyle{0x36A3FF, 72}}));
  HDRSHOT_CHECK(styled.snapshot().objects.front().brush_geometry->path ==
                object.brush_geometry->path);
  auto unstyled = take(AnnotationDocument::apply(styled, UndoAnnotation{}));
  HDRSHOT_CHECK(unstyled.snapshot().objects == before.objects);
  auto deleted =
      take(AnnotationDocument::apply(styled, DeleteAnnotation{object.id}));
  HDRSHOT_CHECK(deleted.snapshot().objects.empty());
  HDRSHOT_CHECK(take(AnnotationDocument::apply(deleted, UndoAnnotation{}))
                    .snapshot()
                    .objects == styled.snapshot().objects);
  HDRSHOT_CHECK(bool(!AnnotationScale::from(0)));
  HDRSHOT_CHECK(bool(!AnnotationScale::from(NAN)));
  HDRSHOT_CHECK(bool(!to_capture_px(std::numeric_limits<double>::max(),
                                    take(AnnotationScale::from(2)))));
}
void extreme_scale_is_safe() {
  auto scale = take(AnnotationScale::from(1));
  BrushStrokeBuilder builder;
  HDRSHOT_CHECK(
      builder.begin({9}, {0x111111, 4}, {20, 20}, {0, 0, 100, 100}, scale)
          .has_value());
  auto stroke = take(builder.finish({80, 80}));
  auto object = stroke.object;
  object.brush_geometry->scale_x = 1e-300;
  object.brush_geometry->scale_y = 1e-300;
  object.bounds = take(brush_geometry_bounds(*object.brush_geometry));
  HDRSHOT_CHECK(brush_hit_test(object, {1, 1}, 9, scale));
  AnnotationObject line{{10},
                        AnnotationKind::line,
                        {10, 10, 50, 1},
                        ShapeStyle{0xFF4D67, 4},
                        {},
                        ArrowGeometry{{10, 10}, {60, 10}}};
  auto huge = take(AnnotationScale::from(std::numeric_limits<double>::max()));
  HDRSHOT_CHECK(
      !AnnotationRenderPlanner::rasterize_shape(line, {100, 100}, huge));
  AnnotationCoverageCache cache;
  HDRSHOT_CHECK(!cache.get(line, {100, 100}, huge));
}
void invalid_model_inputs_are_atomic() {
  auto document = AnnotationDocument::empty();
  const auto before = document.snapshot();
  AnnotationObject object{
      {42}, AnnotationKind::brush, {0, 0, 1, 1}, BrushStyle{0x111111, 48}};
  for (auto points :
       {std::vector<StrokePoint>{}, std::vector<StrokePoint>{{NAN, 0}},
        std::vector<StrokePoint>{{0, INFINITY}},
        std::vector<StrokePoint>{{1e100, 0}}}) {
    object.brush_geometry =
        BrushGeometry{std::make_shared<const BrushPath>(std::move(points))};
    HDRSHOT_CHECK(
        !AnnotationDocument::apply(document, CreateAnnotation{object}));
    HDRSHOT_CHECK(document.snapshot() == before);
  }
  object.brush_geometry = BrushGeometry{
      std::make_shared<const BrushPath>(std::vector<StrokePoint>{{0, 0}})};
  object.style = ShapeStyle{0x111111, 4};
  HDRSHOT_CHECK(!AnnotationDocument::apply(document, CreateAnnotation{object}));
  object.style = BrushStyle{0x111111, 48};
  object.brush_geometry->origin_x = std::numeric_limits<double>::max();
  HDRSHOT_CHECK(!AnnotationDocument::apply(document, CreateAnnotation{object}));
  AnnotationObject arrow{{43},
                         AnnotationKind::arrow,
                         {0, 0, 10, 1},
                         ShapeStyle{0x111111, 4},
                         {},
                         ArrowGeometry{{std::numeric_limits<int>::min(), 0},
                                       {std::numeric_limits<int>::max(), 0}}};
  HDRSHOT_CHECK(!AnnotationDocument::apply(document, CreateAnnotation{arrow}));
  HDRSHOT_CHECK(document.snapshot() == before);
}

void line_history_contract() {
  AnnotationObject line{{91},
                        AnnotationKind::line,
                        {20, 20, 40, 1},
                        ShapeStyle{0xFF4D67, 4},
                        {},
                        ArrowGeometry{{20, 20}, {60, 20}}};
  auto document = take(AnnotationDocument::apply(AnnotationDocument::empty(),
                                                 CreateAnnotation{line}));
  auto created = document.snapshot();
  document = take(AnnotationDocument::apply(
      document, SetArrowGeometry{line.id, {{20, 20}, {60, 40}}}));
  auto resized = document.snapshot();
  document = take(
      AnnotationDocument::apply(document, MoveAnnotation{line.id, {5, 6}}));
  auto moved = document.snapshot();
  document = take(AnnotationDocument::apply(
      document, RestyleAnnotation{line.id, ShapeStyle{0x25C06D, 6}}));
  auto styled = document.snapshot();
  document =
      take(AnnotationDocument::apply(document, DeleteAnnotation{line.id}));
  HDRSHOT_CHECK(document.snapshot().objects.empty());
  for (const auto &expected : {styled, moved, resized, created}) {
    document = take(AnnotationDocument::apply(document, UndoAnnotation{}));
    HDRSHOT_CHECK(document.snapshot().objects == expected.objects);
  }
  document = take(AnnotationDocument::apply(document, UndoAnnotation{}));
  HDRSHOT_CHECK(document.snapshot().objects.empty());
  document = take(AnnotationDocument::apply(document, RedoAnnotation{}));
  HDRSHOT_CHECK(document.snapshot().objects == created.objects);
}

void invalid_clip_endpoints_are_rejected() {
  const auto scale = take(AnnotationScale::from(1));
  const std::vector<PixelRect> clips{{1000000000, 0, 2000000000, 1},
                                     {0, 1000000000, 1, 2000000000},
                                     {std::numeric_limits<int>::max(), 0, 1, 1},
                                     {0, std::numeric_limits<int>::max(), 1, 1},
                                     {0, 0, -1, 10}};
  BrushCoverageAccumulator checked;
  StrokeSegment point{{.5, .5}, {.5, .5}};
  take(accumulate_brush_segments(checked, std::span(&point, 1), 1,
                                 {0, 0, 1, 1}));
  const auto old = freeze_brush_coverage(checked, {1}, 0x111111, {0, 0, 1, 1});
  const auto counters = checked.counters;
  const auto huge = std::numeric_limits<double>::max();
  for (auto extreme : {StrokeSegment{{-huge, .5}, {huge, .5}},
                       StrokeSegment{{0, 0}, {1e200, 1e200}}}) {
    StrokeSegment batch[]{point, extreme};
    HDRSHOT_CHECK(!accumulate_brush_segments(checked, batch, 1, {0, 0, 1, 1}));
    HDRSHOT_CHECK(freeze_brush_coverage(checked, {1}, 0x111111, {0, 0, 1, 1}) ==
                  old);
    HDRSHOT_CHECK(checked.counters.segments_processed ==
                  counters.segments_processed);
    HDRSHOT_CHECK(checked.counters.sample_evaluations ==
                  counters.sample_evaluations);
  }
  const StrokeSegment remote{
      {1e169, 1e169},
      {std::nextafter(1e169, INFINITY), std::nextafter(1e169, -INFINITY)}};
  HDRSHOT_CHECK(!accumulate_brush_segments(checked, std::span(&remote, 1),
                                           1e169, {0, 0, 1, 1}));
  HDRSHOT_CHECK(freeze_brush_coverage(checked, {1}, 0x111111, {0, 0, 1, 1}) ==
                old);
  for (auto clip : clips) {
    BrushStrokeBuilder builder;
    HDRSHOT_CHECK(!builder.begin({1}, {0x111111, 4}, {1e9, 0}, clip, scale));
    BrushCoverageAccumulator accumulator;
    StrokeSegment segment{{0, 0}, {1, 1}};
    HDRSHOT_CHECK(!accumulate_brush_segments(accumulator,
                                             std::span(&segment, 1), 2, clip));
    HDRSHOT_CHECK(
        freeze_brush_coverage(accumulator, {1}, 0x111111, clip).spans.empty());
    HDRSHOT_CHECK(!AnnotationRenderPlanner::build(
        AnnotationDocument::empty().snapshot(), clip, 1));
    HDRSHOT_CHECK(!update_annotation_render_plan({}, {}, nullptr, clip, scale,
                                                 nullptr, nullptr));
    AnnotationRenderPlan raw{0, {10, 10}, {}, {0, 0, 10, 10}, scale};
    HDRSHOT_CHECK(!AnnotationRenderPlanner::rebase(raw, clip));
  }
}

void line_has_no_arrowhead() {
  auto scale = take(AnnotationScale::from(2));
  AnnotationObject line{{1},
                        AnnotationKind::line,
                        {20, 20, 80, 1},
                        ShapeStyle{0xFF4D67, 4},
                        {},
                        ArrowGeometry{{20, 20}, {100, 20}}};
  auto layer =
      take(AnnotationRenderPlanner::rasterize_shape(line, {140, 80}, scale));
  auto got = pixels(layer, 140, 80);
  for (int y = 0; y < 80; ++y)
    for (int x = 0; x < 140; ++x) {
      double px = std::clamp(x + .5, 20., 100.);
      auto value = std::uint8_t(std::lround(
          std::clamp(4.5 - std::hypot(x + .5 - px, y + .5 - 20), 0., 1.) *
          255));
      HDRSHOT_CHECK(bool(got[std::size_t(y) * 140 + x] == value));
    }
}
} // namespace
int main() {
  return test::run(
      {{"brush oracle and immutable prefixes", oracle_and_immutable_prefixes},
       {"brush history and boundaries", model_history_and_boundaries},
       {"line no arrowhead", line_has_no_arrowhead},
       {"clip endpoints reject before int32 arithmetic",
        invalid_clip_endpoints_are_rejected},
       {"line create transform style delete history", line_history_contract},
       {"extreme legal geometry and rejected physical scale",
        extreme_scale_is_safe},
       {"invalid model inputs remain atomic",
        invalid_model_inputs_are_atomic}});
}
