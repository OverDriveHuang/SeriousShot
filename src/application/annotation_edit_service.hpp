#pragma once
#include "domain/annotation/annotation_render_plan.hpp"
#include "domain/geometry/selection_model.hpp"
namespace hdrshot {
struct AnnotationEditContext {
  SelectionSnapshot selection;
  AnnotationScale scale;
  TextRasterizerPort *text{};
  AnnotationCoverageCache *cache{};
};
struct PreparedAnnotationEdit {
  AnnotationDocument document;
  SelectionSnapshot selection;
  std::shared_ptr<const AnnotationRenderPlan> plan;
};
Result<PreparedAnnotationEdit, Error>
prepare_annotation_edit(const AnnotationDocument &,
                        const AnnotationRenderPlan *, const AnnotationCommand &,
                        const AnnotationEditContext &,
                        const ReusableAnnotationLayer * = nullptr);
Result<PreparedAnnotationEdit, Error>
prepare_selection_change(const AnnotationDocument &,
                         const AnnotationRenderPlan *, SelectionSnapshot,
                         const AnnotationEditContext &);
Result<std::shared_ptr<const AnnotationRenderPlan>, Error>
prepare_annotation_preview(const AnnotationDocument &,
                           const AnnotationRenderPlan *,
                           const AnnotationPreviewRequest &,
                           const AnnotationEditContext &);
} // namespace hdrshot
