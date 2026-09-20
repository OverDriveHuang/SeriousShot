#pragma once
#include "domain/annotation/annotation_render_plan.hpp"

namespace hdrshot {
class AnnotationPixelPlanValidator {
 public:
  static bool valid(const AnnotationPixelPlan& plan);
  // For streaming consumers of a sorted plan: validate only this row, without
  // rescanning the entire image on every output row.
  static bool valid_row(const AnnotationPixelPlan& plan, std::int32_t y);
};
}  // namespace hdrshot
