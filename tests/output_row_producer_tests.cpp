#include "domain/color/pq_reference_white_mapper.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "domain/output/output_row_producer.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cmath>
#include <limits>
#include <utility>

namespace {

using namespace hdrshot;

CanonicalFrameView frame(const std::uint16_t first, const std::uint16_t second) {
  return CanonicalFrameView{
      FrameId{5},
      6,
      7,
      PixelRect{0, 0, 2, 1},
      PixelSize{2, 1},
      1.0,
      ColorEncoding{
          ColorPrimaries::display_p3,
          TransferFunction::extended_srgb,
          AlphaMode::straight,
          0.0},
      DisplayDynamicRange::hdr,
      {first, first, first, 0x0000, second, second, second, 0x3C00},
  };
}

AnnotationPixelPlan empty_plan() {
  const auto result = AnnotationRenderPlanner::build_pixel_plan(
      AnnotationRenderPlan{0, PixelSize{2, 1}, {}});
  HDRSHOT_CHECK(result.has_value());
  return result.value();
}

OutputPlan hdr_plan() {
  return OutputPlan{OutputClass::hdr, EncodingIntent::hdr_pq, 16, "test"};
}

OutputPlan sdr_plan() {
  return OutputPlan{
      OutputClass::wide_gamut_sdr,
      EncodingIntent::wide_gamut_sdr,
      16,
      "test",
  };
}

void hdr_row_linearizes_extended_p3_and_uses_selected_diffuse_white() {
  const auto result = OutputRowProducer::produce_row(
      frame(0x0000, 0x3E00), empty_plan(), hdr_plan(), 0, 203.0);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value().rgb_u16.size() == 6U);
  const auto black = PqReferenceWhiteMapper::map_linear_edr(0.0, 203.0);
  const auto white = PqReferenceWhiteMapper::map_linear_edr(
      ExtendedP3Mapper::inverse_extended_srgb(1.5F), 203.0);
  HDRSHOT_CHECK(black.has_value());
  HDRSHOT_CHECK(white.has_value());
  for (std::size_t channel = 0U; channel < 3U; ++channel) {
    HDRSHOT_CHECK(result.value().rgb_u16[channel] == black.value().png_u16);
    HDRSHOT_CHECK(result.value().rgb_u16[3U + channel] == white.value().png_u16);
  }
}

void sdr_row_directly_quantizes_extended_p3_code_and_ignores_diffuse_white() {
  const auto at_100 = OutputRowProducer::produce_row(
      frame(0x0000, 0x3800), empty_plan(), sdr_plan(), 0, 100.0);
  const auto at_203 = OutputRowProducer::produce_row(
      frame(0x0000, 0x3800), empty_plan(), sdr_plan(), 0, 203.0);
  const auto at_invalid = OutputRowProducer::produce_row(
      frame(0x0000, 0x3800), empty_plan(), sdr_plan(), 0,
      std::numeric_limits<double>::quiet_NaN());
  HDRSHOT_CHECK(at_100.has_value());
  HDRSHOT_CHECK(at_203.has_value());
  HDRSHOT_CHECK(at_invalid.has_value());
  HDRSHOT_CHECK(at_100.value().rgb_u16 == at_203.value().rgb_u16);
  HDRSHOT_CHECK(at_100.value().rgb_u16 == at_invalid.value().rgb_u16);
  HDRSHOT_CHECK(at_100.value().rgb_u16[0] == 0U);
  HDRSHOT_CHECK(at_100.value().rgb_u16[3] == 32768U);
  HDRSHOT_CHECK(at_100.value().clipped_channel_count == 0U);
}

void opaque_annotation_row_does_not_read_hidden_invalid_source_or_alpha() {
  const auto render_plan = AnnotationRenderPlan{
      2,
      PixelSize{2, 1},
      {AnnotationCoverageLayer{
          ObjectId{9}, AnnotationKind::rectangle, PixelRect{0, 0, 1, 1},
          0xFFFFFF, {CoverageSpan{0, 0, {255U}}}, "test"}},
  };
  const auto pixel_plan = AnnotationRenderPlanner::build_pixel_plan(render_plan);
  HDRSHOT_CHECK(pixel_plan.has_value());
  const auto result = OutputRowProducer::produce_all(
      frame(0x7C00, 0x3800), pixel_plan.value(), sdr_plan());
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value().encoding.alpha == AlphaMode::opaque);
  HDRSHOT_CHECK(result.value().rgb_u16.size() == 6U);
  HDRSHOT_CHECK(result.value().rgb_u16[0] == 65535U);
  HDRSHOT_CHECK(result.value().rgb_u16[1] == 65535U);
  HDRSHOT_CHECK(result.value().rgb_u16[2] == 65535U);
}

void hdr_precision_quantizes_only_pq_codes_and_preserves_linear_statistics() {
  const auto source = frame(0x3800, 0x3E00);
  const auto plan = empty_plan();
  const auto at_16 = OutputRowProducer::produce_all(
      source, plan, hdr_plan(), 203.0, HdrPqPrecision::bits_16);
  const auto at_12 = OutputRowProducer::produce_all(
      source, plan, hdr_plan(), 203.0, HdrPqPrecision::bits_12);
  const auto at_10 = OutputRowProducer::produce_all(
      source, plan, hdr_plan(), 203.0, HdrPqPrecision::bits_10);
  HDRSHOT_CHECK(at_16.has_value());
  HDRSHOT_CHECK(at_12.has_value());
  HDRSHOT_CHECK(at_10.has_value());
  HDRSHOT_CHECK(at_16.value().rgb_u16 != at_12.value().rgb_u16);
  HDRSHOT_CHECK(at_12.value().rgb_u16 != at_10.value().rgb_u16);
  HDRSHOT_CHECK(at_16.value().content_light == at_12.value().content_light);
  HDRSHOT_CHECK(at_12.value().content_light == at_10.value().content_light);
  HDRSHOT_CHECK(at_10.value().content_light.pixel_count == 2U);

  for (const auto [samples, maximum_code] : {
           std::pair{&at_12.value().rgb_u16, 4095U},
           std::pair{&at_10.value().rgb_u16, 1023U},
       }) {
    for (const auto sample : *samples) {
      const auto effective = static_cast<std::uint32_t>(std::floor(
          static_cast<double>(sample) * maximum_code / 65535.0 + 0.5));
      const auto expanded = static_cast<std::uint16_t>(std::floor(
          static_cast<double>(effective) * 65535.0 / maximum_code + 0.5));
      HDRSHOT_CHECK(sample == expanded);
    }
  }

  const auto levels = finalize_content_light(at_10.value().content_light);
  HDRSHOT_CHECK(levels.has_value());
  HDRSHOT_CHECK(levels->max_fall_x10000 <= levels->max_cll_x10000);
  HDRSHOT_CHECK(levels->max_fall_x10000 > 0U);
}

void overlapping_spans_with_correct_total_count_are_rejected() {
  auto plan = empty_plan();
  plan.source_visible_spans = {{0, 0, 1}, {0, 0, 1}};
  const auto result = OutputRowProducer::produce_row(
      frame(0x3800, 0x3800), plan, sdr_plan(), 0);
  HDRSHOT_CHECK(!result.has_value());
  HDRSHOT_CHECK(result.error().code == ErrorCode::state_inconsistent);
}

}  // namespace

int main() {
  return hdrshot::test::run({
      {"D6C-ROW-005 overlaps cannot substitute for missing pixels", overlapping_spans_with_correct_total_count_are_rejected},
      {"D6C-ROW-001 HDR row linearizes Extended P3", hdr_row_linearizes_extended_p3_and_uses_selected_diffuse_white},
      {"D6C-ROW-002 SDR row directly quantizes Display P3", sdr_row_directly_quantizes_extended_p3_code_and_ignores_diffuse_white},
      {"D6C-ROW-003 annotation row skips hidden source", opaque_annotation_row_does_not_read_hidden_invalid_source_or_alpha},
      {"D6C-ROW-004 HDR precision preserves content light", hdr_precision_quantizes_only_pq_codes_and_preserves_linear_statistics},
  });
}
