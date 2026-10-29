#include "application/annotation_edit_service.hpp"
#include "domain/annotation/annotation_geometry.hpp"
#include "domain/annotation/brush_stroke_builder.hpp"
#include "test_support.hpp"
using namespace hdrshot;
namespace {
template <class T> T take(Result<T, Error> value) {
  HDRSHOT_CHECK(bool(value));
  return std::move(value.value());
}
class FailingText final : public TextRasterizerPort {
public:
  Result<TextCoverageMask, Error>
  rasterize(const TextRasterRequest &) override {
    return Result<TextCoverageMask, Error>::failure(
        {ErrorCode::font_unavailable, "test", Retryability::never, {}});
  }
};
void failure_and_clip_transactions() {
  auto scale = take(AnnotationScale::from(1));
  auto doc = AnnotationDocument::empty();
  AnnotationCoverageCache cache;
  FailingText text;
  AnnotationEditContext context{{1, {10, 10, 100, 80}}, scale, &text, &cache};
  auto empty =
      take(prepare_selection_change(doc, nullptr, context.selection, context));
  AnnotationObject word{{2},
                        AnnotationKind::text,
                        {20, 20, 30, 20},
                        TextStyle{0x111111, 20},
                        "字"};
  auto before = doc.snapshot();
  HDRSHOT_CHECK(!prepare_annotation_edit(doc, empty.plan.get(),
                                         CreateAnnotation{word}, context));
  HDRSHOT_CHECK(doc.snapshot() == before);
  HDRSHOT_CHECK(empty.plan->ordered_layers.empty());
  BrushStrokeBuilder builder;
  HDRSHOT_CHECK(builder
                    .begin({3}, {0xFF4D67, 48}, {10, 30},
                           context.selection.desktop_rect, scale)
                    .has_value());
  auto completed = take(builder.finish({10, 70}));
  auto committed = take(prepare_annotation_edit(
      doc, empty.plan.get(), CreateAnnotation{completed.object}, context));
  auto expanded =
      take(prepare_selection_change(committed.document, committed.plan.get(),
                                    {2, {0, 0, 120, 100}}, context));
  auto full = take(AnnotationRenderPlanner::build(committed.document.snapshot(),
                                                  {0, 0, 120, 100}, 1));
  HDRSHOT_CHECK(*expanded.plan == full);
  HDRSHOT_CHECK(expanded.plan->ordered_layers[0].bounds_px.x == 0);
  HDRSHOT_CHECK(!prepare_selection_change(committed.document,
                                          committed.plan.get(),
                                          {3, {11, 10, 99, 80}}, context));
  auto changed = completed.object;
  changed.brush_geometry->scale_y = 1.25;
  auto geometry = take(brush_geometry_bounds(*changed.brush_geometry));
  changed.bounds = geometry;
  HDRSHOT_CHECK(changed.brush_geometry->path ==
                completed.object.brush_geometry->path);
}

void transaction_and_preview_contract() {
  auto doc = AnnotationDocument::empty();
  AnnotationEditContext context{{1, {0, 0, 400, 300}},
                                take(AnnotationScale::from(1.25))};
  auto empty =
      take(prepare_selection_change(doc, nullptr, context.selection, context));
  BrushStrokeBuilder builder;
  HDRSHOT_CHECK(
      bool(builder.begin({1}, {0xFF4D67, 48}, {50.2, 50.4},
                         context.selection.desktop_rect, context.scale)));
  auto first = take(builder.snapshot());
  auto p1 =
      take(prepare_annotation_preview(doc, empty.plan.get(), first, context));
  StrokePoint points[]{{70.1, 50.2}, {70.8, 90.5}};
  HDRSHOT_CHECK(bool(builder.append(points)));
  auto second = take(builder.snapshot());
  auto p2 =
      take(prepare_annotation_preview(doc, empty.plan.get(), second, context));
  HDRSHOT_CHECK(
      bool(p1->ordered_layers.size() == 1 && p2->ordered_layers.size() == 1));
  HDRSHOT_CHECK(bool(doc.snapshot().objects.empty()));
  HDRSHOT_CHECK(bool(builder.path_freezes() == 0));
  auto invalid = second;
  invalid.clip.width--;
  HDRSHOT_CHECK(bool(
      !prepare_annotation_preview(doc, empty.plan.get(), invalid, context)));
  invalid = second;
  invalid.layer.object_id = {9};
  HDRSHOT_CHECK(bool(
      !prepare_annotation_preview(doc, empty.plan.get(), invalid, context)));
  auto completed = take(builder.finish(points[1]));
  auto reuse = completed.reusable;
  auto bad = reuse;
  bad.clip.height--;
  HDRSHOT_CHECK(bool(!prepare_annotation_edit(
      doc, empty.plan.get(), CreateAnnotation{completed.object}, context,
      &bad)));
  HDRSHOT_CHECK(bool(doc.snapshot().objects.empty()));
  HDRSHOT_CHECK(
      bool(take(builder.finish(points[1])).object.brush_geometry->path ==
           completed.object.brush_geometry->path));
  auto committed = take(prepare_annotation_edit(
      doc, empty.plan.get(), CreateAnnotation{completed.object}, context,
      &reuse));
  HDRSHOT_CHECK(bool(committed.document.snapshot().objects.size() == 1));
  HDRSHOT_CHECK(bool(!prepare_annotation_preview(
      committed.document, committed.plan.get(), second, context)));
  auto stale = *committed.plan;
  stale.source_document_revision++;
  HDRSHOT_CHECK(bool(!prepare_annotation_edit(committed.document, &stale,
                                              SelectAnnotation{{}}, context)));
  AnnotationObject line{{2},
                        AnnotationKind::line,
                        {100, 100, 50, 1},
                        ShapeStyle{0x36A3FF, 4},
                        {},
                        ArrowGeometry{{100, 100}, {150, 100}}};
  auto preview = take(prepare_annotation_preview(
      committed.document, committed.plan.get(),
      ObjectPreview{PreviewIntent::create, line, {}}, context));
  HDRSHOT_CHECK(bool(preview->ordered_layers.size() == 2));
  HDRSHOT_CHECK(bool(!prepare_annotation_preview(
      committed.document, committed.plan.get(),
      ObjectPreview{PreviewIntent::replace, line, {}}, context)));
  auto moved = completed.object;
  moved.brush_geometry->origin_x = 10;
  moved.bounds.x += 10;
  auto replaced = take(prepare_annotation_preview(
      committed.document, committed.plan.get(),
      ObjectPreview{PreviewIntent::replace, moved, {}}, context));
  HDRSHOT_CHECK(bool(replaced->ordered_layers.size() == 1));
  HDRSHOT_CHECK(
      bool(replaced->ordered_layers[0].spans.front().coverage_u8.shares_storage(
          committed.plan->ordered_layers[0].spans.front().coverage_u8)));
  auto with_origin = take(prepare_annotation_preview(
      committed.document, committed.plan.get(),
      ObjectPreview{PreviewIntent::replace, moved, reuse}, context));
  HDRSHOT_CHECK(*with_origin == *replaced);
  auto bad_origin = reuse;
  bad_origin.object.id = {99};
  HDRSHOT_CHECK(!prepare_annotation_preview(
      committed.document, committed.plan.get(),
      ObjectPreview{PreviewIntent::replace, moved, bad_origin}, context));
  auto unused = reuse;
  unused.object.id = {99};
  HDRSHOT_CHECK(
      !prepare_annotation_edit(committed.document, committed.plan.get(),
                               SelectAnnotation{{}}, context, &unused));
  auto cleared =
      take(prepare_selection_change(doc, empty.plan.get(), {2, {}}, context));
  HDRSHOT_CHECK(bool(cleared.selection.desktop_rect.empty()));
}
} // namespace
int main() {
  return test::run(
      {{"failure and clip transactions", failure_and_clip_transactions},
       {"atomic edit and path-free preview",
        transaction_and_preview_contract}});
}
