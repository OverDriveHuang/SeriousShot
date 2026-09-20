#pragma once

#include "core/error.hpp"
#include "core/result.hpp"
#include "domain/annotation/annotation_document.hpp"
#include "domain/annotation/annotation_render_plan.hpp"
#include "domain/frame/frame_cropper.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hdrshot {

struct LuminanceClipManifest {
  std::size_t clipped_pixel_count{};
  std::size_t clipped_channel_count{};

  friend bool operator==(const LuminanceClipManifest&, const LuminanceClipManifest&) = default;
};

struct RenderedSelection {
  FrameId source_frame_id{};
  SelectionRevision source_selection_revision{};
  DocumentRevision source_document_revision{};
  PixelSize size_px{};
  ColorEncoding encoding{
      ColorPrimaries::display_p3,
      TransferFunction::pq,
      AlphaMode::opaque,
      203.0,
  };
  std::vector<std::uint16_t> rgba_png_u16;
  LuminanceClipManifest luminance_clip{};
};

class AnnotationRenderer {
 public:
  [[nodiscard]] static Result<RenderedSelection, Error> render(
      const CanonicalFrameView& frame,
      const AnnotationRenderPlan& plan,
      double target_diffuse_white_nits = 203.0);

  // Convenience boundary for isolated tests and callers that do not already own
  // a preview plan. Production preview/export should pass the same immutable plan.
  [[nodiscard]] static Result<RenderedSelection, Error> render(
      const CanonicalFrameView& frame,
      const AnnotationDocumentSnapshot& annotations,
      double target_diffuse_white_nits = 203.0,
      TextRasterizerPort* text_rasterizer = nullptr);
};

}  // namespace hdrshot
