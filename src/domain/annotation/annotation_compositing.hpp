#pragma once
#include "domain/annotation/annotation_render_plan.hpp"
#include "domain/annotation/annotation_pixel_plan_validator.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include <algorithm>
#include "domain/frame/frame_cropper.hpp"
#include <array>
#include <limits>
#include <map>

namespace hdrshot {
// Explicit CPU reference/fallback materialization. The normal Mac presenter and
// encoder bind native_samples directly and never call this function.
inline Result<AnnotationPixelPlan, Error> materialize_annotation_plan(const AnnotationPixelPlan& plan) {
  if (!AnnotationPixelPlanValidator::valid(plan)) return Result<AnnotationPixelPlan, Error>::failure(
      {ErrorCode::state_inconsistent, "AnnotationMaterialization", Retryability::never, {{"reason", "invalid_plan"}}});
  auto result = plan;
  struct Group { std::size_t first{}, end{}; std::vector<std::array<float, 4>> samples; };
  std::map<LinearSampleRef, Group> groups;
  for (const auto& span : result.annotation_owned_spans) if (span.native_samples) {
    auto [it, inserted] = groups.try_emplace(span.native_samples,
        Group{span.native_sample_offset, span.native_sample_offset + static_cast<std::size_t>(span.length), {}});
    if (!inserted) {
      it->second.first = std::min(it->second.first, span.native_sample_offset);
      it->second.end = std::max(it->second.end, span.native_sample_offset + static_cast<std::size_t>(span.length));
    }
  }
  for (auto& [storage, group] : groups) {
    auto samples = storage->read_samples(group.first, group.end - group.first);
    if (!samples) return Result<AnnotationPixelPlan, Error>::failure(samples.error());
    if (samples.value().size() != group.end - group.first)
      return Result<AnnotationPixelPlan, Error>::failure({ErrorCode::state_inconsistent,
          "AnnotationMaterialization", Retryability::never, {{"reason", "sample_count_mismatch"}}});
    group.samples = std::move(samples.value());
  }
  for (auto& span : result.annotation_owned_spans) if (span.native_samples) {
    const auto& group = groups.at(span.native_samples);
    const auto offset = span.native_sample_offset - group.first;
    const auto begin = group.samples.begin() + static_cast<std::ptrdiff_t>(offset);
    span.edge_samples.assign(begin, begin + span.length);
    span.native_samples.reset(); span.native_sample_offset = 0;
  }
  if (!AnnotationPixelPlanValidator::valid(result)) return Result<AnnotationPixelPlan, Error>::failure(
      {ErrorCode::invalid_color_contract, "AnnotationMaterialization", Retryability::never, {{"reason", "invalid_composed_samples"}}});
  return Result<AnnotationPixelPlan, Error>::success(std::move(result));
}
inline std::array<float, 4> annotation_linear_sample(
    const AnnotationOwnedSpan& span, std::size_t offset) {
  if (!span.edge_samples.empty()) return span.edge_samples[offset];
  const auto c = ExtendedP3Mapper::annotation_linear_display_p3(span.color_srgb_rgb);
  return {c[0], c[1], c[2], 0.0F};
}

inline Result<std::array<float, 3>, Error> resolve_annotation_pixel(
    const std::array<float, 4>& sample, const std::uint16_t* source,
    TransferFunction transfer = TransferFunction::extended_srgb) {
  std::array<float, 3> result{sample[0], sample[1], sample[2]};
  if (sample[3] > 0.0F) {
    for (std::size_t c = 0; c < 3; ++c) {
      auto value = ExtendedP3Mapper::decode_binary16(source[c]);
      if (!value) return Result<std::array<float, 3>, Error>::failure(value.error());
      result[c] += sample[3] * std::max(
          0.0F, ExtendedP3Mapper::source_linear(value.value(), transfer));
    }
  }
  return Result<std::array<float, 3>, Error>::success(result);
}

inline Result<std::array<float, 3>, Error> resolve_annotation_pixel(
    const std::array<float, 4>& sample, const SelectionRoiView& source, std::size_t offset) {
  std::array<float, 3> result{sample[0], sample[1], sample[2]};
  if (sample[3] > 0.0F) {
    for (std::size_t c = 0; c < 3; ++c) {
      auto value = source.sample(offset + c);
      if (!value) return Result<std::array<float, 3>, Error>::failure(value.error());
      result[c] += sample[3] * std::max(0.0F,
          ExtendedP3Mapper::source_linear(value.value(), source.encoding.transfer));
    }
  }
  return Result<std::array<float, 3>, Error>::success(result);
}

// Backend-neutral upload layout: zero index = source passthrough; otherwise
// table[index-1] contains premultiplied P3 RGB + remaining source weight.
struct AnnotationGpuUpload {
  std::vector<std::uint32_t> indices;
  std::vector<std::array<float, 4>> samples;
};
inline Result<AnnotationGpuUpload, Error> prepare_annotation_gpu_upload(
    const AnnotationPixelPlan& plan) {
  const auto fail = [] {
    return Result<AnnotationGpuUpload, Error>::failure(Error{
        ErrorCode::state_inconsistent, "AnnotationGpuUpload", Retryability::never,
        {{"reason", "invalid_pixel_plan"}}});
  };
  if (!AnnotationPixelPlanValidator::valid(plan)) return fail();
  const auto width = static_cast<std::size_t>(plan.output_size_px.width);
  const auto height = static_cast<std::size_t>(plan.output_size_px.height);
  if (width > std::numeric_limits<std::size_t>::max() / height) return fail();
  AnnotationGpuUpload out{std::vector<std::uint32_t>(width * height), {}};
  std::map<std::uint32_t, std::uint32_t> palette;
  for (const auto& span : plan.annotation_owned_spans) {
    if (span.native_samples) return fail(); // Never mistake native clean RGB for palette color.
    std::uint32_t solid_index = 0;
    if (span.edge_samples.empty()) {
      auto found = palette.find(span.color_srgb_rgb);
      if (found == palette.end()) {
        if (out.samples.size() >= std::numeric_limits<std::uint32_t>::max()) return fail();
        out.samples.push_back(annotation_linear_sample(span, 0));
        solid_index = static_cast<std::uint32_t>(out.samples.size());
        palette.emplace(span.color_srgb_rgb, solid_index);
      } else solid_index = found->second;
    }
    const auto start = static_cast<std::size_t>(span.y) * width + static_cast<std::size_t>(span.x);
    for (std::int32_t i = 0; i < span.length; ++i) {
      auto index = solid_index;
      if (!span.edge_samples.empty()) {
        if (out.samples.size() >= std::numeric_limits<std::uint32_t>::max()) return fail();
        out.samples.push_back(span.edge_samples[static_cast<std::size_t>(i)]);
        index = static_cast<std::uint32_t>(out.samples.size());
      }
      out.indices[start + static_cast<std::size_t>(i)] = index;
    }
  }
  if (out.samples.empty()) out.samples.push_back({0, 0, 0, 0});
  return Result<AnnotationGpuUpload, Error>::success(std::move(out));
}
} // namespace hdrshot
