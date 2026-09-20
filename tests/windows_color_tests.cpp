#include "platform/windows/windows_color.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "test_support.hpp"
#include <limits>

using namespace hdrshot;
namespace {
void white_units() {
  HDRSHOT_CHECK_NEAR(WindowsColor::scrgb_to_edr_scale({true, 80}).value(), 1, 1e-7);
  const auto scale = WindowsColor::scrgb_to_edr_scale({true, 203}).value();
  const auto p3 = WindowsColor::scrgb_to_linear_p3({2.5375F, 2.5375F, 2.5375F}, scale);
  for (auto v : p3) HDRSHOT_CHECK_NEAR(v, 1, 2e-7);
  HDRSHOT_CHECK_NEAR(WindowsColor::scrgb_to_edr_scale({false, 400}).value(), 1, 0);
  HDRSHOT_CHECK(!WindowsColor::scrgb_to_edr_scale({true, 0}));
  HDRSHOT_CHECK(!WindowsColor::scrgb_to_edr_scale({true, std::numeric_limits<double>::quiet_NaN()}));
}
void gamut_and_extended_range() {
  const auto r = WindowsColor::scrgb_to_linear_p3({1, 0, 0}, 1);
  HDRSHOT_CHECK_NEAR(r[0], 0.82246197, 1e-7);
  HDRSHOT_CHECK_NEAR(r[1], 0.03319420, 1e-7);
  HDRSHOT_CHECK_NEAR(r[2], 0.01708263, 1e-7);
  for (const auto p3 : {std::array<float, 3>{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {-0.25F, 2, 12}}) {
    const auto raw = WindowsColor::linear_p3_to_scrgb(p3, 2.5F);
    const auto roundtrip = WindowsColor::scrgb_to_linear_p3(raw, 0.4F);
    for (std::size_t c = 0; c < 3; ++c) HDRSHOT_CHECK_NEAR(roundtrip[c], p3[c], 2e-6);
  }
  HDRSHOT_CHECK(WindowsColor::linear_p3_to_scrgb({1, 0, 0}, 1)[1] < 0);
}
void capture_does_not_gamma_encode() {
  const std::vector<std::uint16_t> raw{0x3800, 0x3800, 0x3800, 0x7e00};
  const auto converted = WindowsColor::normalize_capture(raw, {false, 0});
  HDRSHOT_CHECK(converted.has_value());
  for (int c = 0; c < 3; ++c) HDRSHOT_CHECK(converted.value()[c] == 0x3800);
  HDRSHOT_CHECK(converted.value()[3] == 0x3c00);
  auto invalid = raw;
  invalid[0] = 0x7c00;
  HDRSHOT_CHECK(!WindowsColor::normalize_capture(invalid, {false, 0}));
}
void classification_and_coverage() {
  CanonicalFrameView frame{};
  frame.size_px = {2, 1};
  frame.encoding = WindowsColor::linear_p3_encoding();
  frame.rgba_half = {0xbc00, 0x3c00, 0, 0x3c00, 0x4000, 0x4000, 0x4000, 0x3c00};
  AnnotationPixelPlan plan{0, {2, 1}, {{0, 0, 2}}, {}};
  WindowsLinearP3RangeProbe probe;
  HDRSHOT_CHECK(!probe.probe(FrameCropper::view(frame), plan, {}).value().fits_sdr);
  plan.source_visible_spans = {{0, 0, 1}};
  plan.annotation_owned_spans = {{0, 1, 1, ObjectId{1}, 0xff0000, {{{0.4F, 0, 0, 0.5F}}}}};
  const auto fit = probe.probe(FrameCropper::view(frame), plan, {});
  HDRSHOT_CHECK(fit && fit.value().fits_sdr && fit.value().skipped_annotation_pixel_count == 1);
  auto roi = FrameCropper::view(frame);
  roi.first_sample_offset = std::numeric_limits<std::size_t>::max();
  HDRSHOT_CHECK(!probe.probe(roi, plan, {}));
  frame.encoding.transfer = TransferFunction::extended_srgb;
  HDRSHOT_CHECK(!probe.probe(FrameCropper::view(frame), plan, {}));
}
}
int main() {
  return hdrshot::test::run({{"white units", white_units}, {"gamut and extended range", gamut_and_extended_range},
      {"linear capture", capture_does_not_gamma_encode}, {"classification and AA ownership", classification_and_coverage}});
}
