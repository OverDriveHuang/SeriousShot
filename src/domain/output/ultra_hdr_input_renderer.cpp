#include "domain/output/ultra_hdr_input_renderer.hpp"

#include "domain/color/extended_p3_mapper.hpp"
#include "domain/annotation/annotation_pixel_plan_validator.hpp"
#include "domain/annotation/annotation_compositing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace hdrshot {
namespace {

Error render_error(const ErrorCode code, const char* reason,
    std::source_location origin = std::source_location::current()) {
  return Error{code, "CpuUltraHdrInputRenderer", Retryability::never,
               {{"reason", reason}}, origin};
}

template <typename Span>
auto first_span_for_row(const std::vector<Span>& spans, const std::int32_t y) {
  return std::lower_bound(
      spans.begin(), spans.end(), y,
      [](const Span& span, const std::int32_t row) { return span.y < row; });
}


}  // namespace

Result<LinearDisplayP3HalfImage, Error> CpuUltraHdrInputRenderer::render(
    const UltraHdrInputRenderRequest& request) {
  if (request.source == nullptr || request.pixel_plan == nullptr) {
    return Result<LinearDisplayP3HalfImage, Error>::failure(
        render_error(ErrorCode::invalid_input, "missing_request_input"));
  }
  const auto& source = *request.source;
  const auto& plan = *request.pixel_plan;
  if (source.linear_source || std::any_of(plan.annotation_owned_spans.begin(), plan.annotation_owned_spans.end(),
      [](const auto& s) { return static_cast<bool>(s.native_samples); })) {
    auto cpu_source = FrameCropper::read_cpu_region(source);
    if (!cpu_source) return Result<LinearDisplayP3HalfImage, Error>::failure(cpu_source.error());
    auto cpu_plan = materialize_annotation_plan(plan);
    if (!cpu_plan) return Result<LinearDisplayP3HalfImage, Error>::failure(cpu_plan.error());
    const auto view = FrameCropper::view(cpu_source.value());
    return render({&view, &cpu_plan.value(), request.reference_white_nits});
  }
  if (!source.valid_storage() || source.size_px.width <= 0 || source.size_px.height <= 0 ||
      source.size_px.width > kUltraHdrMaximumDimension ||
      source.size_px.height > kUltraHdrMaximumDimension ||
      source.size_px != plan.output_size_px ||
      source.encoding.primaries != ColorPrimaries::display_p3 ||
      (source.encoding.transfer != TransferFunction::extended_srgb &&
       source.encoding.transfer != TransferFunction::linear) ||
      source.encoding.source_reference_white_nits != 0.0 ||
      request.reference_white_nits != kUltraHdrReferenceWhiteNits) {
    return Result<LinearDisplayP3HalfImage, Error>::failure(
        render_error(ErrorCode::invalid_color_contract, "unsupported_source_contract"));
  }
  const auto width = static_cast<std::size_t>(source.size_px.width);
  const auto height = static_cast<std::size_t>(source.size_px.height);
  if (width > std::numeric_limits<std::size_t>::max() / height / 4U ||
      source.row_stride_samples < width * 4U ||
      source.first_sample_offset > source.sample_count() ||
      source.sample_count() - source.first_sample_offset < width * 4U ||
      height - 1U > (source.sample_count() - source.first_sample_offset - width * 4U) /
          source.row_stride_samples) {
    return Result<LinearDisplayP3HalfImage, Error>::failure(
        render_error(ErrorCode::invalid_input, "invalid_source_shape"));
  }
  if (!AnnotationPixelPlanValidator::valid(plan)) {
    return Result<LinearDisplayP3HalfImage, Error>::failure(
        render_error(ErrorCode::state_inconsistent, "invalid_pixel_ownership"));
  }

  LinearDisplayP3HalfImage result;
  result.size_px = source.size_px;
  result.reference_white_nits = request.reference_white_nits;
  result.source_visible_maximum_linear_component = 0.0;
  result.rgba_half.resize(width * height * 4U);
  const auto one = ExtendedP3Mapper::encode_binary16(1.0F);
  constexpr float kMaximumLinearSample = 10000.0F / 203.0F;

  for (std::int32_t y = 0; y < source.size_px.height; ++y) {
    std::size_t written = 0U;
    auto source_span = first_span_for_row(plan.source_visible_spans, y);
    while (source_span != plan.source_visible_spans.end() && source_span->y == y) {
      if (source_span->x < 0 || source_span->length <= 0 ||
          source_span->x + source_span->length > source.size_px.width) {
        return Result<LinearDisplayP3HalfImage, Error>::failure(
            render_error(ErrorCode::state_inconsistent, "source_span_out_of_bounds"));
      }
      for (std::int32_t x = source_span->x;
           x < source_span->x + source_span->length; ++x) {
        const auto input = source.first_sample_offset +
            static_cast<std::size_t>(y) * source.row_stride_samples +
            static_cast<std::size_t>(x) * 4U;
        const auto output =
            (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4U;
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
          const auto decoded = source.sample(input + channel);
          if (!decoded) {
            return Result<LinearDisplayP3HalfImage, Error>::failure(decoded.error());
          }
          const auto linear = std::clamp(
              ExtendedP3Mapper::source_linear(decoded.value(), source.encoding.transfer),
              0.0F, kMaximumLinearSample);
          result.rgba_half[output + channel] =
              ExtendedP3Mapper::encode_binary16(linear);
          result.maximum_linear_component = std::max(
              result.maximum_linear_component, static_cast<double>(linear));
          result.source_visible_maximum_linear_component = std::max(
              *result.source_visible_maximum_linear_component, static_cast<double>(linear));
        }
        result.rgba_half[output + 3U] = one;
        ++written;
      }
      ++source_span;
    }
    auto annotation = first_span_for_row(plan.annotation_owned_spans, y);
    while (annotation != plan.annotation_owned_spans.end() && annotation->y == y) {
      if (annotation->x < 0 || annotation->length <= 0 ||
          annotation->x + annotation->length > source.size_px.width) {
        return Result<LinearDisplayP3HalfImage, Error>::failure(
            render_error(ErrorCode::state_inconsistent, "annotation_span_out_of_bounds"));
      }
      if (!annotation->edge_samples.empty()) {
        for (std::int32_t i = 0; i < annotation->length; ++i) {
          const auto x = static_cast<std::size_t>(annotation->x + i);
          const auto input = source.first_sample_offset +
              static_cast<std::size_t>(y) * source.row_stride_samples + x * 4U;
          const auto resolved = resolve_annotation_pixel(
              annotation->edge_samples[static_cast<std::size_t>(i)],
              source, input);
          if (!resolved) return Result<LinearDisplayP3HalfImage, Error>::failure(resolved.error());
          const auto output = (static_cast<std::size_t>(y) * width + x) * 4U;
          for (std::size_t c = 0; c < 3U; ++c) {
            const float linear = std::min(resolved.value()[c], kMaximumLinearSample);
            result.rgba_half[output + c] = ExtendedP3Mapper::encode_binary16(linear);
            result.maximum_linear_component = std::max(
                result.maximum_linear_component, static_cast<double>(linear));
          }
          result.rgba_half[output + 3U] = one;
          ++written;
        }
        ++annotation;
        continue;
      }
      const auto linear = ExtendedP3Mapper::annotation_linear_display_p3(
          annotation->color_srgb_rgb);
      const std::array<std::uint16_t, 4> encoded{
          ExtendedP3Mapper::encode_binary16(linear[0]),
          ExtendedP3Mapper::encode_binary16(linear[1]),
          ExtendedP3Mapper::encode_binary16(linear[2]),
          one};
      result.maximum_linear_component = std::max(
          result.maximum_linear_component,
          static_cast<double>(std::max({linear[0], linear[1], linear[2]})));
      for (std::int32_t x = annotation->x;
           x < annotation->x + annotation->length; ++x) {
        const auto output =
            (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4U;
        std::copy(encoded.begin(), encoded.end(),
                  result.rgba_half.begin() + static_cast<std::ptrdiff_t>(output));
        ++written;
      }
      ++annotation;
    }
    if (written != width) {
      return Result<LinearDisplayP3HalfImage, Error>::failure(
          render_error(ErrorCode::state_inconsistent, "pixel_plan_row_not_exhaustive"));
    }
  }
  return Result<LinearDisplayP3HalfImage, Error>::success(std::move(result));
}

}  // namespace hdrshot
