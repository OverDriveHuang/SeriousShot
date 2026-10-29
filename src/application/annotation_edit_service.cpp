#include "application/annotation_edit_service.hpp"
namespace hdrshot {
Result<PreparedAnnotationEdit, Error> prepare_annotation_edit(
    const AnnotationDocument &document, const AnnotationRenderPlan *plan,
    const AnnotationCommand &command, const AnnotationEditContext &context,
    const ReusableAnnotationLayer *reuse) {
  auto next = AnnotationDocument::apply(document, command);
  if (!next)
    return Result<PreparedAnnotationEdit, Error>::failure(next.error());
  auto built = update_annotation_render_plan(
      document.snapshot(), next.value().snapshot(), plan,
      context.selection.desktop_rect, context.scale, context.text,
      context.cache, reuse);
  if (!built)
    return Result<PreparedAnnotationEdit, Error>::failure(built.error());
  return Result<PreparedAnnotationEdit, Error>::success(
      {std::move(next.value()), context.selection,
       std::make_shared<const AnnotationRenderPlan>(std::move(built.value()))});
}
Result<PreparedAnnotationEdit, Error> prepare_selection_change(
    const AnnotationDocument &document, const AnnotationRenderPlan *plan,
    SelectionSnapshot selection, const AnnotationEditContext &context) {
  auto built = update_annotation_render_plan(
      document.snapshot(), document.snapshot(), plan, selection.desktop_rect,
      context.scale, context.text, context.cache);
  if (!built)
    return Result<PreparedAnnotationEdit, Error>::failure(built.error());
  return Result<PreparedAnnotationEdit, Error>::success(
      {document, selection,
       std::make_shared<const AnnotationRenderPlan>(std::move(built.value()))});
}
Result<std::shared_ptr<const AnnotationRenderPlan>, Error>
prepare_annotation_preview(const AnnotationDocument &document,
                           const AnnotationRenderPlan *plan,
                           const AnnotationPreviewRequest &request,
                           const AnnotationEditContext &context) {
  auto built = prepare_annotation_preview_plan(
      document.snapshot(), plan, request, context.selection.desktop_rect,
      context.scale, context.text, context.cache);
  if (!built)
    return Result<std::shared_ptr<const AnnotationRenderPlan>, Error>::failure(
        built.error());
  return Result<std::shared_ptr<const AnnotationRenderPlan>, Error>::success(
      std::make_shared<const AnnotationRenderPlan>(std::move(built.value())));
}
} // namespace hdrshot
