#include "platform/windows/windows_color.hpp"

#include "domain/annotation/annotation_pixel_plan_validator.hpp"
#include "domain/color/extended_p3_mapper.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hdrshot {
namespace {
Error color_error(const char* reason) {
  return {ErrorCode::invalid_color_contract, "WindowsColor", Retryability::never,
          {{"reason", reason}}};
}
}

Result<float, Error> WindowsColor::scrgb_to_edr_scale(WindowsSourceWhite white) {
  if (!white.hdr_active) return Result<float, Error>::success(1.0F);
  if (!std::isfinite(white.sdr_white_nits) || white.sdr_white_nits < 80.0 ||
      white.sdr_white_nits > 10000.0) {
    return Result<float, Error>::failure(color_error("invalid_hdr_sdr_white"));
  }
  return Result<float, Error>::success(static_cast<float>(80.0 / white.sdr_white_nits));
}

std::array<float, 3> WindowsColor::scrgb_to_linear_p3(
    const std::array<float, 3> rgb, const float scale) noexcept {
  // D65-adapted sRGB -> Display P3; same primaries as shared annotation colors.
  return {(0.82246197F * rgb[0] + 0.17753803F * rgb[1]) * scale,
          (0.03319420F * rgb[0] + 0.96680580F * rgb[1]) * scale,
          (0.01708263F * rgb[0] + 0.07239744F * rgb[1] + 0.91051993F * rgb[2]) * scale};
}

std::array<float, 3> WindowsColor::linear_p3_to_scrgb(
    const std::array<float, 3> rgb, const float scale) noexcept {
  return {(1.22494018F * rgb[0] - 0.22494018F * rgb[1]) * scale,
          (-0.04205695F * rgb[0] + 1.04205695F * rgb[1]) * scale,
          (-0.01963755F * rgb[0] - 0.07863605F * rgb[1] + 1.09827360F * rgb[2]) * scale};
}

Result<std::vector<std::uint16_t>, Error> WindowsColor::normalize_capture(
    const std::span<const std::uint16_t> raw, const WindowsSourceWhite white) {
  const auto scale = scrgb_to_edr_scale(white);
  if (!scale) return Result<std::vector<std::uint16_t>, Error>::failure(scale.error());
  if (raw.empty() || raw.size() % 4U != 0U) {
    return Result<std::vector<std::uint16_t>, Error>::failure(color_error("invalid_rgba_size"));
  }
  std::vector<std::uint16_t> out(raw.size());
  for (std::size_t p = 0; p < raw.size(); p += 4U) {
    std::array<float, 3> rgb{};
    for (std::size_t c = 0; c < 3U; ++c) {
      const auto value = ExtendedP3Mapper::decode_binary16(raw[p + c]);
      if (!value) return Result<std::vector<std::uint16_t>, Error>::failure(value.error());
      rgb[c] = value.value();
    }
    const auto p3 = scrgb_to_linear_p3(rgb, scale.value());
    for (std::size_t c = 0; c < 3U; ++c) {
      if (!std::isfinite(p3[c]) || std::abs(p3[c]) > 65504.0F) {
        return Result<std::vector<std::uint16_t>, Error>::failure(color_error("linear_p3_overflow"));
      }
      out[p + c] = ExtendedP3Mapper::encode_binary16(p3[c]);
    }
    out[p + 3U] = 0x3c00U;
  }
  return Result<std::vector<std::uint16_t>, Error>::success(std::move(out));
}

bool WindowsColor::valid_linear_p3_roi(const SelectionRoiView& source) noexcept {
  if (source.encoding != linear_p3_encoding() || source.size_px.width <= 0 ||
      source.size_px.height <= 0) return false;
  const auto w = static_cast<std::size_t>(source.size_px.width);
  const auto h = static_cast<std::size_t>(source.size_px.height);
  if (w > std::numeric_limits<std::size_t>::max() / h / 4U ||
      source.row_stride_samples < w * 4U ||
      source.first_sample_offset > source.rgba_half.size()) return false;
  const auto available = source.rgba_half.size() - source.first_sample_offset;
  return available >= w * 4U &&
      h - 1U <= (available - w * 4U) / source.row_stride_samples;
}

Result<RangeFitResult, Error> WindowsLinearP3RangeProbe::probe(
    const SelectionRoiView& source, const AnnotationPixelPlan& plan,
    const RangeProbeOptimization) {
  if (!WindowsColor::valid_linear_p3_roi(source) || source.size_px != plan.output_size_px ||
      !AnnotationPixelPlanValidator::valid(plan)) {
    return Result<RangeFitResult, Error>::failure(color_error("invalid_source_or_pixel_plan"));
  }
  RangeFitResult out{true, 0, 0, 0, "windows_linear_p3_source_scan"};
  for (const auto& span : plan.source_visible_spans) out.source_visible_pixel_count += span.length;
  for (const auto& span : plan.annotation_owned_spans) out.skipped_annotation_pixel_count += span.length;
  // No display-headroom fast path: the whole supported Windows matrix is not
  // proven yet. All annotation-owned pixels (including AA) skip classification.
  for (const auto& span : plan.source_visible_spans) {
    for (std::int32_t x = span.x; x < span.x + span.length; ++x) {
      const auto p = source.first_sample_offset +
          static_cast<std::size_t>(span.y) * source.row_stride_samples +
          static_cast<std::size_t>(x) * 4U;
      for (std::size_t c = 0; c < 3U; ++c) {
        const auto decoded = ExtendedP3Mapper::decode_binary16(source.rgba_half[p + c]);
        ++out.scanned_component_count;
        if (!decoded) return Result<RangeFitResult, Error>::failure(decoded.error());
        if (decoded.value() > 1.0F) out.fits_sdr = false;
      }
    }
  }
  return Result<RangeFitResult, Error>::success(std::move(out));
}
}  // namespace hdrshot
