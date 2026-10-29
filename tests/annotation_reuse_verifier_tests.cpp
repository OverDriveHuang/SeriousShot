#include "application/annotation_edit_service.hpp"
#include "domain/annotation/brush_stroke_builder.hpp"
#include "domain/annotation/annotation_geometry.hpp"
#include "test_support.hpp"
using namespace hdrshot;
namespace {
template<class T> T take(Result<T, Error> value) {
  HDRSHOT_CHECK(value.has_value());
  return std::move(value.value());
}
void rejects_stale_or_forged_brush_reuse(bool forge_revision) {
  const auto scale = take(AnnotationScale::from(1.25));
  AnnotationEditContext context{{1,{0,0,400,300}},scale};
  auto doc = AnnotationDocument::empty();
  auto empty = take(prepare_selection_change(doc,nullptr,context.selection,context));
  BrushStrokeBuilder b;
  take(b.begin({1},{0xFF4D67,48},{50,50},context.selection.desktop_rect,scale));
  auto prefix = take(b.snapshot());
  auto completed = take(b.finish({300,200}));
  ReusableAnnotationLayer stale{completed.object,prefix.layer,prefix.clip,prefix.scale,
      forge_revision ? completed.preview.trajectory_revision : prefix.trajectory_revision};
  auto prepared = prepare_annotation_edit(doc,empty.plan.get(),CreateAnnotation{completed.object},context,&stale);
  HDRSHOT_CHECK(!prepared);
  HDRSHOT_CHECK(doc.snapshot().objects.empty());
  HDRSHOT_CHECK(b.path_freezes()==1);
  HDRSHOT_CHECK(take(b.finish({300,200})).object.brush_geometry->path == completed.object.brush_geometry->path);
}
void rejects_same_count_different_path() {
  const auto scale = take(AnnotationScale::from(1));
  AnnotationEditContext context{{1,{0,0,400,300}},scale};
  auto doc = AnnotationDocument::empty();
  BrushStrokeBuilder a,b;
  take(a.begin({1},{0xFF4D67,48},{50,50},context.selection.desktop_rect,scale));
  take(b.begin({1},{0xFF4D67,48},{50,50},context.selection.desktop_rect,scale));
  auto first=take(a.finish({300,50})), second=take(b.finish({50,200}));
  HDRSHOT_CHECK(first.preview.trajectory_revision==second.preview.trajectory_revision);
  ReusableAnnotationLayer stale{second.object,first.preview.layer,first.preview.clip,first.preview.scale,first.preview.trajectory_revision};
  HDRSHOT_CHECK(!prepare_annotation_edit(doc,nullptr,CreateAnnotation{second.object},context,&stale));
}
void rejects_geometry_or_width_relabel() {
  const auto scale = take(AnnotationScale::from(1));
  AnnotationEditContext context{{1,{0,0,400,300}},scale};
  auto doc=AnnotationDocument::empty(); BrushStrokeBuilder b;
  take(b.begin({1},{0xFF4D67,48},{50,50},context.selection.desktop_rect,scale));
  auto completed=take(b.finish({200,150}));
  for (int variant=0;variant<3;++variant) {
    auto changed=completed.object;
    if (variant==0) changed.brush_geometry->origin_x=20;
    else if (variant==1) changed.brush_geometry->scale_x=1.25;
    else std::get<BrushStyle>(changed.style).diameter_dip=72;
    changed.bounds=take(brush_geometry_bounds(*changed.brush_geometry));
    ReusableAnnotationLayer stale{changed,completed.preview.layer,completed.preview.clip,completed.preview.scale,completed.preview.trajectory_revision};
    HDRSHOT_CHECK(!prepare_annotation_edit(doc,nullptr,CreateAnnotation{changed},context,&stale));
  }
}
void rejects_mismatched_previous_plan_color() {
  const auto scale=take(AnnotationScale::from(1));
  AnnotationEditContext context{{1,{0,0,400,300}},scale};
  auto doc=AnnotationDocument::empty(); BrushStrokeBuilder b;
  take(b.begin({1},{0xFF4D67,48},{50,50},context.selection.desktop_rect,scale));
  auto completed=take(b.finish({200,150}));
  auto committed=take(prepare_annotation_edit(doc,nullptr,CreateAnnotation{completed.object},context));
  auto stale=*committed.plan;
  stale.ordered_layers[0].color_srgb_rgb=0x111111;
  HDRSHOT_CHECK(!prepare_annotation_edit(committed.document,&stale,SelectAnnotation{{}},context));
}
void rejects_other_document_plan_same_revision_and_material() {
  const auto scale=take(AnnotationScale::from(1));
  AnnotationEditContext context{{1,{0,0,400,300}},scale};
  auto doc=AnnotationDocument::empty(); BrushStrokeBuilder a,b;
  take(a.begin({1},{0xFF4D67,48},{50,50},context.selection.desktop_rect,scale));
  take(b.begin({1},{0xFF4D67,48},{50,50},context.selection.desktop_rect,scale));
  auto left=take(a.finish({300,50})),right=take(b.finish({50,200}));
  auto first=take(prepare_annotation_edit(doc,nullptr,CreateAnnotation{left.object},context));
  auto second=take(prepare_annotation_edit(doc,nullptr,CreateAnnotation{right.object},context));
  HDRSHOT_CHECK(first.document.snapshot().revision==second.document.snapshot().revision);
  HDRSHOT_CHECK(!prepare_annotation_edit(second.document,first.plan.get(),SelectAnnotation{{}},context));
}
void real_proof_tampering_is_rejected() {
  const auto scale=take(AnnotationScale::from(1.25));
  AnnotationEditContext context{{1,{0,0,400,300}},scale};
  auto doc=AnnotationDocument::empty(); BrushStrokeBuilder b;
  take(b.begin({1},{0xFF4D67,48},{50,50},context.selection.desktop_rect,scale));
  auto completed=take(b.finish({200,150}));
  auto accepted=take(prepare_annotation_edit(doc,nullptr,CreateAnnotation{completed.object},context,&completed.reusable));
  HDRSHOT_CHECK(accepted.plan->ordered_layers[0]==completed.preview.layer);
  for (int mutation=0;mutation<15;++mutation) {
    auto bad=completed.reusable;
    switch(mutation) {
      case 0: ++bad.trajectory_revision; break;
      case 1: bad.object.brush_geometry->path=std::make_shared<const BrushPath>(bad.object.brush_geometry->path->points); break;
      case 2: bad.object.brush_geometry->origin_x+=1; bad.object.bounds=take(brush_geometry_bounds(*bad.object.brush_geometry)); break;
      case 3: std::get<BrushStyle>(bad.object.style).diameter_dip=72; break;
      case 4: std::get<BrushStyle>(bad.object.style).color_srgb_rgb=0x111111; bad.layer.color_srgb_rgb=0x111111; break;
      case 5: bad.layer.bounds_px.width--; break;
      case 6: bad.layer.raster_identity+="-changed"; break;
      case 7: bad.layer.spans.front().x++; break;
      case 8: bad.layer.spans.front().y++; break;
      case 9: { auto& samples=bad.layer.spans.front().coverage_u8; samples=CoverageSamples(std::vector<std::uint8_t>(samples.begin(),samples.end())); break; }
      case 10: { auto& samples=bad.layer.spans.front().coverage_u8; samples=samples.slice(0,samples.size()-1); break; }
      case 11: { auto& samples=bad.layer.spans.front().coverage_u8; samples=samples.slice(1,samples.size()-1); break; }
      case 12: { auto& samples=bad.layer.spans.front().coverage_u8; samples=samples.slice(1000000000,1000000000); break; }
      case 13: bad.clip.width--; break;
      case 14: bad.scale=take(AnnotationScale::from(2)); break;
    }
    HDRSHOT_CHECK(!prepare_annotation_edit(doc,nullptr,CreateAnnotation{bad.object},context,&bad));
    HDRSHOT_CHECK(doc.snapshot().objects.empty());
    HDRSHOT_CHECK(b.path_freezes()==1);
  }
  auto retried=take(b.finish({200,150}));
  HDRSHOT_CHECK(retried.object.brush_geometry->path==completed.object.brush_geometry->path);
  HDRSHOT_CHECK(take(prepare_annotation_edit(doc,nullptr,CreateAnnotation{retried.object},context,&retried.reusable)).document.snapshot().objects.size()==1);
}
void committed_plan_tampering_and_raw_plan_are_rejected() {
  const auto scale=take(AnnotationScale::from(1));
  AnnotationEditContext context{{1,{0,0,400,300}},scale};
  auto doc=AnnotationDocument::empty(); BrushStrokeBuilder b;
  take(b.begin({1},{0xFF4D67,48},{50,50},context.selection.desktop_rect,scale));
  auto completed=take(b.finish({200,150}));
  auto committed=take(prepare_annotation_edit(doc,nullptr,CreateAnnotation{completed.object},context));
  auto second=completed.object; second.id={2}; second.brush_geometry->origin_x=30;
  second.bounds=take(brush_geometry_bounds(*second.brush_geometry));
  committed=take(prepare_annotation_edit(committed.document,committed.plan.get(),CreateAnnotation{second},context));
  const auto original=committed.document.snapshot();
  for(int mutation=0;mutation<9;++mutation) {
    auto bad=*committed.plan;
    switch(mutation) {
      case 0: ++bad.source_document_revision; break;
      case 1: ++bad.source_selection_rect_px.x; break;
      case 2: ++bad.output_size_px.width; break;
      case 3: bad.scale=take(AnnotationScale::from(2)); break;
      case 4: std::swap(bad.ordered_layers[0],bad.ordered_layers[1]); break;
      case 5: bad.ordered_layers[0].bounds_px.width--; break;
      case 6: bad.ordered_layers[0].raster_identity+="-changed"; break;
      case 7: { auto& samples=bad.ordered_layers[0].spans.front().coverage_u8; samples=CoverageSamples(std::vector<std::uint8_t>(samples.begin(),samples.end())); break; }
      case 8: { auto& samples=bad.ordered_layers[0].spans.front().coverage_u8; samples=samples.slice(1000000000,1000000000); break; }
    }
    HDRSHOT_CHECK(!prepare_annotation_edit(committed.document,&bad,SelectAnnotation{{}},context));
    HDRSHOT_CHECK(committed.document.snapshot()==original);
  }
  AnnotationRenderPlan raw{committed.plan->source_document_revision,committed.plan->output_size_px,
      committed.plan->ordered_layers,committed.plan->source_selection_rect_px,committed.plan->scale};
  HDRSHOT_CHECK((AnnotationRenderPlanner::build_pixel_plan(raw)).has_value());
  HDRSHOT_CHECK(!prepare_annotation_edit(committed.document,&raw,SelectAnnotation{{}},context));
  auto invalid_raw=raw;
  auto& invalid_samples=invalid_raw.ordered_layers[0].spans.front().coverage_u8;
  invalid_samples=invalid_samples.slice(1000000000,1000000000);
  HDRSHOT_CHECK(!AnnotationRenderPlanner::build_pixel_plan(invalid_raw));
  AnnotationRenderPlan raw_empty{0,{},{},{},scale};
  AnnotationEditContext empty_context{{1,{}},scale};
  HDRSHOT_CHECK(!prepare_selection_change(doc,&raw_empty,{2,{}},empty_context));
  HDRSHOT_CHECK((prepare_selection_change(doc,nullptr,{2,{}},empty_context)).has_value());
}
void rebase_and_transient_never_become_committed_proof() {
  const auto scale=take(AnnotationScale::from(1));
  AnnotationEditContext context{{1,{10,10,100,80}},scale};
  auto doc=AnnotationDocument::empty(); BrushStrokeBuilder b;
  take(b.begin({1},{0xFF4D67,48},{10,30},context.selection.desktop_rect,scale));
  auto completed=take(b.finish({10,70}));
  auto committed=take(prepare_annotation_edit(doc,nullptr,CreateAnnotation{completed.object},context));
  auto same_rebase=take(AnnotationRenderPlanner::rebase(*committed.plan,context.selection.desktop_rect));
  HDRSHOT_CHECK(same_rebase==*committed.plan);
  HDRSHOT_CHECK((AnnotationRenderPlanner::build_pixel_plan(same_rebase)).has_value());
  HDRSHOT_CHECK(!prepare_annotation_edit(committed.document,&same_rebase,SelectAnnotation{{}},context));
  auto expanded_rebase=take(AnnotationRenderPlanner::rebase(*committed.plan,{0,0,120,100}));
  HDRSHOT_CHECK((AnnotationRenderPlanner::build_pixel_plan(expanded_rebase)).has_value());
  HDRSHOT_CHECK(!prepare_selection_change(committed.document,&expanded_rebase,{2,{0,0,120,100}},context));
  auto proper=take(prepare_selection_change(committed.document,committed.plan.get(),{2,{0,0,120,100}},context));
  auto full=take(AnnotationRenderPlanner::build(committed.document.snapshot(),{0,0,120,100},1));
  HDRSHOT_CHECK(*proper.plan==full);
  auto preview=take(prepare_annotation_preview(committed.document,committed.plan.get(),ObjectPreview{PreviewIntent::replace,completed.object,{}},context));
  HDRSHOT_CHECK(*preview==*committed.plan);
  HDRSHOT_CHECK(!prepare_annotation_edit(committed.document,preview.get(),SelectAnnotation{{}},context));
}
void positive_origin_transform_and_material_changes_keep_coverage() {
  const auto scale=take(AnnotationScale::from(1));
  AnnotationEditContext context{{1,{0,0,400,300}},scale};
  auto doc=AnnotationDocument::empty(); BrushStrokeBuilder b;
  take(b.begin({1},{0xFF4D67,48},{80,80},context.selection.desktop_rect,scale));
  auto completed=take(b.finish({200,150}));
  auto committed=take(prepare_annotation_edit(doc,nullptr,CreateAnnotation{completed.object},context,&completed.reusable));
  auto selected=take(prepare_annotation_edit(committed.document,committed.plan.get(),SelectAnnotation{{}},context));
  auto moved=completed.object; moved.brush_geometry->origin_x+=10; moved.bounds=take(brush_geometry_bounds(*moved.brush_geometry));
  auto preview=take(prepare_annotation_preview(selected.document,selected.plan.get(),ObjectPreview{PreviewIntent::replace,moved,completed.reusable},context));
  HDRSHOT_CHECK(preview->ordered_layers[0].spans.front().coverage_u8.shares_storage(committed.plan->ordered_layers[0].spans.front().coverage_u8));
  auto transformed=take(prepare_annotation_edit(selected.document,selected.plan.get(),SetBrushGeometry{moved.id,*moved.brush_geometry},context));
  auto styled=take(prepare_annotation_edit(transformed.document,transformed.plan.get(),RestyleAnnotation{moved.id,BrushStyle{0x25C06D,48}},context));
  auto candidate=styled.document.snapshot().objects[0]; candidate.brush_geometry->origin_x+=10; candidate.bounds=take(brush_geometry_bounds(*candidate.brush_geometry));
  auto next_preview=take(prepare_annotation_preview(styled.document,styled.plan.get(),ObjectPreview{PreviewIntent::replace,candidate,{}},context));
  HDRSHOT_CHECK(next_preview->ordered_layers[0].spans.front().coverage_u8.shares_storage(styled.plan->ordered_layers[0].spans.front().coverage_u8));
  HDRSHOT_CHECK(!prepare_annotation_preview(styled.document,styled.plan.get(),ObjectPreview{PreviewIntent::replace,candidate,completed.reusable},context));
  auto undone=take(prepare_annotation_edit(styled.document,styled.plan.get(),UndoAnnotation{},context));
  auto redone=take(prepare_annotation_edit(undone.document,undone.plan.get(),RedoAnnotation{},context));
  HDRSHOT_CHECK(redone.document.snapshot().objects==styled.document.snapshot().objects);
  HDRSHOT_CHECK(*redone.plan==take(AnnotationRenderPlanner::build(redone.document.snapshot(),context.selection.desktop_rect,1)));
}

}
int main() {
  return test::run({
    {"reject old prefix with final object", []{rejects_stale_or_forged_brush_reuse(false);}},
    {"reject old prefix with relabeled final revision", []{rejects_stale_or_forged_brush_reuse(true);}},
    {"reject equal length revision with different path", rejects_same_count_different_path},
    {"reject relabeled geometry and brush diameter", rejects_geometry_or_width_relabel},
    {"reject previous plan inconsistent material", rejects_mismatched_previous_plan_color},
    {"reject other document plan with same revision and material", rejects_other_document_plan_same_revision_and_material},
    {"true builder proof rejects all public field mutations",real_proof_tampering_is_rejected},
    {"committed plan proof rejects mutation and raw plan",committed_plan_tampering_and_raw_plan_are_rejected},
    {"rebase and transient lack committed authority",rebase_and_transient_never_become_committed_proof},
    {"positive origin move restyle undo redo",positive_origin_transform_and_material_changes_keep_coverage}
  });
}
