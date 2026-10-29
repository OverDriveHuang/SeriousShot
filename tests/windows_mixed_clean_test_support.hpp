#pragma once

#include "platform/windows/windows_d3d_export.hpp"
#include "platform/windows/windows_gpu_source.hpp"
#include "platform/windows/windows_color.hpp"
#include "test_support.hpp"

namespace hdrshot::test {
struct WindowsMixedCleanFixture {
  SelectionRoiView roi;
  AnnotationPixelPlan plan;
  // Hand oracle: source-visible, opaque white, two precomposed AA edges,
  // source-visible. Neutral scRGB stays neutral Linear P3 at scale/gain 1.
  static constexpr std::array<std::array<float, 4>, 5> expected{{
      {.25F, .25F, .25F, 1}, {1, 1, 1, 1},
      {.625F, .75F, .875F, 1}, {2.25F, 2.5F, 2.75F, 1},
      {.75F, .75F, .75F, 1}}};
};

inline WindowsMixedCleanFixture windows_mixed_clean_fixture() {
  std::vector<std::uint16_t> raw;
  for (auto half : {0x3400U, 0x3800U, 0x4000U, 0x4400U, 0x3a00U})
    raw.insert(raw.end(), {static_cast<std::uint16_t>(half),
        static_cast<std::uint16_t>(half), static_cast<std::uint16_t>(half), 0x3c00});
  auto source = windows_normalize_scrgb_source({5, 1}, std::move(raw), 1.F, 1.);
  HDRSHOT_CHECK(source.has_value());
  // The first value is deliberately unrelated to either edge. Offsets 1 and 2
  // must survive grouped native readback; using offset 0 cannot match the oracle.
  const std::array<PositionedCleanSample, 3> work{{
      {{.9F, .8F, .7F, 0}, 0, 0},
      {{.125F, .25F, .375F, .25F}, 2, 0},
      {{.25F, .5F, .75F, .5F}, 3, 0}}};
  auto clean = windows_clean_composition()->compose_native(source.value(), work);
  HDRSHOT_CHECK(clean.has_value());
  HDRSHOT_CHECK(clean.value()->sample_count() == 3);
  auto samples = clean.value()->read_samples(0, 3);
  HDRSHOT_CHECK(samples.has_value());
  for (std::size_t c = 0; c < 4; ++c) {
    HDRSHOT_CHECK_NEAR(samples.value()[0][c], work[0].annotation[c], 1e-7);
    for (std::size_t i = 1; i < 3; ++i)
      HDRSHOT_CHECK_NEAR(samples.value()[i][c],
          c == 3 ? 0.F : WindowsMixedCleanFixture::expected[i + 1][c], 2e-6);
  }
  WindowsMixedCleanFixture fixture;
  fixture.roi.size_px = {5, 1};
  fixture.roi.source_rect_px = {0, 0, 5, 1};
  fixture.roi.encoding = WindowsColor::linear_p3_encoding();
  fixture.roi.display_dynamic_range = DisplayDynamicRange::hdr;
  fixture.roi.row_stride_samples = 20;
  fixture.roi.linear_source = source.value();
  fixture.plan.output_size_px = {5, 1};
  fixture.plan.source_visible_spans = {{0, 0, 1}, {0, 4, 1}};
  fixture.plan.annotation_owned_spans = {{0, 1, 1, ObjectId{1}, 0xffffff, {}}};
  for (int x = 2; x <= 3; ++x) {
    AnnotationOwnedSpan span{0, x, 1, ObjectId{2}, 0xff0000, {}};
    span.clean_composited = true;
    span.native_samples = clean.value();
    span.native_sample_offset = static_cast<std::size_t>(x - 1);
    fixture.plan.annotation_owned_spans.push_back(std::move(span));
  }
  return fixture;
}
} // namespace hdrshot::test
