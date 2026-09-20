#pragma once

#include "core/error.hpp"
#include "core/geometry.hpp"
#include "core/ids.hpp"
#include "core/result.hpp"
#include "core/linear_source.hpp"
#include "domain/annotation/annotation_document.hpp"
#include "ports/text_rasterizer_port.hpp"

#include <cstdint>
#include <array>
#include <string>
#include <vector>

namespace hdrshot {

struct CoverageSpan {
  std::int32_t y{};
  std::int32_t x{};
  std::vector<std::uint8_t> coverage_u8;

  friend bool operator==(const CoverageSpan&, const CoverageSpan&) = default;
};

struct AnnotationCoverageLayer {
  ObjectId object_id{};
  AnnotationKind kind{AnnotationKind::rectangle};
  PixelRect bounds_px{};
  std::uint32_t color_srgb_rgb{};
  std::vector<CoverageSpan> spans;
  std::string raster_identity;

  friend bool operator==(const AnnotationCoverageLayer&, const AnnotationCoverageLayer&) = default;
};

struct AnnotationRenderPlan {
  DocumentRevision source_document_revision{};
  PixelSize output_size_px{};
  std::vector<AnnotationCoverageLayer> ordered_layers;
  PixelRect source_selection_rect_px{};

  friend bool operator==(const AnnotationRenderPlan&, const AnnotationRenderPlan&) = default;
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

class AnnotationRenderPlanner {
 public:
  [[nodiscard]] static Result<AnnotationRenderPlan, Error> build(
      const AnnotationDocumentSnapshot& annotations,
      PixelRect selection_rect_px,
      double pixels_per_point,
      TextRasterizerPort* text_rasterizer = nullptr);

  [[nodiscard]] static Result<AnnotationRenderPlan, Error> rebase(
      const AnnotationRenderPlan& render_plan,
      PixelRect selection_rect_px);

  [[nodiscard]] static Result<AnnotationCoverageLayer, Error> rasterize_shape(
      const AnnotationObject& object,
      PixelSize output_size_px);

  [[nodiscard]] static Result<AnnotationPixelPlan, Error> build_pixel_plan(
      const AnnotationRenderPlan& render_plan);
};

}  // namespace hdrshot
