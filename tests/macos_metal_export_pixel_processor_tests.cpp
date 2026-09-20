#include "domain/color/extended_p3_mapper.hpp"
#include "domain/output/output_row_producer.hpp"
#include "domain/annotation/annotation_geometry.hpp"
#include "domain/output/ultra_hdr_input_renderer.hpp"
#include "platform/macos/macos_metal_export_pixel_processor.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using namespace hdrshot;

CanonicalFrameView frame(
    const std::uint16_t first,
    const std::uint16_t second) {
  return CanonicalFrameView{
      FrameId{1},
      1,
      1,
      PixelRect{0, 0, 2, 1},
      PixelSize{2, 1},
      1.0,
      ColorEncoding{
          ColorPrimaries::display_p3,
          TransferFunction::extended_srgb,
          AlphaMode::straight,
          0.0},
      DisplayDynamicRange::hdr,
      {first, first, first, 0x3C00, second, second, second, 0x3C00},
  };
}

AnnotationPixelPlan empty_plan() {
  const auto result = AnnotationRenderPlanner::build_pixel_plan(
      AnnotationRenderPlan{0, PixelSize{2, 1}, {}});
  HDRSHOT_CHECK(result.has_value());
  return result.value();
}

OutputPlan sdr_plan() {
  return OutputPlan{
      OutputClass::wide_gamut_sdr,
      EncodingIntent::wide_gamut_sdr,
      16,
      "test",
  };
}

OutputPlan hdr_plan() {
  return OutputPlan{OutputClass::hdr, EncodingIntent::hdr_pq, 16, "test"};
}

std::unique_ptr<MacMetalExportPixelProcessor> processor() {
  auto result = MacMetalExportPixelProcessor::create();
  HDRSHOT_CHECK(result.has_value());
  return std::move(result.value());
}

void sdr_gpu_matches_direct_cpu_quantization_exactly() {
  const auto source = frame(0x3800, 0x3A00);  // Extended Display P3 code 0.5, 0.75.
  const auto view = FrameCropper::view(source);
  const auto plan = empty_plan();
  const auto expected = OutputRowProducer::produce_all(
      source, plan, sdr_plan(), 100.0);
  auto metal = processor();
  const auto actual = metal->process(ExportPixelProcessRequest{
      &view, &plan, sdr_plan(), PqDiffuseWhite::nits_203});
  HDRSHOT_CHECK(expected.has_value());
  HDRSHOT_CHECK(actual.has_value());
  HDRSHOT_CHECK(actual.value().rgb_u16 == expected.value().rgb_u16);
  HDRSHOT_CHECK(actual.value().output_encoding.transfer == TransferFunction::srgb);
  HDRSHOT_CHECK(actual.value().output_encoding.source_reference_white_nits == 0.0);
  HDRSHOT_CHECK(!actual.value().content_light.has_value());
}

void hdr_gpu_matches_fp32_cpu_reference_for_both_diffuse_whites() {
  const auto source = frame(0x3800, 0x3E00);  // Extended Display P3 code 0.5, 1.5.
  const auto view = FrameCropper::view(source);
  const auto plan = empty_plan();
  for (const auto diffuse : {
           PqDiffuseWhite::nits_100,
           PqDiffuseWhite::nits_203,
       }) {
    for (const auto precision : {
             HdrPqPrecision::bits_16,
             HdrPqPrecision::bits_12,
             HdrPqPrecision::bits_10,
         }) {
      const auto nits = pq_diffuse_white_nits(diffuse);
      const auto expected = OutputRowProducer::produce_all(
          source, plan, hdr_plan(), nits, precision);
      auto metal = processor();
      const auto actual = metal->process(ExportPixelProcessRequest{
          &view, &plan, hdr_plan(), diffuse, precision});
      HDRSHOT_CHECK(expected.has_value());
      HDRSHOT_CHECK(actual.has_value());
      HDRSHOT_CHECK(actual.value().rgb_u16.size() == expected.value().rgb_u16.size());
      for (std::size_t index = 0U; index < actual.value().rgb_u16.size(); ++index) {
        const auto difference = std::abs(
            static_cast<int>(actual.value().rgb_u16[index]) -
            static_cast<int>(expected.value().rgb_u16[index]));
        HDRSHOT_CHECK(difference <= 2);
      }
      HDRSHOT_CHECK(actual.value().content_light.has_value());
      HDRSHOT_CHECK(actual.value().content_light == expected.value().content_light);
      HDRSHOT_CHECK(actual.value().output_encoding.transfer == TransferFunction::pq);
      HDRSHOT_CHECK(actual.value().output_encoding.source_reference_white_nits == nits);
    }
  }
}

void opaque_annotation_skips_hidden_non_finite_source() {
  const auto source = frame(0x7C00, 0x3C00);
  const auto view = FrameCropper::view(source);
  const auto pixel_plan = AnnotationRenderPlanner::build_pixel_plan(
      AnnotationRenderPlan{
          1,
          PixelSize{2, 1},
          {AnnotationCoverageLayer{
              ObjectId{3},
              AnnotationKind::rectangle,
              PixelRect{0, 0, 1, 1},
              0xFFFFFF,
              {CoverageSpan{0, 0, {255U}}},
              "test",
          }},
      });
  HDRSHOT_CHECK(pixel_plan.has_value());
  auto metal = processor();
  const auto actual = metal->process(ExportPixelProcessRequest{
      &view, &pixel_plan.value(), sdr_plan(), PqDiffuseWhite::nits_203});
  HDRSHOT_CHECK(actual.has_value());
  HDRSHOT_CHECK(actual.value().rgb_u16[0] == 65535U);
  HDRSHOT_CHECK(actual.value().rgb_u16[1] == 65535U);
  HDRSHOT_CHECK(actual.value().rgb_u16[2] == 65535U);
}

void hdr_annotation_is_composited_and_counted_without_reading_hidden_source() {
  const auto source = frame(0x7C00, 0x3E00);
  const auto view = FrameCropper::view(source);
  const auto pixel_plan = AnnotationRenderPlanner::build_pixel_plan(
      AnnotationRenderPlan{
          1,
          PixelSize{2, 1},
          {AnnotationCoverageLayer{
              ObjectId{3},
              AnnotationKind::rectangle,
              PixelRect{0, 0, 1, 1},
              0xFF0000,
              {CoverageSpan{0, 0, {255U}}},
              "test",
          }},
      });
  HDRSHOT_CHECK(pixel_plan.has_value());
  const auto expected = OutputRowProducer::produce_all(
      source, pixel_plan.value(), hdr_plan(), 203.0,
      HdrPqPrecision::bits_12);
  auto metal = processor();
  const auto actual = metal->process(ExportPixelProcessRequest{
      &view,
      &pixel_plan.value(),
      hdr_plan(),
      PqDiffuseWhite::nits_203,
      HdrPqPrecision::bits_12,
  });
  HDRSHOT_CHECK(expected.has_value());
  HDRSHOT_CHECK(actual.has_value());
  HDRSHOT_CHECK(actual.value().rgb_u16.size() == expected.value().rgb_u16.size());
  for (std::size_t index = 0U; index < actual.value().rgb_u16.size(); ++index) {
    const auto difference = std::abs(
        static_cast<int>(actual.value().rgb_u16[index]) -
        static_cast<int>(expected.value().rgb_u16[index]));
    HDRSHOT_CHECK(difference <= 2);
  }
  HDRSHOT_CHECK(actual.value().content_light == expected.value().content_light);
}

void malformed_pixel_plan_is_rejected_before_gpu_execution() {
  const auto source = frame(0x3800, 0x3800);
  const auto view = FrameCropper::view(source);
  const auto overlapping = AnnotationPixelPlan{
      0,
      PixelSize{2, 1},
      {SourceVisibleSpan{0, 0, 2}},
      {AnnotationOwnedSpan{0, 1, 1, ObjectId{1}, 0xFFFFFF}},
  };
  auto metal = processor();
  const auto actual = metal->process(ExportPixelProcessRequest{
      &view, &overlapping, sdr_plan(), PqDiffuseWhite::nits_203});
  HDRSHOT_CHECK(!actual.has_value());
  HDRSHOT_CHECK(actual.error().code == ErrorCode::state_inconsistent);
  HDRSHOT_CHECK(actual.error().safe_context.at("reason") == "invalid_pixel_plan");
}

void ultra_hdr_renderer_matches_cpu_linear_p3_reference() {
  const auto source = frame(0x3800, 0x3E00);  // Extended Display P3 0.5, 1.5.
  const auto view = FrameCropper::view(source);
  const auto plan = empty_plan();
  CpuUltraHdrInputRenderer cpu;
  const auto expected = cpu.render(UltraHdrInputRenderRequest{
      &view, &plan, kUltraHdrReferenceWhiteNits});
  auto metal = processor();
  const auto actual = metal->render(UltraHdrInputRenderRequest{
      &view, &plan, kUltraHdrReferenceWhiteNits});
  HDRSHOT_CHECK(expected.has_value());
  HDRSHOT_CHECK(actual.has_value());
  HDRSHOT_CHECK(actual.value().size_px == expected.value().size_px);
  HDRSHOT_CHECK(actual.value().reference_white_nits == 203.0);
  HDRSHOT_CHECK(actual.value().rgba_half.size() == expected.value().rgba_half.size());
  for (std::size_t index = 0U; index < actual.value().rgba_half.size(); ++index) {
    const auto expected_value = ExtendedP3Mapper::decode_binary16(
        expected.value().rgba_half[index]);
    const auto actual_value = ExtendedP3Mapper::decode_binary16(
        actual.value().rgba_half[index]);
    HDRSHOT_CHECK(expected_value.has_value() && actual_value.has_value());
    HDRSHOT_CHECK(
        std::abs(actual_value.value() - expected_value.value()) < 0.003F);
  }
  HDRSHOT_CHECK(
      std::abs(actual.value().maximum_linear_component -
               expected.value().maximum_linear_component) < 0.003);
}

void partial_coverage_matches_cpu() {
  const auto high=ExtendedP3Mapper::encode_binary16(ExtendedP3Mapper::encode_extended_srgb(4.0F));
  auto source=frame(high,0x3800);
  const auto view=FrameCropper::view(source);
  auto metal=processor();
  for(const auto amount : {1U,64U,128U,192U,255U}) {
    const auto plan=AnnotationRenderPlanner::build_pixel_plan({1,{2,1},
        {{ObjectId{1},AnnotationKind::text,{0,0,1,1},0xFFFFFF,
          {{0,0,{static_cast<std::uint8_t>(amount)}}},"AA"}}}).value();
    for(const auto output : {sdr_plan(),hdr_plan()}) {
      const auto expected=OutputRowProducer::produce_all(source,plan,output,203);
      const auto actual=metal->process({&view,&plan,output,PqDiffuseWhite::nits_203,HdrPqPrecision::bits_16});
      HDRSHOT_CHECK(expected.has_value() && actual.has_value());
      for(std::size_t i=0;i<expected.value().rgb_u16.size();++i)
        HDRSHOT_CHECK(std::abs(int(expected.value().rgb_u16[i])-int(actual.value().rgb_u16[i]))<=2);
      if(actual.value().content_light)
        HDRSHOT_CHECK(std::abs(double(actual.value().content_light->max_cll_x10000)-
            double(expected.value().content_light.max_cll_x10000))<=16);
    }
    CpuUltraHdrInputRenderer cpu;
    const auto expected=cpu.render({&view,&plan,203});
    const auto actual=metal->render({&view,&plan,203});
    HDRSHOT_CHECK(expected.has_value() && actual.has_value());
    HDRSHOT_CHECK(jpeg_output_kind(actual.value())==JpegOutputKind::display_p3_sdr);
    for(std::size_t i=0;i<actual.value().rgba_half.size();++i)
      HDRSHOT_CHECK(std::abs(ExtendedP3Mapper::decode_binary16(actual.value().rgba_half[i]).value()-
          ExtendedP3Mapper::decode_binary16(expected.value().rgba_half[i]).value())<0.003F);
    HDRSHOT_CHECK(std::abs(actual.value().maximum_linear_component-expected.value().maximum_linear_component)<0.003);
  }
  source.rgba_half[0]=0x7E00;
  for(const auto amount : {128U,255U}) {
    const auto plan=AnnotationRenderPlanner::build_pixel_plan({1,{2,1},
        {{ObjectId{1},AnnotationKind::text,{0,0,1,1},0xFFFFFF,
          {{0,0,{static_cast<std::uint8_t>(amount)}}},"AA"}}}).value();
    HDRSHOT_CHECK(metal->process({&view,&plan,hdr_plan(),PqDiffuseWhite::nits_203}).has_value()==(amount==255));
    HDRSHOT_CHECK(metal->render({&view,&plan,203}).has_value()==(amount==255));
  }
}

void rotated_coverage_drives_same_png_and_jpeg_pixels_on_gpu() {
  AnnotationObject object{ObjectId{70},AnnotationKind::ellipse,{20,30,50,20},
      ShapeStyle{0xFF4D67,6},{},std::nullopt};
  object.transform={0.71,0.25,0.5};
  const auto coverage=AnnotationRenderPlanner::build({1,{object},object.id},{0,0,96,96},1);
  HDRSHOT_CHECK(coverage.has_value());
  const auto pixel_plan=AnnotationRenderPlanner::build_pixel_plan(coverage.value()).value();
  HDRSHOT_CHECK(std::any_of(pixel_plan.annotation_owned_spans.begin(),pixel_plan.annotation_owned_spans.end(),
      [](const auto& span) { return !span.edge_samples.empty(); }));
  auto metal=processor();
  for (bool hdr : {false,true}) {
    auto source=frame(0x3800,0x3E00);
    source.size_px={96,96};
    source.source_rect_px={0,0,96,96};
    source.rgba_half.clear();
    const std::uint16_t code=hdr?0x3E00:0x3800;
    for(int i=0;i<96*96;++i) source.rgba_half.insert(source.rgba_half.end(),{code,code,code,0x3C00});
    const auto view=FrameCropper::view(source);
    const auto output=hdr?hdr_plan():sdr_plan();
    const auto expected=OutputRowProducer::produce_all(source,pixel_plan,output,203);
    const auto actual=metal->process({&view,&pixel_plan,output,PqDiffuseWhite::nits_203,HdrPqPrecision::bits_16});
    HDRSHOT_CHECK(expected && actual);
    HDRSHOT_CHECK(actual.value().rgb_u16.size()==expected.value().rgb_u16.size());
    for(std::size_t i=0;i<actual.value().rgb_u16.size();++i) {
      const auto difference=std::abs(static_cast<int>(actual.value().rgb_u16[i])-static_cast<int>(expected.value().rgb_u16[i]));
      if(difference>2) std::cerr << "rotation parity hdr=" << hdr << " sample=" << i
          << " GPU=" << actual.value().rgb_u16[i] << " CPU=" << expected.value().rgb_u16[i] << '\n';
      HDRSHOT_CHECK(difference<=2);
    }
    CpuUltraHdrInputRenderer cpu;
    const auto jpeg_expected=cpu.render({&view,&pixel_plan,203});
    const auto jpeg_actual=metal->render({&view,&pixel_plan,203});
    HDRSHOT_CHECK(jpeg_expected && jpeg_actual);
    HDRSHOT_CHECK(jpeg_output_kind(jpeg_expected.value())==jpeg_output_kind(jpeg_actual.value()));
    for(std::size_t i=0;i<jpeg_actual.value().rgba_half.size();++i)
      HDRSHOT_CHECK(std::abs(ExtendedP3Mapper::decode_binary16(jpeg_actual.value().rgba_half[i]).value()-
          ExtendedP3Mapper::decode_binary16(jpeg_expected.value().rgba_half[i]).value())<0.003F);
  }
}

void linear_source_png_jpeg_and_aa_match_reference() {
  auto metal = processor();
  CpuUltraHdrInputRenderer cpu;
  for (const float edr : {0.25F, 1.0F, 2.0F, 4.0F, 8.0F}) {
    auto source = frame(ExtendedP3Mapper::encode_binary16(edr), 0x3400);
    source.encoding.transfer = TransferFunction::linear;
    const auto view = FrameCropper::view(source);
    for (bool aa : {false, true}) {
      auto plan = empty_plan();
      if (aa) {
        plan.source_visible_spans = {{0,0,1}};
        plan.annotation_owned_spans = {{0,1,1,ObjectId{1},0xffffff,{{0.5F,0.5F,0.5F,0.5F}}}};
      }
      for (auto output : {sdr_plan(), hdr_plan()}) {
        for (auto white : {PqDiffuseWhite::nits_100, PqDiffuseWhite::nits_203})
          for (auto precision : {HdrPqPrecision::bits_10, HdrPqPrecision::bits_12, HdrPqPrecision::bits_16}) {
            auto expected = OutputRowProducer::produce_all(source, plan, output,
                pq_diffuse_white_nits(white), precision);
            auto actual = metal->process({&view, &plan, output, white, precision});
            HDRSHOT_CHECK(expected && actual);
            for (std::size_t i=0;i<actual.value().rgb_u16.size();++i)
              HDRSHOT_CHECK(std::abs(int(actual.value().rgb_u16[i])-int(expected.value().rgb_u16[i])) <= 2);
            if (output.encoding_intent == EncodingIntent::hdr_pq) {
              HDRSHOT_CHECK(actual.value().content_light.has_value());
              HDRSHOT_CHECK(actual.value().content_light == expected.value().content_light);
            } else {
              HDRSHOT_CHECK(!actual.value().content_light);
              if (edr == 0.25F) HDRSHOT_CHECK(actual.value().rgb_u16[0] > 35000);
            }
          }
      }
      auto expected = cpu.render({&view, &plan, 203});
      auto actual = metal->render({&view, &plan, 203});
      HDRSHOT_CHECK(expected && actual);
      HDRSHOT_CHECK(actual.value().rgba_half == expected.value().rgba_half);
      HDRSHOT_CHECK(actual.value().maximum_linear_component == expected.value().maximum_linear_component);
      HDRSHOT_CHECK(actual.value().rgba_half[0] == ExtendedP3Mapper::encode_binary16(edr));
    }
  }
}

void encoded_sdr_png_and_jpeg_do_not_apply_a_second_oetf() {
  auto source = frame(0x3800, 0x3800); // Both are encoded P3 0.5, not linear 0.5.
  source.encoding.alpha = AlphaMode::opaque;
  const auto view = FrameCropper::view(source);
  auto plan = empty_plan();
  plan.source_visible_spans = {{0,0,1}};
  plan.annotation_owned_spans = {{0,1,1,ObjectId{1},0xffffff,{{0.5F,0.5F,0.5F,0.5F}}}};
  auto metal = processor();
  const auto png = metal->process({&view, &plan, sdr_plan(), PqDiffuseWhite::nits_203});
  const auto png_reference = OutputRowProducer::produce_all(source, plan, sdr_plan(), 203);
  HDRSHOT_CHECK(png && png_reference);
  HDRSHOT_CHECK(png.value().rgb_u16[0] == 32768);
  for (std::size_t i=0;i<png.value().rgb_u16.size();++i)
    HDRSHOT_CHECK(std::abs(int(png.value().rgb_u16[i])-int(png_reference.value().rgb_u16[i]))<=2);

  CpuUltraHdrInputRenderer cpu;
  const auto jpeg = metal->render({&view, &plan, 203});
  const auto jpeg_reference = cpu.render({&view, &plan, 203});
  HDRSHOT_CHECK(jpeg && jpeg_reference);
  // Decode before blending: the unannotated pixel is 0.214; a half-covered
  // white AA pixel is 0.5 + 0.5*0.214, not decode(0.75) or raw 0.75.
  HDRSHOT_CHECK_NEAR(ExtendedP3Mapper::decode_binary16(jpeg.value().rgba_half[0]).value(),
      0.21404114F, 0.0003F);
  HDRSHOT_CHECK_NEAR(ExtendedP3Mapper::decode_binary16(jpeg.value().rgba_half[4]).value(),
      0.60702057F, 0.0005F);
  HDRSHOT_CHECK(jpeg.value().rgba_half == jpeg_reference.value().rgba_half);
}

}  // namespace

int main() {
  const auto availability = hdrshot::MacMetalExportPixelProcessor::create();
  if (!availability) {
    const auto reason = availability.error().safe_context.find("reason");
    if (reason != availability.error().safe_context.end() &&
        reason->second == "metal_device_unavailable") {
      std::cout << "[SKIP] Metal device unavailable in the current session\n";
      return 77;
    }
    std::cerr << "Metal export preflight failed: "
              << hdrshot::to_string(availability.error().code) << '\n';
    for (const auto& [key, value] : availability.error().safe_context) {
      std::cerr << "  " << key << '=' << value << '\n';
    }
    return 1;
  }
  return hdrshot::test::run({
      {"Encoded SDR PNG/JPEG and AA use one inverse transfer", encoded_sdr_png_and_jpeg_do_not_apply_a_second_oetf},
      {"Linear P3 PNG/JPEG and AA do not double decode", linear_source_png_jpeg_and_aa_match_reference},
      {"rotated coverage matches CPU for PNG and JPEG",rotated_coverage_drives_same_png_and_jpeg_pixels_on_gpu},
      {"AA CPU/Metal parity and source-read rules", partial_coverage_matches_cpu},
      {"Metal SDR matches direct Display P3 quantization", sdr_gpu_matches_direct_cpu_quantization_exactly},
      {"Metal HDR matches FP32 100/203 nit reference", hdr_gpu_matches_fp32_cpu_reference_for_both_diffuse_whites},
      {"Metal skips annotation-hidden source", opaque_annotation_skips_hidden_non_finite_source},
      {"Metal HDR counts opaque annotation", hdr_annotation_is_composited_and_counted_without_reading_hidden_source},
      {"Metal rejects overlapping pixel plan", malformed_pixel_plan_is_rejected_before_gpu_execution},
      {"Metal Ultra HDR input matches CPU linear P3 reference",
       ultra_hdr_renderer_matches_cpu_linear_p3_reference},
  });
}
