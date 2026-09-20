#pragma once

#include "core/error.hpp"
#include "core/frame.hpp"
#include "core/result.hpp"
#include "domain/annotation/annotation_render_plan.hpp"
#include "domain/frame/frame_cropper.hpp"
#include "domain/output/output_classifier.hpp"
#include "domain/color/hdr_pq_precision.hpp"
#include "domain/output/content_light_statistics.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hdrshot {

struct ProducedRgb16Row {
  std::int32_t y{};
  std::vector<std::uint16_t> rgb_u16;
  std::size_t clipped_pixel_count{};
  std::size_t clipped_channel_count{};
  ContentLightStatistics content_light{};
};

struct ProducedRgb16Image {
  PixelSize size_px{};
  ColorEncoding encoding{};
  std::vector<std::uint16_t> rgb_u16;
  std::size_t clipped_pixel_count{};
  std::size_t clipped_channel_count{};
  ContentLightStatistics content_light{};
};

class OutputRowProducer {
 public:
  [[nodiscard]] static Result<ProducedRgb16Row, Error> produce_row(
      const CanonicalFrameView& frame,
      const AnnotationPixelPlan& pixel_plan,
      const OutputPlan& output_plan,
      std::int32_t y,
      double target_diffuse_white_nits = 203.0,
      HdrPqPrecision precision = HdrPqPrecision::bits_16);

  [[nodiscard]] static Result<ProducedRgb16Row, Error> produce_row(
      const SelectionRoiView& frame,
      const AnnotationPixelPlan& pixel_plan,
      const OutputPlan& output_plan,
      std::int32_t y,
      double target_diffuse_white_nits = 203.0,
      HdrPqPrecision precision = HdrPqPrecision::bits_16);

  // Reference/test collector. Production export sends rows directly to the
  // streaming encoder and does not call this whole-image helper.
  [[nodiscard]] static Result<ProducedRgb16Image, Error> produce_all(
      const CanonicalFrameView& frame,
      const AnnotationPixelPlan& pixel_plan,
      const OutputPlan& output_plan,
      double target_diffuse_white_nits = 203.0,
      HdrPqPrecision precision = HdrPqPrecision::bits_16);

  [[nodiscard]] static Result<ProducedRgb16Image, Error> produce_all(
      const SelectionRoiView& frame,
      const AnnotationPixelPlan& pixel_plan,
      const OutputPlan& output_plan,
      double target_diffuse_white_nits = 203.0,
      HdrPqPrecision precision = HdrPqPrecision::bits_16);
};

}  // namespace hdrshot
