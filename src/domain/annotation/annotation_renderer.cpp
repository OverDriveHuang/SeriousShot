#include "domain/annotation/annotation_renderer.hpp"

#include "domain/output/output_row_producer.hpp"
#include "domain/color/pq_reference_white_mapper.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hdrshot {
namespace {

Error render_error(const ErrorCode code, const char* reason,
    std::source_location origin = std::source_location::current()) {
  return Error{code, "AnnotationRenderer", Retryability::never, {{"reason", reason}}, origin};
}

}  // namespace

Result<RenderedSelection, Error> AnnotationRenderer::render(
    const CanonicalFrameView& frame,
    const AnnotationRenderPlan& plan,
    const double target_diffuse_white_nits) {
  if (frame.encoding.primaries != ColorPrimaries::display_p3 ||
      (frame.encoding.transfer != TransferFunction::extended_srgb &&
       frame.encoding.transfer != TransferFunction::linear) ||
      frame.encoding.source_reference_white_nits != 0.0 ||
      !std::isfinite(frame.point_pixel_scale) || frame.point_pixel_scale <= 0.0 ||
      frame.size_px.width <= 0 || frame.size_px.height <= 0) {
    return Result<RenderedSelection, Error>::failure(render_error(
        ErrorCode::invalid_color_contract, "unsupported_frame_contract"));
  }
  if (plan.output_size_px != frame.size_px ||
      (!plan.source_selection_rect_px.empty() &&
       plan.source_selection_rect_px != frame.source_rect_px)) {
    return Result<RenderedSelection, Error>::failure(render_error(
        ErrorCode::state_inconsistent, "render_plan_selection_mismatch"));
  }

  const auto pixel_count = static_cast<std::size_t>(frame.size_px.width) *
      static_cast<std::size_t>(frame.size_px.height);
  if (frame.rgba_half.size() != pixel_count * 4U) {
    return Result<RenderedSelection, Error>::failure(render_error(
        ErrorCode::invalid_input, "sample_count_mismatch"));
  }

  auto pixel_plan = AnnotationRenderPlanner::build_pixel_plan(plan);
  if (!pixel_plan) {
    return Result<RenderedSelection, Error>::failure(pixel_plan.error());
  }
  RenderedSelection rendered;
  rendered.source_frame_id = frame.source_frame_id;
  rendered.source_selection_revision = frame.selection_revision;
  rendered.source_document_revision = plan.source_document_revision;
  rendered.size_px = frame.size_px;
  rendered.encoding.source_reference_white_nits = target_diffuse_white_nits;
  rendered.rgba_png_u16.resize(pixel_count * 4U);
  for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
    rendered.rgba_png_u16[pixel * 4U + 3U] = 65535U;
  }

  const OutputPlan output{OutputClass::hdr, EncodingIntent::hdr_pq, 16, "annotation"};
  const auto image = OutputRowProducer::produce_all(
      frame, pixel_plan.value(), output, target_diffuse_white_nits);
  if (!image) return Result<RenderedSelection, Error>::failure(image.error());
  for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
    for (std::size_t c = 0; c < 3U; ++c) {
      rendered.rgba_png_u16[pixel * 4U + c] = image.value().rgb_u16[pixel * 3U + c];
    }
  }
  rendered.luminance_clip.clipped_channel_count = image.value().clipped_channel_count;
  rendered.luminance_clip.clipped_pixel_count = image.value().clipped_pixel_count;
  return Result<RenderedSelection, Error>::success(std::move(rendered));
}

Result<RenderedSelection, Error> AnnotationRenderer::render(
    const CanonicalFrameView& frame,
    const AnnotationDocumentSnapshot& annotations,
    const double target_diffuse_white_nits,
    TextRasterizerPort* const text_rasterizer) {
  auto plan = AnnotationRenderPlanner::build(
      annotations,
      frame.source_rect_px,
      frame.point_pixel_scale,
      text_rasterizer);
  if (!plan) {
    return Result<RenderedSelection, Error>::failure(plan.error());
  }
  return render(frame, plan.value(), target_diffuse_white_nits);
}

}  // namespace hdrshot
