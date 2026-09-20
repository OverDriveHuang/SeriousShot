#include "domain/output/source_range_probe.hpp"
#include "test_support.hpp"

#include <cstdint>

namespace {

using namespace hdrshot;

CanonicalFrameView frame(const std::uint16_t first, const std::uint16_t second) {
  return CanonicalFrameView{
      FrameId{1},
      2,
      3,
      PixelRect{0, 0, 2, 1},
      PixelSize{2, 1},
      1.0,
      ColorEncoding{
          ColorPrimaries::display_p3,
          TransferFunction::extended_srgb,
          AlphaMode::opaque,
          0.0},
      DisplayDynamicRange::hdr,
      {first, first, first, 0x3C00, second, second, second, 0x3C00},
  };
}

AnnotationPixelPlan empty_plan() {
  const auto render_plan = AnnotationRenderPlan{0, PixelSize{2, 1}, {}};
  const auto result = AnnotationRenderPlanner::build_pixel_plan(render_plan);
  HDRSHOT_CHECK(result.has_value());
  return result.value();
}

void source_component_above_edr_one_requires_hdr() {
  const auto result = SourceRangeProbe::probe(frame(0x3800, 0x3E00), empty_plan());
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(!result.value().fits_sdr);
  HDRSHOT_CHECK(result.value().scanned_component_count == 4U);
}

void annotation_owned_hdr_source_is_skipped() {
  const auto render_plan = AnnotationRenderPlan{
      1,
      PixelSize{2, 1},
      {AnnotationCoverageLayer{
          ObjectId{4},
          AnnotationKind::rectangle,
          PixelRect{1, 0, 1, 1},
          0xFF4D67,
          {CoverageSpan{0, 1, {1U}}},
          "test",
      }},
  };
  const auto pixel_plan = AnnotationRenderPlanner::build_pixel_plan(render_plan);
  HDRSHOT_CHECK(pixel_plan.has_value());
  const auto result = SourceRangeProbe::probe(
      frame(0x3800, 0x3E00), pixel_plan.value());
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value().fits_sdr);
  HDRSHOT_CHECK(result.value().source_visible_pixel_count == 1U);
  HDRSHOT_CHECK(result.value().skipped_annotation_pixel_count == 1U);
  HDRSHOT_CHECK(result.value().scanned_component_count == 3U);
}

void proven_sdr_capture_contract_uses_zero_scan_fast_path() {
  const auto result = SourceRangeProbe::probe(
      frame(0x3E00, 0x3E00),
      empty_plan(),
      RangeProbeOptimization{true});
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value().fits_sdr);
  HDRSHOT_CHECK(result.value().scanned_component_count == 0U);
  HDRSHOT_CHECK(result.value().provenance == "capture_contract_sdr_fast_path");
}

void invalid_visible_source_component_is_an_error() {
  const auto result = SourceRangeProbe::probe(frame(0x7C00, 0x3800), empty_plan());
  HDRSHOT_CHECK(!result.has_value());
  HDRSHOT_CHECK(result.error().code == ErrorCode::invalid_color_contract);
}

void linear_source_classifies_without_transfer_curve() {
  auto source = frame(0x3400, 0x3c00);
  source.encoding.transfer = TransferFunction::linear;
  auto result = SourceRangeProbe::probe(source, empty_plan());
  HDRSHOT_CHECK(result && result.value().fits_sdr);
  source.rgba_half[4] = 0x4000;
  result = SourceRangeProbe::probe(source, empty_plan());
  HDRSHOT_CHECK(result && !result.value().fits_sdr);
}
}  // namespace

int main() {
  return hdrshot::test::run({
      {"Linear P3 range threshold", linear_source_classifies_without_transfer_curve},
      {"D6B-RANGE-001 over EDR one requires HDR", source_component_above_edr_one_requires_hdr},
      {"D6B-RANGE-002 annotation-owned source is skipped", annotation_owned_hdr_source_is_skipped},
      {"D6B-RANGE-003 proven SDR contract is zero-scan", proven_sdr_capture_contract_uses_zero_scan_fast_path},
      {"D6B-RANGE-004 invalid visible source is rejected", invalid_visible_source_component_is_an_error},
  });
}
