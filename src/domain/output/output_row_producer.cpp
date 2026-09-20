#include "domain/output/output_row_producer.hpp"

#include "domain/color/extended_p3_mapper.hpp"
#include "domain/annotation/annotation_pixel_plan_validator.hpp"
#include "domain/annotation/annotation_compositing.hpp"
#include "domain/color/pq_reference_white_mapper.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace hdrshot {
namespace {

Error row_error(const ErrorCode code, const char* reason,
    std::source_location origin = std::source_location::current()) {
  return Error{code, "OutputRowProducer", Retryability::never, {{"reason", reason}}, origin};
}

template <typename Span>
auto first_span_for_row(const std::vector<Span>& spans, const std::int32_t y) {
  return std::lower_bound(
      spans.begin(), spans.end(), y,
      [](const Span& span, const std::int32_t row) { return span.y < row; });
}

}  // namespace

Result<ProducedRgb16Row, Error> OutputRowProducer::produce_row(
    const CanonicalFrameView& frame,
    const AnnotationPixelPlan& pixel_plan,
    const OutputPlan& output_plan,
    const std::int32_t y,
    const double target_diffuse_white_nits,
    const HdrPqPrecision precision) {
  return produce_row(
      FrameCropper::view(frame), pixel_plan, output_plan, y,
      target_diffuse_white_nits, precision);
}

Result<ProducedRgb16Row, Error> OutputRowProducer::produce_row(
    const SelectionRoiView& frame,
    const AnnotationPixelPlan& pixel_plan,
    const OutputPlan& output_plan,
    const std::int32_t y,
    const double target_diffuse_white_nits,
    const HdrPqPrecision precision) {
  const auto is_hdr = output_plan.encoding_intent == EncodingIntent::hdr_pq;
  if (!frame.valid_storage() || frame.encoding.primaries != ColorPrimaries::display_p3 ||
      (frame.encoding.transfer != TransferFunction::extended_srgb &&
       frame.encoding.transfer != TransferFunction::linear) ||
      frame.encoding.source_reference_white_nits != 0.0 ||
      (is_hdr &&
       (!std::isfinite(target_diffuse_white_nits) || target_diffuse_white_nits <= 0.0 ||
        !valid_hdr_pq_precision(precision)))) {
    return Result<ProducedRgb16Row, Error>::failure(
        row_error(ErrorCode::invalid_color_contract, "unsupported_source_encoding"));
  }
  if (frame.size_px != pixel_plan.output_size_px || y < 0 || y >= frame.size_px.height ||
      frame.size_px.width <= 0 || frame.size_px.height <= 0) {
    return Result<ProducedRgb16Row, Error>::failure(
        row_error(ErrorCode::state_inconsistent, "row_or_pixel_plan_mismatch"));
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
    return Result<ProducedRgb16Row, Error>::failure(
        row_error(ErrorCode::invalid_input, "sample_count_mismatch"));
  }

  if (!AnnotationPixelPlanValidator::valid_row(pixel_plan, y)) {
    return Result<ProducedRgb16Row, Error>::failure(
        row_error(ErrorCode::state_inconsistent, "pixel_plan_row_not_partitioned"));
  }
  for (auto it = first_span_for_row(pixel_plan.annotation_owned_spans, y);
       it != pixel_plan.annotation_owned_spans.end() && it->y == y; ++it)
    if (it->native_samples) return Result<ProducedRgb16Row, Error>::failure(
        row_error(ErrorCode::precondition_failed, "explicit_cpu_plan_materialization_required"));
  ProducedRgb16Row produced{y, std::vector<std::uint16_t>(width * 3U), 0U, 0U, {}};
  std::size_t written_pixels = 0U;
  auto source = first_span_for_row(pixel_plan.source_visible_spans, y);
  while (source != pixel_plan.source_visible_spans.end() && source->y == y) {
    if (source->x < 0 || source->length <= 0 ||
        source->x + source->length > frame.size_px.width) {
      return Result<ProducedRgb16Row, Error>::failure(
          row_error(ErrorCode::state_inconsistent, "source_span_out_of_bounds"));
    }
    for (std::int32_t x = source->x; x < source->x + source->length; ++x) {
      const auto pixel = frame.first_sample_offset +
          static_cast<std::size_t>(y) * frame.row_stride_samples +
          static_cast<std::size_t>(x) * 4U;
      auto pixel_clipped = false;
      double pixel_max_nits = 0.0;
      for (std::size_t channel = 0U; channel < 3U; ++channel) {
        const auto decoded = frame.sample(pixel + channel);
        if (!decoded) {
          return Result<ProducedRgb16Row, Error>::failure(decoded.error());
        }
        std::uint16_t encoded = 0U;
        bool clipped = false;
        if (output_plan.encoding_intent == EncodingIntent::hdr_pq) {
          const auto linear_edr = std::max(
              0.0F, ExtendedP3Mapper::source_linear(decoded.value(), frame.encoding.transfer));
          const auto mapped = PqReferenceWhiteMapper::map_linear_edr(
              static_cast<double>(linear_edr), target_diffuse_white_nits, precision);
          if (!mapped) {
            return Result<ProducedRgb16Row, Error>::failure(mapped.error());
          }
          encoded = mapped.value().png_u16;
          clipped = mapped.value().clipped;
          const auto fp32_nits = std::min(
              10000.0F,
              linear_edr * static_cast<float>(target_diffuse_white_nits));
          pixel_max_nits = std::max(
              pixel_max_nits, static_cast<double>(fp32_nits));
        } else {
          encoded = ExtendedP3Mapper::quantize_unorm16(
              frame.encoding.transfer == TransferFunction::linear
                  ? ExtendedP3Mapper::encode_extended_srgb(decoded.value())
                  : decoded.value());
        }
        produced.rgb_u16[static_cast<std::size_t>(x) * 3U + channel] = encoded;
        if (clipped) {
          ++produced.clipped_channel_count;
          pixel_clipped = true;
        }
      }
      if (pixel_clipped) {
        ++produced.clipped_pixel_count;
      }
      if (is_hdr) {
        accumulate_content_light(produced.content_light, pixel_max_nits);
      }
      ++written_pixels;
    }
    ++source;
  }

  auto annotation = first_span_for_row(pixel_plan.annotation_owned_spans, y);
  while (annotation != pixel_plan.annotation_owned_spans.end() && annotation->y == y) {
    if (annotation->x < 0 || annotation->length <= 0 ||
        annotation->x + annotation->length > frame.size_px.width) {
      return Result<ProducedRgb16Row, Error>::failure(
          row_error(ErrorCode::state_inconsistent, "annotation_span_out_of_bounds"));
    }
    if (!annotation->edge_samples.empty()) {
      for (std::int32_t i = 0; i < annotation->length; ++i) {
        const auto x = static_cast<std::size_t>(annotation->x + i);
        const auto input = frame.first_sample_offset +
            static_cast<std::size_t>(y) * frame.row_stride_samples + x * 4U;
        const auto resolved = resolve_annotation_pixel(
            annotation->edge_samples[static_cast<std::size_t>(i)],
            frame, input);
        if (!resolved) return Result<ProducedRgb16Row, Error>::failure(resolved.error());
        double maximum_nits = 0.0;
        bool clipped = false;
        for (std::size_t c = 0; c < 3U; ++c) {
          const float linear = resolved.value()[c];
          if (is_hdr) {
            const auto mapped = PqReferenceWhiteMapper::map_linear_edr(
                linear, target_diffuse_white_nits, precision);
            if (!mapped) return Result<ProducedRgb16Row, Error>::failure(mapped.error());
            produced.rgb_u16[x * 3U + c] = mapped.value().png_u16;
            produced.clipped_channel_count += mapped.value().clipped ? 1U : 0U;
            clipped = clipped || mapped.value().clipped;
            maximum_nits = std::max(maximum_nits, static_cast<double>(std::min(
                10000.0F, linear * static_cast<float>(target_diffuse_white_nits))));
          } else {
            // Classification deliberately excludes AA edges. SDR clips any
            // residual edge-only highlights, as specified by ADR-013.
            produced.rgb_u16[x * 3U + c] = ExtendedP3Mapper::quantize_unorm16(
                ExtendedP3Mapper::encode_extended_srgb(linear));
          }
        }
        produced.clipped_pixel_count += clipped ? 1U : 0U;
        if (is_hdr) accumulate_content_light(produced.content_light, maximum_nits);
        ++written_pixels;
      }
      ++annotation;
      continue;
    }
    const auto linear = ExtendedP3Mapper::annotation_linear_display_p3(
        annotation->color_srgb_rgb);
    std::array<std::uint16_t, 3> encoded{};
    double annotation_max_nits = 0.0;
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
      if (output_plan.encoding_intent == EncodingIntent::hdr_pq) {
        const auto mapped = PqReferenceWhiteMapper::map_linear_edr(
            linear[channel], target_diffuse_white_nits, precision);
        if (!mapped) {
          return Result<ProducedRgb16Row, Error>::failure(mapped.error());
        }
        encoded[channel] = mapped.value().png_u16;
        const auto fp32_nits = std::min(
            10000.0F,
            linear[channel] * static_cast<float>(target_diffuse_white_nits));
        annotation_max_nits = std::max(
            annotation_max_nits, static_cast<double>(fp32_nits));
      } else {
        encoded[channel] = ExtendedP3Mapper::quantize_unorm16(
            ExtendedP3Mapper::encode_extended_srgb(linear[channel]));
      }
    }
    for (std::int32_t x = annotation->x; x < annotation->x + annotation->length; ++x) {
      for (std::size_t channel = 0U; channel < 3U; ++channel) {
        produced.rgb_u16[static_cast<std::size_t>(x) * 3U + channel] = encoded[channel];
      }
      ++written_pixels;
      if (is_hdr) {
        accumulate_content_light(produced.content_light, annotation_max_nits);
      }
    }
    ++annotation;
  }
  if (written_pixels != width) {
    return Result<ProducedRgb16Row, Error>::failure(
        row_error(ErrorCode::state_inconsistent, "pixel_plan_row_not_exhaustive"));
  }
  return Result<ProducedRgb16Row, Error>::success(std::move(produced));
}

Result<ProducedRgb16Image, Error> OutputRowProducer::produce_all(
    const CanonicalFrameView& frame,
    const AnnotationPixelPlan& pixel_plan,
    const OutputPlan& output_plan,
    const double target_diffuse_white_nits,
    const HdrPqPrecision precision) {
  return produce_all(
      FrameCropper::view(frame), pixel_plan, output_plan,
      target_diffuse_white_nits, precision);
}

Result<ProducedRgb16Image, Error> OutputRowProducer::produce_all(
    const SelectionRoiView& frame,
    const AnnotationPixelPlan& pixel_plan,
    const OutputPlan& output_plan,
    const double target_diffuse_white_nits,
    const HdrPqPrecision precision) {
  if (frame.size_px.width <= 0 || frame.size_px.height <= 0) {
    return Result<ProducedRgb16Image, Error>::failure(
        row_error(ErrorCode::invalid_input, "empty_dimensions"));
  }
  if (!AnnotationPixelPlanValidator::valid(pixel_plan)) {
    return Result<ProducedRgb16Image, Error>::failure(
        row_error(ErrorCode::state_inconsistent, "pixel_plan_not_partitioned"));
  }
  const auto width = static_cast<std::size_t>(frame.size_px.width);
  const auto height = static_cast<std::size_t>(frame.size_px.height);
  if (height > std::numeric_limits<std::size_t>::max() / width / 3U) {
    return Result<ProducedRgb16Image, Error>::failure(
        row_error(ErrorCode::invalid_input, "size_overflow"));
  }
  ProducedRgb16Image image;
  image.size_px = frame.size_px;
  image.encoding = ColorEncoding{
      ColorPrimaries::display_p3,
      output_plan.encoding_intent == EncodingIntent::hdr_pq
          ? TransferFunction::pq
          : TransferFunction::srgb,
      AlphaMode::opaque,
      output_plan.encoding_intent == EncodingIntent::hdr_pq
          ? target_diffuse_white_nits
          : 0.0,
  };
  image.rgb_u16.resize(width * height * 3U);
  for (std::int32_t y = 0; y < frame.size_px.height; ++y) {
    auto row = produce_row(
        frame, pixel_plan, output_plan, y, target_diffuse_white_nits, precision);
    if (!row) {
      return Result<ProducedRgb16Image, Error>::failure(row.error());
    }
    const auto destination = static_cast<std::size_t>(y) * width * 3U;
    std::copy(row.value().rgb_u16.begin(), row.value().rgb_u16.end(),
              image.rgb_u16.begin() + static_cast<std::ptrdiff_t>(destination));
    image.clipped_pixel_count += row.value().clipped_pixel_count;
    image.clipped_channel_count += row.value().clipped_channel_count;
    image.content_light.max_cll_x10000 = std::max(
        image.content_light.max_cll_x10000,
        row.value().content_light.max_cll_x10000);
    image.content_light.luminance_sum_x10000 +=
        row.value().content_light.luminance_sum_x10000;
    image.content_light.pixel_count += row.value().content_light.pixel_count;
  }
  return Result<ProducedRgb16Image, Error>::success(std::move(image));
}

}  // namespace hdrshot
