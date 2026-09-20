#pragma once
#include "ports/clean_composition_port.hpp"
#include <mutex>

namespace hdrshot {
struct CleanContentSnapshot {
  FrozenDesktopRef source;
  DisplayId display_id{};
  std::uint64_t content_revision{};
  // Display-local coordinates, independent of selection and UI revisions.
  std::vector<AnnotationOwnedSpan> owned;
  [[nodiscard]] Result<AnnotationPixelPlan, Error> roi_plan(
      PixelRect selection, DocumentRevision revision) const;
};
using CleanContentRef = std::shared_ptr<const CleanContentSnapshot>;
struct CleanContentCounters {
  std::uint64_t content_builds{}, cache_hits{}, composed_pixels{};
  std::size_t retained_sample_bytes{};
};
// One cache per display session. Immutable returned revisions outlive the cache.
// Serialized construction cannot publish an out-of-order asynchronous result.
class CleanContentCache {
 public:
  explicit CleanContentCache(std::shared_ptr<CleanCompositionPort> executor = {});
  Result<CleanContentRef, Error> get(FrozenDesktopRef source, DisplayId display,
                                    const AnnotationRenderPlan& plan);
  Result<CleanContentRef, Error> get(FrozenDesktopRef source, DisplayId display,
      std::shared_ptr<const AnnotationRenderPlan> plan);
  CleanContentCounters counters() const;
 private:
  std::shared_ptr<CleanCompositionPort> executor_;
  mutable std::mutex mutex_;
  std::vector<AnnotationCoverageLayer> layers_;
  CleanContentRef current_;
  std::shared_ptr<const AnnotationRenderPlan> last_plan_;
  CleanContentCounters counters_;
};
} // namespace hdrshot
