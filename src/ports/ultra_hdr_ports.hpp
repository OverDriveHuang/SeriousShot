#pragma once

#include "core/error.hpp"
#include "core/frame.hpp"
#include "core/result.hpp"
#include "domain/annotation/annotation_render_plan.hpp"
#include "domain/frame/frame_cropper.hpp"
#include "ports/export_ports.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace hdrshot {

inline constexpr double kUltraHdrReferenceWhiteNits = 203.0;
inline constexpr std::int32_t kUltraHdrMaximumDimension = 16384;

struct UltraHdrInputRenderRequest {
  const SelectionRoiView* source{};
  const AnnotationPixelPlan* pixel_plan{};
  double reference_white_nits{kUltraHdrReferenceWhiteNits};
};

struct LinearDisplayP3HalfImage {
  // Native source encodings end at the renderer boundary. Both platforms
  // produce D65 P3 linear RGB, with 1.0 == 203 nit; alpha is padding fixed at 1.
  PixelSize size_px{};
  std::vector<std::uint16_t> rgba_half;
  double reference_white_nits{kUltraHdrReferenceWhiteNits};
  double maximum_linear_component{};
  // Classification only; absent for standalone callers without annotations.
  // The actual maximum above still drives HDR headroom, including AA edges.
  std::optional<double> source_visible_maximum_linear_component;
};

class UltraHdrInputRendererPort {
 public:
  virtual ~UltraHdrInputRendererPort() = default;
  [[nodiscard]] virtual Result<LinearDisplayP3HalfImage, Error> render(
      const UltraHdrInputRenderRequest& request) = 0;
};

struct UltraHdrEncodeRequest {
  const LinearDisplayP3HalfImage* linear_display_p3{};
  UltraHdrJpegQuality quality{UltraHdrJpegQuality::balanced};
  std::string_view software{};
};

enum class JpegOutputKind { display_p3_sdr, ultra_hdr };

// Called after validating a finite, nonnegative renderer range. Shared by
// encoder and application: a UI format choice is not proof of HDR content.
[[nodiscard]] inline JpegOutputKind jpeg_output_kind(double maximum_linear_component) {
  return maximum_linear_component <= 1.0
      ? JpegOutputKind::display_p3_sdr : JpegOutputKind::ultra_hdr;
}

[[nodiscard]] inline JpegOutputKind jpeg_output_kind(const LinearDisplayP3HalfImage& image) {
  return jpeg_output_kind(image.source_visible_maximum_linear_component.value_or(
      image.maximum_linear_component));
}

struct EncodedUltraHdrJpeg {
  std::vector<std::uint8_t> bytes;
  JpegOutputKind kind{JpegOutputKind::ultra_hdr};
};

class UltraHdrEncoderPort {
 public:
  virtual ~UltraHdrEncoderPort() = default;
  // JPEG base: P3/sRGB ICC. ISO gain application: base P3.
  // HDR alternate: explicit P3/PQ ICC; raw input remains linear P3.
  // Both profiles are shared across platforms. No BT.2020 intermediate.
  // HDR uses ISO 21496-1 + XMP, RGB gain with a common content-adaptive
  // two-pass range. Tone-map/gain strategy belongs behind this port; future HDR+SDR
  // encoding must not introduce platform branches in the shared workflow.
  // SDR-range input instead produces an ordinary JPEG with only P3/sRGB ICC.
  [[nodiscard]] virtual Result<EncodedUltraHdrJpeg, Error> encode(
      const UltraHdrEncodeRequest& request) = 0;
};

}  // namespace hdrshot
