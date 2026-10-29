#pragma once
#include <new>
namespace hdrshot {
// Deterministic owner-thread fault seam. Disabled in normal operation; no
// global allocator replacement and no cross-thread interference.
enum class AnnotationAllocationStage {
  none,
  dirty_row,
  sparse_insert,
  row_payload,
  row_publication,
  point_growth,
  finish_path,
  finish_proof,
  plan_events,
  plan_active,
  clean_snapshot,
  history_candidate,
  boundary_growth,
  aa_merge_growth
};
inline thread_local AnnotationAllocationStage annotation_allocation_failure =
    AnnotationAllocationStage::none;
inline void annotation_allocation_checkpoint(AnnotationAllocationStage stage) {
  if (annotation_allocation_failure == stage) {
    annotation_allocation_failure = AnnotationAllocationStage::none;
    throw std::bad_alloc{};
  }
}
} // namespace hdrshot
