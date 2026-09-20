#include "domain/output/source_range_probe.hpp"
#include "domain/annotation/annotation_pixel_plan_validator.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>

namespace hdrshot {
namespace {

Error probe_error(const ErrorCode code, const char* reason,
    std::source_location origin = std::source_location::current()) {
  return Error{code, "SourceRangeProbe", Retryability::never, {{"reason", reason}}, origin};
}

bool valid_span(const std::int32_t y,
                const std::int32_t x,
                const std::int32_t length,
                const PixelSize size) {
  return y >= 0 && y < size.height && x >= 0 && length > 0 &&
      static_cast<std::int64_t>(x) + static_cast<std::int64_t>(length) <=
          static_cast<std::int64_t>(size.width);
}

}  // namespace

Result<RangeFitResult, Error> SourceRangeProbe::probe(
    const CanonicalFrameView& frame,
    const AnnotationPixelPlan& pixel_plan,
    const RangeProbeOptimization optimization) {
  return probe(FrameCropper::view(frame), pixel_plan, optimization);
}

Result<RangeFitResult, Error> SourceRangeProbe::probe(
    const SelectionRoiView& frame,
    const AnnotationPixelPlan& pixel_plan,
    const RangeProbeOptimization optimization) {
  if (!frame.valid_storage() || frame.encoding.primaries != ColorPrimaries::display_p3 ||
      (frame.encoding.transfer != TransferFunction::extended_srgb &&
       frame.encoding.transfer != TransferFunction::linear) ||
      frame.encoding.source_reference_white_nits != 0.0) {
    return Result<RangeFitResult, Error>::failure(
        probe_error(ErrorCode::invalid_color_contract, "unsupported_source_encoding"));
  }
  if (frame.size_px != pixel_plan.output_size_px || frame.size_px.width <= 0 ||
      frame.size_px.height <= 0) {
    return Result<RangeFitResult, Error>::failure(
        probe_error(ErrorCode::state_inconsistent, "pixel_plan_size_mismatch"));
  }
  const auto width = static_cast<std::size_t>(frame.size_px.width);
  const auto height = static_cast<std::size_t>(frame.size_px.height);
  if (height > std::numeric_limits<std::size_t>::max() / width / 4U ||
      frame.row_stride_samples < width * 4U ||
      frame.first_sample_offset > frame.sample_count() ||
      (height > 0U &&
       (height - 1U) >
           (frame.sample_count() - frame.first_sample_offset) /
               frame.row_stride_samples) ||
      frame.first_sample_offset + (height - 1U) * frame.row_stride_samples +
              width * 4U >
          frame.sample_count()) {
    return Result<RangeFitResult, Error>::failure(
        probe_error(ErrorCode::invalid_input, "sample_count_mismatch"));
  }

  if (!AnnotationPixelPlanValidator::valid(pixel_plan)) {
    return Result<RangeFitResult, Error>::failure(
        probe_error(ErrorCode::state_inconsistent, "invalid_pixel_ownership"));
  }
  std::size_t source_pixel_count = 0U;
  for (const auto& span : pixel_plan.source_visible_spans) {
    if (!valid_span(span.y, span.x, span.length, frame.size_px)) {
      return Result<RangeFitResult, Error>::failure(
          probe_error(ErrorCode::state_inconsistent, "source_span_out_of_bounds"));
    }
    source_pixel_count += static_cast<std::size_t>(span.length);
  }
  std::size_t annotation_pixel_count = 0U;
  for (const auto& span : pixel_plan.annotation_owned_spans) {
    if (!valid_span(span.y, span.x, span.length, frame.size_px)) {
      return Result<RangeFitResult, Error>::failure(
          probe_error(ErrorCode::state_inconsistent, "annotation_span_out_of_bounds"));
    }
    annotation_pixel_count += static_cast<std::size_t>(span.length);
  }
  if (source_pixel_count + annotation_pixel_count != width * height) {
    return Result<RangeFitResult, Error>::failure(
        probe_error(ErrorCode::state_inconsistent, "pixel_plan_not_exhaustive"));
  }

  if (optimization.source_visible_within_sdr_guaranteed) {
    return Result<RangeFitResult, Error>::success(RangeFitResult{
        true,
        source_pixel_count,
        annotation_pixel_count,
        0U,
        "capture_contract_sdr_fast_path",
    });
  }

  if (frame.linear_source) {
    auto cpu = FrameCropper::read_cpu_region(frame);
    if (!cpu) return Result<RangeFitResult, Error>::failure(cpu.error());
    return probe(cpu.value(), pixel_plan, optimization);
  }

  std::size_t scanned_components = 0U;
  for (const auto& span : pixel_plan.source_visible_spans) {
    for (std::int32_t x = span.x; x < span.x + span.length; ++x) {
      const auto pixel = frame.first_sample_offset +
          static_cast<std::size_t>(span.y) * frame.row_stride_samples +
          static_cast<std::size_t>(x) * 4U;
      for (std::size_t channel = 0U; channel < 3U; ++channel) {
        const auto value = frame.sample(pixel + channel);
        ++scanned_components;
        if (!value) {
          return Result<RangeFitResult, Error>::failure(
              probe_error(ErrorCode::invalid_color_contract, "invalid_source_component"));
        }
        if (value.value() > 1.0F) {
          return Result<RangeFitResult, Error>::success(RangeFitResult{
              false,
              source_pixel_count,
              annotation_pixel_count,
              scanned_components,
              "source_visible_component_above_edr_one",
          });
        }
      }
    }
  }
  return Result<RangeFitResult, Error>::success(RangeFitResult{
      true,
      source_pixel_count,
      annotation_pixel_count,
      scanned_components,
      "source_visible_components_fit_edr_one",
  });
}

}  // namespace hdrshot
