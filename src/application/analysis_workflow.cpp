#include "application/analysis_workflow.hpp"
#include "domain/annotation/annotation_pixel_plan_validator.hpp"
#include "domain/output/output_classifier.hpp"
#include <algorithm>

namespace hdrshot {
namespace {
Error failure(const char *reason) {
  return {ErrorCode::state_inconsistent,
          "AnalysisWorkflow",
          Retryability::never,
          {{"reason", reason}}};
}
ExportSnapshot compact_snapshot(const ExportSnapshot &original,
                                LinearSourceRef source,
                                std::vector<AnnotationOwnedSpan> owned = {}) {
  const auto size = source->size_px();
  const PixelRect rect{0, 0, size.width, size.height};
  auto desktop = std::make_shared<FrozenDesktop>();
  desktop->frame_id = original.frozen_desktop->frame_id;
  desktop->display_generation = original.frozen_desktop->display_generation;
  desktop->desktop_bounds_points = {0, 0, static_cast<double>(size.width),
                                    static_cast<double>(size.height)};
  CanonicalFrameSegment segment;
  segment.display_id = original.target_display_id;
  if (segment.display_id.value == 0)
    segment.display_id = DisplayId{1};
  segment.desktop_frame_points = desktop->desktop_bounds_points;
  segment.point_pixel_scale = 1;
  segment.size_px = size;
  segment.pixel_format = PixelFormat::rgba32_float;
  segment.encoding = {ColorPrimaries::display_p3, TransferFunction::linear,
                      AlphaMode::opaque, 0.0};
  // Actual source-visible pixels, not the display headroom, choose the class.
  segment.display_dynamic_range = DisplayDynamicRange::hdr;
  segment.linear_source = std::move(source);
  desktop->canonical_segments.push_back(std::move(segment));
  auto result = original;
  result.frozen_desktop = desktop;
  result.target_display_id = desktop->canonical_segments.front().display_id;
  result.selection = {original.selection.revision, rect};
  result.annotations.objects.clear();
  result.annotation_render_plan = std::make_shared<AnnotationRenderPlan>(
      AnnotationRenderPlan{result.annotations.revision, size, {}, rect});
  auto clean = std::make_shared<CleanContentSnapshot>();
  clean->source = desktop;
  clean->display_id = result.target_display_id;
  clean->content_revision = original.annotations.revision;
  clean->owned = std::move(owned);
  result.clean_content = std::move(clean);
  result.clean_document_revision = result.annotations.revision;
  return result;
}
} // namespace

Result<PreparedAnalysis, Error>
AnalysisWorkflow::prepare(const ExportSnapshot &snapshot,
                          AnalysisPort &executor,
                          SourceRangeProbePort *range_probe) {
  if (!snapshot.frozen_desktop || snapshot.selection.desktop_rect.empty())
    return Result<PreparedAnalysis, Error>::failure(
        failure("missing_selection_source"));
  auto view =
      snapshot.target_display_id.value == 0
          ? FrameCropper::view(*snapshot.frozen_desktop, snapshot.selection)
          : FrameCropper::view_display(*snapshot.frozen_desktop,
                                       snapshot.target_display_id,
                                       snapshot.selection);
  if (!view)
    return Result<PreparedAnalysis, Error>::failure(view.error());
  if (!snapshot.annotation_render_plan ||
      snapshot.annotation_render_plan->source_document_revision !=
          snapshot.annotations.revision ||
      snapshot.annotation_render_plan->source_selection_rect_px !=
          snapshot.selection.desktop_rect)
    return Result<PreparedAnalysis, Error>::failure(
        failure("stale_annotation_render_plan"));
  if (snapshot.clean_content &&
      (snapshot.clean_content->source != snapshot.frozen_desktop ||
       snapshot.clean_content->display_id != snapshot.target_display_id ||
       snapshot.clean_document_revision != snapshot.annotations.revision))
    return Result<PreparedAnalysis, Error>::failure(
        failure("stale_clean_content"));
  auto plan =
      snapshot.clean_content
          ? snapshot.clean_content->roi_plan(snapshot.selection.desktop_rect,
                                             snapshot.annotations.revision)
          : AnnotationRenderPlanner::build_pixel_plan(
                *snapshot.annotation_render_plan);
  if (!plan)
    return Result<PreparedAnalysis, Error>::failure(plan.error());
  if (!AnnotationPixelPlanValidator::valid(plan.value()))
    return Result<PreparedAnalysis, Error>::failure(
        failure("invalid_annotation_pixel_plan"));
  // Production Overlay supplies already-clean sparse replacements. Keeping them
  // preserves annotation ownership for original-output classification, without
  // retaining a full display or compositing partially-covered edges twice.
  if (std::any_of(plan.value().annotation_owned_spans.begin(),
                  plan.value().annotation_owned_spans.end(),
                  [](const auto &span) { return !span.clean_composited; }))
    return Result<PreparedAnalysis, Error>::failure(
        failure("clean_annotations_required"));
  const RangeProbeOptimization optimization{
      view.value().display_dynamic_range == DisplayDynamicRange::sdr};
  auto fit =
      range_probe
          ? range_probe->probe(view.value(), plan.value(), optimization)
          : SourceRangeProbe::probe(view.value(), plan.value(), optimization);
  if (!fit)
    return Result<PreparedAnalysis, Error>::failure(fit.error());
  auto classified = OutputClassifier::classify({fit.value()});
  if (!classified)
    return Result<PreparedAnalysis, Error>::failure(classified.error());
  auto clean = executor.prepare(view.value(), plan.value());
  if (!clean)
    return Result<PreparedAnalysis, Error>::failure(clean.error());
  if (!clean.value() || clean.value()->size_px() != view.value().size_px)
    return Result<PreparedAnalysis, Error>::failure(
        failure("prepared_source_shape_mismatch"));
  PreparedAnalysis result;
  result.input = {clean.value(), snapshot.operation_id.value,
                  classified.value().output_class == OutputClass::hdr};
  result.original = compact_snapshot(
      snapshot, clean.value(), std::move(plan.value().annotation_owned_spans));
  return Result<PreparedAnalysis, Error>::success(std::move(result));
}

Result<ExportSnapshot, Error>
AnalysisWorkflow::report_snapshot(const ExportSnapshot &original,
                                  LinearSourceRef composed) {
  if (!original.frozen_desktop || !composed || composed->size_px().width <= 0 ||
      composed->size_px().height <= 0)
    return Result<ExportSnapshot, Error>::failure(
        failure("invalid_composed_report"));
  return Result<ExportSnapshot, Error>::success(
      compact_snapshot(original, std::move(composed)));
}
} // namespace hdrshot
