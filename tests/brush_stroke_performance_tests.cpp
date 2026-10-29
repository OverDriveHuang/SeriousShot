#include "application/annotation_edit_service.hpp"
#include "domain/annotation/brush_stroke_builder.hpp"
#include "test_support.hpp"
using namespace hdrshot;
namespace {
template <class T> T take(Result<T, Error> value) {
  HDRSHOT_CHECK(bool(value));
  return std::move(value.value());
}
void incremental_counts_and_large_translation() {
  auto scale = take(AnnotationScale::from(1));
  BrushStrokeBuilder builder;
  HDRSHOT_CHECK(bool(builder.begin({1}, {0xFF4D67, 72}, {300, 300},
                                   {0, 0, 2400, 2400}, scale)));
  std::vector<StrokePoint> points;
  for (int y = 300; y < 1800; y += 80) {
    points.push_back({300., double(y)});
    points.push_back({1800., double(y)});
  }
  auto delta = take(builder.append(points));
  HDRSHOT_CHECK(bool(delta.segments_processed <= points.size()));
  auto count = builder.counters();
  auto prefix = take(builder.snapshot());
  auto repeat = take(builder.snapshot());
  HDRSHOT_CHECK(
      bool(builder.counters().sample_evaluations == count.sample_evaluations));
  HDRSHOT_CHECK(
      bool(builder.counters().copied_samples == count.copied_samples));
  HDRSHOT_CHECK(bool(builder.path_freezes() == 0));
  HDRSHOT_CHECK(builder.counters().frozen_rows == count.frozen_rows);
  for (std::size_t i = 0; i < prefix.layer.spans.size(); ++i)
    HDRSHOT_CHECK(prefix.layer.spans[i].coverage_u8.shares_storage(
        repeat.layer.spans[i].coverage_u8));
  auto live_doc = AnnotationDocument::empty();
  AnnotationEditContext live_context{{1, {0, 0, 2400, 2400}}, scale};
  for (int i = 0; i < 5; ++i) {
    auto active = take(
        prepare_annotation_preview(live_doc, nullptr, prefix, live_context));
    HDRSHOT_CHECK(active->ordered_layers.size() == 1);
    HDRSHOT_CHECK(builder.path_freezes() == 0);
  }
  auto completed = take(builder.finish(points.back()));
  HDRSHOT_CHECK(bool(builder.path_freezes() == 1));
  std::size_t bytes = 0;
  for (auto &span : prefix.layer.spans)
    bytes += span.coverage_u8.size();
  HDRSHOT_CHECK(bool(bytes < 1024 * 1024));
  HDRSHOT_CHECK(builder.counters().opaque_pixels_skipped > 0);
  HDRSHOT_CHECK(builder.counters().internal_samples_materialized == 0);
  std::size_t opaque_pixels=0;
  for (const auto& span:prefix.layer.spans) {
    if(span.is_opaque()) opaque_pixels+=span.length();
    else for(auto c:span.coverage_u8) HDRSHOT_CHECK(c>0 && c<255);
  }
  HDRSHOT_CHECK(opaque_pixels > 1024 * 1024);
  auto target = completed.object;
  target.brush_geometry->origin_x = 10;
  target.bounds.x += 10;
  AnnotationEditContext committed_context{{1, {0, 0, 2400, 2400}}, scale};
  auto committed = take(prepare_annotation_edit(
      AnnotationDocument::empty(), nullptr, CreateAnnotation{completed.object},
      committed_context, &completed.reusable));
  auto moved = take(prepare_annotation_edit(
      committed.document, committed.plan.get(),
      SetBrushGeometry{target.id, *target.brush_geometry}, committed_context));
  const auto &translated = moved.plan->ordered_layers.front();
  HDRSHOT_CHECK(translated.spans.size() == prefix.layer.spans.size());
  for (std::size_t i = 0; i < translated.spans.size(); ++i)
    HDRSHOT_CHECK(translated.spans[i].coverage_u8.shares_storage(
        prefix.layer.spans[i].coverage_u8));
  target.brush_geometry->scale_x = 1.1;
  auto resized = take(prepare_annotation_edit(
      moved.document, moved.plan.get(),
      SetBrushGeometry{target.id, *target.brush_geometry}, committed_context));
  const auto& resized_spans=resized.plan->ordered_layers.front().spans;
  auto old_aa=std::find_if(translated.spans.begin(),translated.spans.end(),[](const auto& s){return !s.is_opaque();});
  auto new_aa=std::find_if(resized_spans.begin(),resized_spans.end(),[](const auto& s){return !s.is_opaque();});
  HDRSHOT_CHECK(old_aa!=translated.spans.end() && new_aa!=resized_spans.end());
  HDRSHOT_CHECK(!new_aa->coverage_u8.shares_storage(old_aa->coverage_u8));
  auto doc = AnnotationDocument::empty();
  AnnotationEditContext context{{1, {0, 0, 2400, 2400}}, scale};
  for (int i = 0; i < 5; ++i) {
    auto preview =
        take(prepare_annotation_preview(doc, nullptr, repeat, context));
    HDRSHOT_CHECK(bool(preview->ordered_layers.size() == 1));
  }
  HDRSHOT_CHECK(bool(builder.path_freezes() == 1));
  builder.cancel();
  HDRSHOT_CHECK(bool(!prefix.layer.spans.empty()));
}
void existing_large_sample_payload_is_shared() {
  AnnotationRenderPlan plan{1,{2200,1200},{},{0,0,2200,1200},take(AnnotationScale::from(1))};
  AnnotationCoverageLayer layer{{3},AnnotationKind::text,{10,10,2048,1024},0xFF4D67,{},"large-existing-sample-layer"};
  for(int y=10;y<1034;++y) layer.spans.push_back({y,10,std::vector<std::uint8_t>(2048,128)});
  plan.ordered_layers.push_back(std::move(layer));
  auto rebased=take(AnnotationRenderPlanner::rebase(plan,{-5,-5,2210,1210}));
  std::size_t shared_bytes=0;
  for(std::size_t i=0;i<plan.ordered_layers[0].spans.size();++i) {
    const auto& a=plan.ordered_layers[0].spans[i];const auto& b=rebased.ordered_layers[0].spans[i];
    HDRSHOT_CHECK(a.coverage_u8.shares_storage(b.coverage_u8));shared_bytes+=a.coverage_u8.size();
  }
  HDRSHOT_CHECK(shared_bytes>1024*1024);
}

} // namespace
int main() {
  return test::run({{"large existing sample payload survives rebase without copying",existing_large_sample_payload_is_shared}, {"incremental counts and large shared translation",
                     incremental_counts_and_large_translation}});
}
