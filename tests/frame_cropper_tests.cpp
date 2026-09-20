#include "domain/frame/frame_cropper.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <vector>

namespace {

using namespace hdrshot;

FrozenDesktop fixture() {
  std::vector<std::uint16_t> pixels;
  for (std::uint16_t value = 0; value < 48; ++value) {
    pixels.push_back(value);
  }
  return FrozenDesktop{
      FrameId{9},
      4,
      LogicalRect{0, 0, 4, 3},
      {CanonicalFrameSegment{
          DisplayId{1},
          LogicalRect{0, 0, 4, 3},
          1.0,
          PixelSize{4, 3},
          PixelFormat::rgba16_float,
          ColorEncoding{ColorPrimaries::bt2020, TransferFunction::pq, AlphaMode::straight, 100.0},
          DisplayDynamicRange::hdr,
          std::move(pixels),
      }},
  };
}

void crop_is_pixel_exact_and_preserves_provenance() {
  const auto result = FrameCropper::crop(
      fixture(), SelectionSnapshot{7, PixelRect{1, 1, 2, 2}});
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value().source_frame_id == FrameId{9});
  HDRSHOT_CHECK(result.value().display_generation == 4);
  HDRSHOT_CHECK(result.value().selection_revision == 7);
  HDRSHOT_CHECK(result.value().size_px == (PixelSize{2, 2}));
  HDRSHOT_CHECK(result.value().point_pixel_scale == 1.0);
  const std::vector<std::uint16_t> expected{
      20, 21, 22, 23, 24, 25, 26, 27,
      36, 37, 38, 39, 40, 41, 42, 43,
  };
  HDRSHOT_CHECK(result.value().rgba_half == expected);
}

void invalid_and_cross_segment_requests_fail_without_partial_view() {
  const auto outside = FrameCropper::crop(
      fixture(), SelectionSnapshot{1, PixelRect{3, 2, 2, 2}});
  HDRSHOT_CHECK(!outside.has_value());
  HDRSHOT_CHECK(outside.error().code == ErrorCode::invalid_input);

  auto two_segments = fixture();
  two_segments.canonical_segments.push_back(two_segments.canonical_segments.front());
  const auto cross = FrameCropper::crop(
      two_segments, SelectionSnapshot{1, PixelRect{0, 0, 1, 1}});
  HDRSHOT_CHECK(!cross.has_value());
  HDRSHOT_CHECK(cross.error().code == ErrorCode::precondition_failed);
}

void explicit_display_crop_selects_one_segment_from_multi_display_frame() {
  auto two_segments = fixture();
  auto second = two_segments.canonical_segments.front();
  second.display_id = DisplayId{2};
  second.desktop_frame_points.x = 4.0;
  for (auto& value : second.rgba_half) {
    value = static_cast<std::uint16_t>(value + 100U);
  }
  two_segments.canonical_segments.push_back(std::move(second));
  two_segments.desktop_bounds_points.width = 8.0;
  const auto cropped = FrameCropper::crop_display(
      two_segments,
      DisplayId{2},
      SelectionSnapshot{2, PixelRect{1, 1, 2, 1}});
  HDRSHOT_CHECK(cropped.has_value());
  HDRSHOT_CHECK(cropped.value().size_px == (PixelSize{2, 1}));
  HDRSHOT_CHECK(cropped.value().rgba_half == (std::vector<std::uint16_t>{
      120, 121, 122, 123, 124, 125, 126, 127,
  }));
}

void selection_roi_is_a_read_only_view_of_the_source_segment() {
  const auto desktop = fixture();
  const auto viewed = FrameCropper::view_display(
      desktop, DisplayId{1}, SelectionSnapshot{5, PixelRect{1, 1, 2, 2}});
  HDRSHOT_CHECK(viewed.has_value());
  HDRSHOT_CHECK(viewed.value().rgba_half.data() ==
                desktop.canonical_segments.front().rgba_half.data());
  HDRSHOT_CHECK(viewed.value().first_sample_offset == 20U);
  HDRSHOT_CHECK(viewed.value().row_stride_samples == 16U);
  HDRSHOT_CHECK(viewed.value().rgba_half[
                    viewed.value().first_sample_offset] == 20U);
  HDRSHOT_CHECK(viewed.value().rgba_half[
                    viewed.value().first_sample_offset +
                    viewed.value().row_stride_samples] == 36U);
}

}  // namespace

int main() {
  using hdrshot::test::TestCase;
  return hdrshot::test::run(std::vector<TestCase>{
      {"D4 single segment crop is exact", crop_is_pixel_exact_and_preserves_provenance},
      {"D4 invalid/cross-segment is explicit", invalid_and_cross_segment_requests_fail_without_partial_view},
      {"D4 explicit display crop from multi-display frame", explicit_display_crop_selects_one_segment_from_multi_display_frame},
      {"D4 selection ROI aliases canonical source without crop copy", selection_roi_is_a_read_only_view_of_the_source_segment},
  });
}
