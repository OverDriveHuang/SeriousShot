#include "platform/windows/windows_d3d_export.hpp"
#include "platform/windows/windows_color.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "domain/color/pq_reference_white_mapper.hpp"
#include "test_support.hpp"
#include <iostream>

using namespace hdrshot;
namespace {
std::unique_ptr<WindowsD3DExportPixelProcessor> gpu;
CanonicalFrameView frame(std::initializer_list<float> values) {
  CanonicalFrameView out{};
  out.size_px={static_cast<int>(values.size()),1};
  out.encoding=WindowsColor::linear_p3_encoding();
  for(auto v:values) { auto h=ExtendedP3Mapper::encode_binary16(v); out.rgba_half.insert(out.rgba_half.end(),{h,h,h,0x3c00}); }
  return out;
}
AnnotationPixelPlan plan(PixelSize size) {
  AnnotationPixelPlan p{0,size,{},{}};
  for(int y=0;y<size.height;++y) p.source_visible_spans.push_back({y,0,size.width});
  return p;
}
void sdr_encoding_and_no_clli() {
  auto source=frame({0,0.003F,0.18F,0.5F,1.0F,-0.2F});
  auto roi=FrameCropper::view(source); auto coverage=plan(source.size_px);
  const auto result=gpu->process({&roi,&coverage,{},PqDiffuseWhite::nits_203,HdrPqPrecision::bits_10});
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(!result.value().content_light);
  for(std::size_t i=0;i<6;++i) {
    const auto linear=ExtendedP3Mapper::decode_binary16(source.rgba_half[i*4]).value();
    const auto expected=ExtendedP3Mapper::quantize_unorm16(ExtendedP3Mapper::encode_extended_srgb(linear));
    HDRSHOT_CHECK_NEAR(result.value().rgb_u16[i*3],expected,2);
  }
}
void hdr_pq_precision_and_clli() {
  auto source=frame({0,0.18F,0.5F,1.0F,2.0F,5.0F,64.0F});
  auto roi=FrameCropper::view(source); auto coverage=plan(source.size_px);
  const OutputPlan output{OutputClass::hdr,EncodingIntent::hdr_pq,16,"test"};
  for(auto white:{PqDiffuseWhite::nits_100,PqDiffuseWhite::nits_203}) {
    for(auto bits:{HdrPqPrecision::bits_10,HdrPqPrecision::bits_12,HdrPqPrecision::bits_16}) {
      const auto result=gpu->process({&roi,&coverage,output,white,bits});
      HDRSHOT_CHECK(result.has_value());
      for(std::size_t i=0;i<7;++i) {
        const auto linear=ExtendedP3Mapper::decode_binary16(source.rgba_half[i*4]).value();
        const auto expected=PqReferenceWhiteMapper::map_linear_edr(linear,pq_diffuse_white_nits(white),bits).value().png_u16;
        const double tolerance=bits==HdrPqPrecision::bits_10?65:(bits==HdrPqPrecision::bits_12?17:8);
        HDRSHOT_CHECK_NEAR(result.value().rgb_u16[i*3],expected,tolerance);
      }
      HDRSHOT_CHECK(result.value().content_light->pixel_count==7);
      const auto expected_max=content_light_units(64*pq_diffuse_white_nits(white));
      HDRSHOT_CHECK_NEAR(result.value().content_light->max_cll_x10000,expected_max,16);
      HDRSHOT_CHECK(result.value().luminance_clip.clipped_pixel_count==(white==PqDiffuseWhite::nits_203?1:0));
    }
  }
}
void clli_carry_and_partial_workgroup() {
  auto source=frame({30}); source.size_px={257,1}; source.rgba_half.resize(257*4,0x4f80);
  auto roi=FrameCropper::view(source); auto coverage=plan(source.size_px);
  const auto result=gpu->process({&roi,&coverage,{OutputClass::hdr,EncodingIntent::hdr_pq,16,"test"},PqDiffuseWhite::nits_203,HdrPqPrecision::bits_16});
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value().content_light->pixel_count==257);
  const auto expected=static_cast<std::uint64_t>(content_light_units(30*203))*257;
  HDRSHOT_CHECK_NEAR(static_cast<double>(result.value().content_light->luminance_sum_x10000),static_cast<double>(expected),257*16);
}
void uhdr_aa_ownership_and_invalid_source() {
  auto source=frame({0.5F,2.0F,std::numeric_limits<float>::quiet_NaN()});
  auto roi=FrameCropper::view(source);
  AnnotationPixelPlan coverage{0,{3,1},{{0,0,1}},
      {{0,1,1,ObjectId{1},0xff0000,{{{0.4F,0,0,0.5F}}}}, {0,2,1,ObjectId{2},0xffffff,{}}}};
  auto result=gpu->render({&roi,&coverage});
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK_NEAR(*result.value().source_visible_maximum_linear_component,0.5,1e-6);
  HDRSHOT_CHECK_NEAR(result.value().maximum_linear_component,1.4,1e-6);
  HDRSHOT_CHECK(jpeg_output_kind(result.value())==JpegOutputKind::display_p3_sdr);
  HDRSHOT_CHECK_NEAR(ExtendedP3Mapper::decode_binary16(result.value().rgba_half[4]).value(),1.4,0.001);
  coverage.annotation_owned_spans[1].edge_samples={{{0.1F,0,0,0.5F}}};
  HDRSHOT_CHECK(!gpu->render({&roi,&coverage}));
  roi.encoding.transfer=TransferFunction::extended_srgb;
  HDRSHOT_CHECK(!gpu->render({&roi,&coverage}));
}
}
int main() {
  auto result=WindowsD3DExportPixelProcessor::create();
  if(!result) {
    for(auto& [k,v]:result.error().safe_context) std::cerr<<k<<'='<<v<<' ';
    std::cerr<<'\n'; return 1;
  }
  gpu=std::move(result.value());
  return hdrshot::test::run({{"SDR transfer and no cLLI",sdr_encoding_and_no_clli},
      {"HDR precision and clipping",hdr_pq_precision_and_clli},
      {"cLLI 64-bit carry and partial group",clli_carry_and_partial_workgroup},
      {"UHDR AA coverage and nonfinite rejection",uhdr_aa_ownership_and_invalid_source}});
}
