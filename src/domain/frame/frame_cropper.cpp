#include "domain/frame/frame_cropper.hpp"

#include <algorithm>
#include <cmath>
#include "domain/color/extended_p3_mapper.hpp"
#include <cstddef>
#include <map>
#include <string>

namespace hdrshot {
namespace {

Error crop_error(const ErrorCode code, const char* reason) {
  return Error{code, "FrameCropper", Retryability::never, {{"reason", reason}}};
}

}  // namespace

Result<float, Error> SelectionRoiView::sample(const std::size_t offset) const {
  if (linear_source) return Result<float, Error>::failure(
      crop_error(ErrorCode::precondition_failed, "explicit_cpu_region_read_required"));
  if (!valid_storage() || offset >= sample_count()) return Result<float, Error>::failure(
      crop_error(ErrorCode::invalid_input, "sample_out_of_bounds"));
  if (rgba_float.empty()) return ExtendedP3Mapper::decode_binary16(rgba_half[offset]);
  if (!std::isfinite(rgba_float[offset])) return Result<float, Error>::failure(
      crop_error(ErrorCode::invalid_color_contract, "non_finite_float"));
  return Result<float, Error>::success(rgba_float[offset]);
}

Result<CanonicalFrameView, Error> FrameCropper::crop(
    const FrozenDesktop& frozen_desktop,
    const SelectionSnapshot& selection) {
  if (frozen_desktop.canonical_segments.size() != 1) {
    return Result<CanonicalFrameView, Error>::failure(crop_error(
        ErrorCode::precondition_failed, "cross_segment_crop_not_implemented"));
  }
  return crop_display(
      frozen_desktop,
      frozen_desktop.canonical_segments.front().display_id,
      selection);
}

Result<CanonicalFrameView, Error> FrameCropper::crop_display(
    const FrozenDesktop& frozen_desktop,
    const DisplayId target_display_id,
    const SelectionSnapshot& selection) {
  const auto found = std::find_if(
      frozen_desktop.canonical_segments.begin(),
      frozen_desktop.canonical_segments.end(),
      [target_display_id](const CanonicalFrameSegment& candidate) {
        return candidate.display_id == target_display_id;
      });
  if (found == frozen_desktop.canonical_segments.end()) {
    return Result<CanonicalFrameView, Error>::failure(
        crop_error(ErrorCode::object_not_found, "target_display_segment_missing"));
  }
  const auto& segment = *found;
  const auto rect = selection.desktop_rect;
  if (rect.empty() || rect.x < 0 || rect.y < 0 ||
      rect.right() > segment.size_px.width || rect.bottom() > segment.size_px.height) {
    return Result<CanonicalFrameView, Error>::failure(
        crop_error(ErrorCode::invalid_input, "selection_out_of_bounds"));
  }

  const auto source_width = static_cast<std::size_t>(segment.size_px.width);
  if (segment.linear_source) {
    auto view = view_display(frozen_desktop, target_display_id, selection);
    if (!view) return Result<CanonicalFrameView, Error>::failure(view.error());
    return read_cpu_region(view.value());
  }
  const auto output_width = static_cast<std::size_t>(rect.width);
  const auto output_height = static_cast<std::size_t>(rect.height);
  std::vector<std::uint16_t> pixels(segment.rgba_float.empty() ? output_width * output_height * 4U : 0);
  LinearFloatPixels floats(segment.rgba_float.empty() ? 0 : output_width * output_height * 4U);
  for (std::size_t row = 0; row < output_height; ++row) {
    const auto source_row = static_cast<std::size_t>(rect.y) + row;
    const auto source_offset =
        (source_row * source_width + static_cast<std::size_t>(rect.x)) * 4U;
    const auto destination_offset = row * output_width * 4U;
    if (!floats.empty()) {
      std::copy_n(segment.rgba_float.begin() + static_cast<std::ptrdiff_t>(source_offset),
          output_width * 4U, floats.begin() + static_cast<std::ptrdiff_t>(destination_offset));
    } else std::copy_n(
        segment.rgba_half.begin() + static_cast<std::ptrdiff_t>(source_offset),
        static_cast<std::ptrdiff_t>(output_width * 4U),
        pixels.begin() + static_cast<std::ptrdiff_t>(destination_offset));
  }

  return Result<CanonicalFrameView, Error>::success(CanonicalFrameView{
      frozen_desktop.frame_id,
      frozen_desktop.display_generation,
      selection.revision,
      rect,
      PixelSize{rect.width, rect.height},
      segment.point_pixel_scale,
      segment.encoding,
      segment.display_dynamic_range,
      std::move(pixels),
      std::move(floats),
  });
}

SelectionRoiView FrameCropper::view(const CanonicalFrameView& frame) {
  return SelectionRoiView{
      frame.source_frame_id,
      frame.display_generation,
      frame.selection_revision,
      frame.source_rect_px,
      frame.size_px,
      frame.point_pixel_scale,
      frame.encoding,
      frame.display_dynamic_range,
      frame.rgba_half,
      0U,
      static_cast<std::size_t>(frame.size_px.width) * 4U,
      frame.rgba_float,
      frame.rgba_float.capacity(),
      {},
  };
}

Result<SelectionRoiView, Error> FrameCropper::view(
    const FrozenDesktop& frozen_desktop,
    const SelectionSnapshot& selection) {
  if (frozen_desktop.canonical_segments.size() != 1) {
    return Result<SelectionRoiView, Error>::failure(crop_error(
        ErrorCode::precondition_failed, "cross_segment_view_not_implemented"));
  }
  return view_display(
      frozen_desktop,
      frozen_desktop.canonical_segments.front().display_id,
      selection);
}

Result<SelectionRoiView, Error> FrameCropper::view_display(
    const FrozenDesktop& frozen_desktop,
    const DisplayId target_display_id,
    const SelectionSnapshot& selection) {
  const auto found = std::find_if(
      frozen_desktop.canonical_segments.begin(),
      frozen_desktop.canonical_segments.end(),
      [target_display_id](const CanonicalFrameSegment& candidate) {
        return candidate.display_id == target_display_id;
      });
  if (found == frozen_desktop.canonical_segments.end()) {
    return Result<SelectionRoiView, Error>::failure(
        crop_error(ErrorCode::object_not_found, "target_display_segment_missing"));
  }
  const auto& segment = *found;
  const auto rect = selection.desktop_rect;
  if (rect.empty() || rect.x < 0 || rect.y < 0 ||
      rect.right() > segment.size_px.width || rect.bottom() > segment.size_px.height) {
    return Result<SelectionRoiView, Error>::failure(
        crop_error(ErrorCode::invalid_input, "selection_out_of_bounds"));
  }
  const auto source_width = static_cast<std::size_t>(segment.size_px.width);
  const auto first_sample =
      (static_cast<std::size_t>(rect.y) * source_width +
       static_cast<std::size_t>(rect.x)) * 4U;
  return Result<SelectionRoiView, Error>::success(SelectionRoiView{
      frozen_desktop.frame_id,
      frozen_desktop.display_generation,
      selection.revision,
      rect,
      PixelSize{rect.width, rect.height},
      segment.point_pixel_scale,
      segment.encoding,
      segment.display_dynamic_range,
      segment.rgba_half,
      first_sample,
      source_width * 4U,
      segment.rgba_float,
      segment.rgba_float.capacity(),
      segment.linear_source,
  });
}

Result<CanonicalFrameView, Error> FrameCropper::read_cpu_region(const SelectionRoiView& frame) {
  if (!frame.valid_storage() || frame.size_px.width <= 0 || frame.size_px.height <= 0)
    return Result<CanonicalFrameView, Error>::failure(crop_error(ErrorCode::invalid_input, "invalid_cpu_roi"));
  CanonicalFrameView out{frame.source_frame_id, frame.display_generation,
      frame.selection_revision, frame.source_rect_px, frame.size_px,
      frame.point_pixel_scale, frame.encoding, frame.display_dynamic_range, {}, {}};
  const auto width = static_cast<std::size_t>(frame.size_px.width);
  const auto height = static_cast<std::size_t>(frame.size_px.height);
  if (frame.linear_source) {
    const auto stride = frame.row_stride_samples / 4;
    if (!stride || stride != static_cast<std::size_t>(frame.linear_source->size_px().width) ||
        frame.row_stride_samples % 4 || frame.first_sample_offset % 4 ||
        frame.first_sample_offset > frame.sample_count() ||
        frame.sample_count() - frame.first_sample_offset < width * 4 ||
        height - 1 > (frame.sample_count() - frame.first_sample_offset - width * 4) / frame.row_stride_samples)
      return Result<CanonicalFrameView, Error>::failure(crop_error(ErrorCode::invalid_input, "invalid_native_roi_stride"));
    PixelRect rect{static_cast<int>((frame.first_sample_offset / 4) % stride),
        static_cast<int>((frame.first_sample_offset / 4) / stride), frame.size_px.width, frame.size_px.height};
    auto samples = frame.linear_source->read_region(rect);
    if (!samples) return Result<CanonicalFrameView, Error>::failure(samples.error());
    if (samples.value().size() != width * height * 4)
      return Result<CanonicalFrameView, Error>::failure(crop_error(ErrorCode::invalid_input, "native_readback_shape"));
    out.rgba_float = std::move(samples.value());
  } else {
    if (frame.row_stride_samples < width * 4 || frame.first_sample_offset > frame.sample_count() ||
        frame.sample_count() - frame.first_sample_offset < width * 4 ||
        height - 1 > (frame.sample_count() - frame.first_sample_offset - width * 4) / frame.row_stride_samples)
      return Result<CanonicalFrameView, Error>::failure(crop_error(ErrorCode::invalid_input, "cpu_roi_out_of_bounds"));
    if (frame.rgba_float.empty()) out.rgba_half.resize(width * height * 4);
    else out.rgba_float.resize(width * height * 4);
    for (std::size_t y = 0; y < height; ++y) {
      const auto src = frame.first_sample_offset + y * frame.row_stride_samples;
      if (frame.rgba_float.empty()) std::copy_n(frame.rgba_half.data() + src, width * 4, out.rgba_half.data() + y * width * 4);
      else std::copy_n(frame.rgba_float.data() + src, width * 4, out.rgba_float.data() + y * width * 4);
    }
  }
  return Result<CanonicalFrameView, Error>::success(std::move(out));
}

}  // namespace hdrshot
