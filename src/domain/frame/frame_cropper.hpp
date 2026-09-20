#pragma once

#include "core/error.hpp"
#include "core/frame.hpp"
#include "core/result.hpp"
#include "domain/geometry/selection_model.hpp"

#include <cstdint>
#include <cstddef>
#include <span>
#include <limits>
#include <vector>

namespace hdrshot {

struct CanonicalFrameView {
  FrameId source_frame_id{};
  DisplayGeneration display_generation{};
  SelectionRevision selection_revision{};
  PixelRect source_rect_px{};
  PixelSize size_px{};
  double point_pixel_scale{1.0};
  ColorEncoding encoding{};
  DisplayDynamicRange display_dynamic_range{DisplayDynamicRange::sdr};
  std::vector<std::uint16_t> rgba_half;
  LinearFloatPixels rgba_float;
};

// Read-only ROI: CPU spans borrow their frame; a native source is shared-owned.
// The FrozenDesktop or CanonicalFrameView owning CPU samples must outlive it.
struct SelectionRoiView {
  FrameId source_frame_id{};
  DisplayGeneration display_generation{};
  SelectionRevision selection_revision{};
  PixelRect source_rect_px{};
  PixelSize size_px{};
  double point_pixel_scale{1.0};
  ColorEncoding encoding{};
  DisplayDynamicRange display_dynamic_range{DisplayDynamicRange::sdr};
  std::span<const std::uint16_t> rgba_half;
  std::size_t first_sample_offset{};
  std::size_t row_stride_samples{};
  std::span<const float> rgba_float;
  std::size_t float_storage_capacity_samples{};
  LinearSourceRef linear_source{};

  [[nodiscard]] bool valid_storage() const {
    if (linear_source) {
      const auto dimensions = linear_source->size_px();
      if (!rgba_half.empty() || !rgba_float.empty() ||
          encoding.transfer != TransferFunction::linear ||
          encoding.primaries != ColorPrimaries::display_p3 ||
          encoding.source_reference_white_nits != 0.0 ||
          dimensions.width <= 0 || dimensions.height <= 0 ||
          size_px.width <= 0 || size_px.height <= 0 ||
          size_px.width > dimensions.width || size_px.height > dimensions.height)
        return false;
      const auto width = static_cast<std::size_t>(dimensions.width);
      const auto height = static_cast<std::size_t>(dimensions.height);
      if (width > std::numeric_limits<std::size_t>::max() / height / 16U ||
          linear_source->byte_count() != width * height * 16U ||
          row_stride_samples != width * 4U || first_sample_offset % 4U != 0 ||
          first_sample_offset / 4U >= width * height)
        return false;
      const auto first = first_sample_offset / 4U;
      return first % width <= width - static_cast<std::size_t>(size_px.width) &&
          first / width <= height - static_cast<std::size_t>(size_px.height);
    }
    return rgba_float.empty() ? !rgba_half.empty()
        : rgba_half.empty() && encoding.transfer == TransferFunction::linear;
  }
  [[nodiscard]] std::size_t sample_count() const {
    if (linear_source) return linear_source->byte_count() / sizeof(float);
    return rgba_float.empty() ? rgba_half.size() : rgba_float.size();
  }
  [[nodiscard]] const void* sample_data(std::size_t offset) const {
    if (linear_source) return nullptr; // Native memory requires explicit readback.
    return rgba_float.empty() ? static_cast<const void*>(rgba_half.data() + offset)
                              : static_cast<const void*>(rgba_float.data() + offset);
  }
  [[nodiscard]] std::size_t sample_bytes() const {
    if (linear_source) return sizeof(float);
    return rgba_float.empty() ? sizeof(std::uint16_t) : sizeof(float);
  }
  [[nodiscard]] Result<float, Error> sample(std::size_t offset) const;

};

class FrameCropper {
 public:
  // Explicit reference/fallback boundary. Materializes only this ROI, never
  // changes or attaches a full CPU copy to the immutable native source.
  [[nodiscard]] static Result<CanonicalFrameView, Error> read_cpu_region(
      const SelectionRoiView& frame);
  [[nodiscard]] static Result<CanonicalFrameView, Error> crop(
      const FrozenDesktop& frozen_desktop,
      const SelectionSnapshot& selection);

  [[nodiscard]] static Result<CanonicalFrameView, Error> crop_display(
      const FrozenDesktop& frozen_desktop,
      DisplayId target_display_id,
      const SelectionSnapshot& selection);

  [[nodiscard]] static SelectionRoiView view(const CanonicalFrameView& frame);

  [[nodiscard]] static Result<SelectionRoiView, Error> view(
      const FrozenDesktop& frozen_desktop,
      const SelectionSnapshot& selection);

  [[nodiscard]] static Result<SelectionRoiView, Error> view_display(
      const FrozenDesktop& frozen_desktop,
      DisplayId target_display_id,
      const SelectionSnapshot& selection);
};

}  // namespace hdrshot
