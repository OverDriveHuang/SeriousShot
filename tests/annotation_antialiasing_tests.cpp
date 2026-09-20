#include "domain/annotation/annotation_compositing.hpp"
#include "domain/output/output_row_producer.hpp"
#include "domain/output/source_range_probe.hpp"
#include "domain/output/ultra_hdr_input_renderer.hpp"
#include "test_support.hpp"
#include <cmath>
#include <limits>

namespace {
using namespace hdrshot;
CanonicalFrameView frame(float linear) {
  CanonicalFrameView out{FrameId{1}, 1, 1, {0, 0, 5, 1}, {5, 1}, 1.0,
      {ColorPrimaries::display_p3, TransferFunction::extended_srgb, AlphaMode::straight, 0.0},
      DisplayDynamicRange::hdr, {}};
  const auto bits = ExtendedP3Mapper::encode_binary16(ExtendedP3Mapper::encode_extended_srgb(linear));
  for (int i = 0; i < 5; ++i) out.rgba_half.insert(out.rgba_half.end(), {bits, bits, bits, 0x3C00});
  return out;
}
AnnotationRenderPlan coverage(std::vector<std::uint8_t> values = {0,64,128,192,255}) {
  return {1, {5,1}, {{ObjectId{1}, AnnotationKind::text, {0,0,5,1},
      0xFFFFFF, {{0,0,std::move(values)}}, "coverage-test"}}};
}
AnnotationPixelPlan pixel_plan(const AnnotationRenderPlan& render) {
  auto result = AnnotationRenderPlanner::build_pixel_plan(render);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(AnnotationPixelPlanValidator::valid(result.value()));
  return result.value();
}
void grayscale_coverage_is_not_binarized() {
  const auto plan = pixel_plan(coverage());
  const auto upload = prepare_annotation_gpu_upload(plan);
  HDRSHOT_CHECK(upload.has_value());
  HDRSHOT_CHECK(upload.value().indices[0] == 0);
  const auto source = frame(0);
  const auto output = OutputRowProducer::produce_all(source, plan,
      {OutputClass::wide_gamut_sdr, EncodingIntent::wide_gamut_sdr,16,"aa"});
  HDRSHOT_CHECK(output.has_value());
  const std::array<int,5> amounts{0,64,128,192,255};
  for (std::size_t x=0; x<5; ++x) {
    const auto expected = ExtendedP3Mapper::quantize_unorm16(
        ExtendedP3Mapper::encode_extended_srgb(static_cast<float>(amounts[x])/255.0F));
    HDRSHOT_CHECK(std::abs(int(output.value().rgb_u16[x*3])-int(expected)) <= 1);
    if (x>0 && x<4) {
      const auto sample = upload.value().samples[upload.value().indices[x]-1];
      HDRSHOT_CHECK(std::abs(sample[3]-(1.0F-float(amounts[x])/255.0F)) < 1e-6F);
    }
  }
}
void overlapping_layers_keep_order_and_opaque_fast_path() {
  auto render = coverage({0,128,255,128,255});
  render.ordered_layers[0].color_srgb_rgb = 0xFF0000;
  render.ordered_layers.push_back({ObjectId{2},AnnotationKind::rectangle,{0,0,5,1},
      0x0000FF,{{0,0,{0,128,128,255,0}}},"second"});
  const auto upload = prepare_annotation_gpu_upload(pixel_plan(render));
  HDRSHOT_CHECK(upload.has_value());
  const auto red = ExtendedP3Mapper::annotation_linear_display_p3(0xFF0000);
  const auto blue = ExtendedP3Mapper::annotation_linear_display_p3(0x0000FF);
  const float a=128.0F/255.0F;
  const auto mixed=upload.value().samples[upload.value().indices[1]-1];
  for(std::size_t c=0;c<3;++c)
    HDRSHOT_CHECK(std::abs(mixed[c]-(a*blue[c]+(1-a)*a*red[c]))<1e-6F);
  HDRSHOT_CHECK(std::abs(mixed[3]-(1-a)*(1-a))<1e-6F);
  const auto over_solid=upload.value().samples[upload.value().indices[2]-1];
  HDRSHOT_CHECK(over_solid[3]==0);
  const auto solid=upload.value().samples[upload.value().indices[3]-1];
  HDRSHOT_CHECK(solid[3]==0);
  for(std::size_t c=0;c<3;++c) HDRSHOT_CHECK(solid[c]==blue[c]);
}
void edge_only_hdr_is_sdr_by_explicit_policy() {
  auto source=frame(4);
  for(std::size_t c=0;c<3;++c) source.rgba_half[c]=0x3800;
  const auto plan=pixel_plan(coverage());
  const auto range=SourceRangeProbe::probe(source,plan);
  HDRSHOT_CHECK(range.has_value() && range.value().fits_sdr);
  HDRSHOT_CHECK(range.value().source_visible_pixel_count==1);
  HDRSHOT_CHECK(range.value().skipped_annotation_pixel_count==4);
  const auto sdr=OutputRowProducer::produce_all(source,plan,
      {OutputClass::wide_gamut_sdr,EncodingIntent::wide_gamut_sdr,16,"aa"});
  HDRSHOT_CHECK(sdr.has_value());
  HDRSHOT_CHECK(sdr.value().rgb_u16[3]==65535);
  CpuUltraHdrInputRenderer renderer;
  const auto view=FrameCropper::view(source);
  const auto jpeg=renderer.render({&view,&plan,203});
  HDRSHOT_CHECK(jpeg.has_value());
  HDRSHOT_CHECK(jpeg.value().maximum_linear_component>1);
  HDRSHOT_CHECK(*jpeg.value().source_visible_maximum_linear_component<1);
  HDRSHOT_CHECK(jpeg_output_kind(jpeg.value())==JpegOutputKind::display_p3_sdr);
}
void hdr_light_statistics_include_composited_edges() {
  auto source=frame(4);
  // Uncovered source keeps HDR selected, but its peak is below a bright edge.
  const auto low=ExtendedP3Mapper::encode_binary16(ExtendedP3Mapper::encode_extended_srgb(1.5F));
  for(std::size_t c=0;c<3;++c) source.rgba_half[c]=low;
  const auto plan=pixel_plan(coverage());
  HDRSHOT_CHECK(!SourceRangeProbe::probe(source,plan).value().fits_sdr);
  const auto output=OutputRowProducer::produce_all(source,plan,
      {OutputClass::hdr,EncodingIntent::hdr_pq,16,"aa"},203);
  HDRSHOT_CHECK(output.has_value());
  const float underlying=ExtendedP3Mapper::inverse_extended_srgb(
      ExtendedP3Mapper::decode_binary16(source.rgba_half[4]).value());
  const double expected=(64.0F/255.0F+(1-64.0F/255.0F)*underlying)*203;
  HDRSHOT_CHECK(std::abs(double(output.value().content_light.max_cll_x10000)/10000-expected)<0.001);
  HDRSHOT_CHECK(output.value().content_light.pixel_count==5);
}
void hidden_nan_is_ignored_only_when_fully_covered() {
  auto source=frame(0);
  source.rgba_half[4]=0x7E00;
  const OutputPlan output{OutputClass::hdr,EncodingIntent::hdr_pq,16,"aa"};
  auto plan=pixel_plan(coverage({0,255,255,255,255}));
  HDRSHOT_CHECK(OutputRowProducer::produce_all(source,plan,output).has_value());
  plan=pixel_plan(coverage());
  HDRSHOT_CHECK(!OutputRowProducer::produce_all(source,plan,output).has_value());
  auto malformed=plan;
  malformed.annotation_owned_spans[0].edge_samples[0][3]=std::numeric_limits<float>::quiet_NaN();
  HDRSHOT_CHECK(!AnnotationPixelPlanValidator::valid(malformed));
  HDRSHOT_CHECK(!prepare_annotation_gpu_upload(malformed).has_value());
}
}
int main() {
  return hdrshot::test::run({
    {"AA preserves grayscale coverage",grayscale_coverage_is_not_binarized},
    {"AA overlap and opaque fast path",overlapping_layers_keep_order_and_opaque_fast_path},
    {"AA edge-only HDR deliberately clips to SDR",edge_only_hdr_is_sdr_by_explicit_policy},
    {"AA HDR content-light includes final edges",hdr_light_statistics_include_composited_edges},
    {"AA hidden source and malformed coverage",hidden_nan_is_ignored_only_when_fully_covered}});
}
