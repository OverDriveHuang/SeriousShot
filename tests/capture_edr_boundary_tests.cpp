#include "domain/color/capture_edr_boundary.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "domain/frame/frame_pipeline.hpp"
#include "domain/output/source_range_probe.hpp"
#include "ports/ultra_hdr_ports.hpp"
#include "test_support.hpp"
#include <cmath>
#include <limits>

namespace {
using namespace hdrshot;
void threshold_adjacent_samples_and_double_boundary() {
  const float first_hdr = std::nextafter(kCaptureSdrMaximumEdr,
      std::numeric_limits<float>::infinity());
  HDRSHOT_CHECK(std::bit_cast<std::uint32_t>(first_hdr) == 0x3f8050b6U);
  for (float value : {-2.0F, -0.0F, 0.5F, 1.0F, 1.00033020973F, 1.001F, kCaptureSdrMaximumEdr})
    HDRSHOT_CHECK(!source_requires_hdr(value, true));
  HDRSHOT_CHECK(source_requires_hdr(first_hdr, true));
  HDRSHOT_CHECK(source_requires_hdr(kCaptureHdrMinimumEdr, true));
  HDRSHOT_CHECK(!source_requires_hdr(std::nextafter(kCaptureHdrMinimumEdr, 0.0), true));
  HDRSHOT_CHECK(source_requires_hdr(1.001F));
  LinearDisplayP3HalfImage image;
  image.capture_sdr_tolerance = true;
  for (double maximum : {1.001, double(kCaptureSdrMaximumEdr), kCaptureHdrMinimumEdr, double(first_hdr)}) {
    image.maximum_linear_component = maximum;
    image.source_visible_maximum_linear_component = maximum;
    HDRSHOT_CHECK((jpeg_output_kind(image) == JpegOutputKind::ultra_hdr) ==
        source_requires_hdr(maximum, true));
    HDRSHOT_CHECK(image.maximum_linear_component == maximum);
  }
}

void capture_inverse_is_exact_and_classification_only_changes_decision() {
  for (bool hdr : {false, true}) {
    NativeCaptureFrame frame{};
    frame.display_id = DisplayId{1};
    frame.size_px = {3, 1};
    frame.encoding = {ColorPrimaries::display_p3, TransferFunction::extended_srgb,
        AlphaMode::opaque, 0.0};
    frame.rgba_half = {0x3c01, 0x3800, 0xb800, 0x7e00,
        0x3c01, static_cast<std::uint16_t>(hdr ? 0x3c02 : 0x3800), 0xb800, 0xfc00,
        0x3c00, 0x3800, 0x8000, 0x3c00};
    const DisplaySnapshot display{DisplayId{1}, {0,0,3,1}, 1, {3,1}, DisplayDynamicRange::hdr};
    auto interpreted = SourceColorInterpreter::interpret(frame, display);
    HDRSHOT_CHECK(interpreted && interpreted.value().capture_sdr_tolerance);
    const auto& segment = interpreted.value();
    for (std::size_t i = 0; i < frame.rgba_half.size(); ++i) {
      const float expected = i % 4 == 3 ? 1.0F : ExtendedP3Mapper::inverse_extended_srgb(
          ExtendedP3Mapper::decode_binary16(frame.rgba_half[i]).value());
      HDRSHOT_CHECK(std::bit_cast<std::uint32_t>(segment.rgba_float[i]) == std::bit_cast<std::uint32_t>(expected));
    }
    const auto original = segment.rgba_float;
    FrozenDesktop desktop{FrameId{1}, 1, {0,0,3,1}, {segment}};
    auto roi = FrameCropper::view_display(desktop, DisplayId{1}, {1,{0,0,3,1}});
    HDRSHOT_CHECK(roi && roi.value().capture_sdr_tolerance);
    AnnotationPixelPlan plan;
    plan.output_size_px = {3,1};
    plan.source_visible_spans = {{0,0,3}};
    auto fit = SourceRangeProbe::probe(roi.value(), plan);
    HDRSHOT_CHECK(fit && fit.value().fits_sdr == !hdr);
    HDRSHOT_CHECK(segment.rgba_float == original);
    auto small = FrameCropper::view_display(desktop, DisplayId{1}, {2,{0,0,1,1}});
    plan.output_size_px = {1,1}; plan.source_visible_spans = {{0,0,1}};
    fit = SourceRangeProbe::probe(small.value(), plan);
    HDRSHOT_CHECK(fit && fit.value().fits_sdr);
    frame.encoding.transfer = TransferFunction::linear;
    const auto linear = SourceColorInterpreter::interpret(frame, display);
    HDRSHOT_CHECK(linear && !linear.value().capture_sdr_tolerance);
    HDRSHOT_CHECK(linear.value().rgba_half == frame.rgba_half);
  }
}
}
int main() {
  return hdrshot::test::run({
      {"capture threshold adjacent FP32 and exact double", threshold_adjacent_samples_and_double_boundary},
      {"exact capture inverse and ROI classification", capture_inverse_is_exact_and_classification_only_changes_decision},
  });
}
