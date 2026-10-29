#pragma once

#include "core/error.hpp"
#include "core/geometry.hpp"
#include "core/ids.hpp"
#include "core/linear_source.hpp"
#include "core/result.hpp"
#include "domain/annotation/annotation_coverage.hpp"
#include "domain/annotation/annotation_document.hpp"
#include "ports/text_rasterizer_port.hpp"

#include <cstdint>
#include <array>
#include <string>
#include <vector>

namespace hdrshot {

enum class PreviewIntent { create, replace };
struct ObjectPreview {
  PreviewIntent intent;
  AnnotationObject object;
  std::optional<ReusableAnnotationLayer> origin;
};
using AnnotationPreviewRequest = std::variant<ObjectPreview, BrushPreview>;

struct AnnotationRenderPlan {
  DocumentRevision source_document_revision{};
  PixelSize output_size_px{};
  std::vector<AnnotationCoverageLayer> ordered_layers;
  PixelRect source_selection_rect_px{};
  AnnotationScale scale;

  AnnotationRenderPlan(DocumentRevision revision, PixelSize output,
                       std::vector<AnnotationCoverageLayer> layers,
                       PixelRect selection, AnnotationScale scale)
      : source_document_revision(revision), output_size_px(output),
        ordered_layers(std::move(layers)), source_selection_rect_px(selection),
        scale(scale) {}
  friend bool operator==(const AnnotationRenderPlan &a,
                         const AnnotationRenderPlan &b) {
    return a.source_document_revision == b.source_document_revision &&
           a.output_size_px == b.output_size_px &&
           a.ordered_layers == b.ordered_layers &&
           a.source_selection_rect_px == b.source_selection_rect_px &&
           a.scale == b.scale;
  }

private:
  struct Provenance;
  std::shared_ptr<const Provenance> provenance_;
  void bind_source(const AnnotationDocumentSnapshot &);
  bool matches_source(const AnnotationDocumentSnapshot &) const;
  friend class AnnotationRenderPlanner;
  friend class AnnotationPlanUpdater;
};

struct SourceVisibleSpan {
  std::int32_t y{};
  std::int32_t x{};
  std::int32_t length{};

  friend bool operator==(const SourceVisibleSpan&, const SourceVisibleSpan&) = default;
};

struct AnnotationOwnedSpan {
  std::int32_t y{};
  std::int32_t x{};
  std::int32_t length{};
  ObjectId object_id{};
  std::uint32_t color_srgb_rgb{};
  // Empty: fully covered constant color. Otherwise one sample per pixel:
  // premultiplied Linear Display P3 RGB, then remaining source weight.
  std::vector<std::array<float, 4>> edge_samples{};
  // Already composed clean Linear P3, potentially HDR. Requires weight=0;
  // remains annotation-owned for ADR-013 classification.
  bool clean_composited{};
  // Optional immutable native sparse sample buffer. Mutually exclusive with
  // edge_samples; coordinates and ownership are still shared-domain values.
  LinearSampleRef native_samples{};
  std::size_t native_sample_offset{};

  friend bool operator==(const AnnotationOwnedSpan&, const AnnotationOwnedSpan&) = default;
};

// Resolves layer ordering into mutually exclusive pixel spans.
// Every output pixel belongs to exactly one source-visible or annotation-owned
// span. Coverage samples remain in AnnotationRenderPlan for geometry/preview;
// Fully covered pixels skip hidden source. Partial edges retain its contribution.
struct AnnotationPixelPlan {
  DocumentRevision source_document_revision{};
  PixelSize output_size_px{};
  std::vector<SourceVisibleSpan> source_visible_spans;
  std::vector<AnnotationOwnedSpan> annotation_owned_spans;

  friend bool operator==(const AnnotationPixelPlan&, const AnnotationPixelPlan&) = default;
};

// Capture/editor-owned, bounded cache of coverage for integer translations.
// It stores no platform resources and must only be used on its owner's thread.
class AnnotationCoverageCache {
 public:
   [[nodiscard]] Result<AnnotationCoverageLayer, Error>
   get(const AnnotationObject &object, PixelSize output, AnnotationScale scale);
   void clear() { entries_.clear(); }

 private:
  struct Entry {
    AnnotationObject key;
    PixelRect clip;
    AnnotationCoverageLayer layer;
    int x{}, y{};
    AnnotationScale scale;
  };
  std::vector<Entry> entries_;
};

class AnnotationRenderPlanner {
 public:
   [[nodiscard]] static Result<AnnotationRenderPlan, Error>
   build(const AnnotationDocumentSnapshot &annotations,
         PixelRect selection_rect_px, double capture_pixels_per_dip,
         TextRasterizerPort *text_rasterizer = nullptr,
         AnnotationCoverageCache *coverage_cache = nullptr);

   [[nodiscard]] static Result<AnnotationRenderPlan, Error>
   rebase(const AnnotationRenderPlan &render_plan, PixelRect selection_rect_px);

   [[nodiscard]] static Result<AnnotationCoverageLayer, Error>
   rasterize_shape(const AnnotationObject &object, PixelSize output_size_px,
                   AnnotationScale scale);

   [[nodiscard]] static Result<AnnotationPixelPlan, Error>
   build_pixel_plan(const AnnotationRenderPlan &render_plan);
};

}  // namespace hdrshot

namespace hdrshot {
bool brush_clip_changed(const AnnotationObject &, PixelRect old_roi,
                        PixelRect new_roi, AnnotationScale);
Result<AnnotationRenderPlan, Error> update_annotation_render_plan(
    const AnnotationDocumentSnapshot &, const AnnotationDocumentSnapshot &,
    const AnnotationRenderPlan *, PixelRect, AnnotationScale,
    TextRasterizerPort *, AnnotationCoverageCache *,
    const ReusableAnnotationLayer * = nullptr);
Result<AnnotationRenderPlan, Error> prepare_annotation_preview_plan(
    const AnnotationDocumentSnapshot &, const AnnotationRenderPlan *,
    const AnnotationPreviewRequest &, PixelRect, AnnotationScale,
    TextRasterizerPort *, AnnotationCoverageCache *);
} // namespace hdrshot
