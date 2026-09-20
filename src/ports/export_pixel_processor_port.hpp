#pragma once

#include "core/error.hpp"
#include "core/frame.hpp"
#include "core/result.hpp"
#include "domain/annotation/annotation_render_plan.hpp"
#include "domain/annotation/annotation_renderer.hpp"
#include "domain/frame/frame_cropper.hpp"
#include "domain/output/output_classifier.hpp"
#include "domain/output/content_light_statistics.hpp"
#include "ports/export_ports.hpp"

#include <cstdint>
#include <vector>

namespace hdrshot {

struct ExportPixelProcessRequest {
  const SelectionRoiView* source{};
  const AnnotationPixelPlan* pixel_plan{};
  OutputPlan output_plan{};
  PqDiffuseWhite pq_diffuse_white{PqDiffuseWhite::nits_203};
  HdrPqPrecision hdr_pq_precision{HdrPqPrecision::bits_10};
};

struct ExportPixelProcessResult {
  PixelSize size_px{};
  ColorEncoding output_encoding{};
  std::vector<std::uint16_t> rgb_u16;
  LuminanceClipManifest luminance_clip{};
  std::optional<ContentLightStatistics> content_light;
};

class ExportPixelProcessorPort {
 public:
  virtual ~ExportPixelProcessorPort() = default;
  [[nodiscard]] virtual Result<ExportPixelProcessResult, Error> process(
      const ExportPixelProcessRequest& request) = 0;
};

}  // namespace hdrshot
