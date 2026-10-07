#include "platform/windows/windows_d3d_export.hpp"
#include "platform/windows/windows_color.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "domain/color/pq_reference_white_mapper.hpp"
#include "domain/color/capture_edr_boundary.hpp"
#include "domain/color/display_p3_icc_profile.hpp"
#include "domain/png/png_encoder.hpp"
#include "adapters/shared/libultrahdr_encoder.hpp"
#include "ultra_hdr_metadata_test_support.hpp"
#include <ultrahdr/jpegdecoderhelper.h>
#include <zlib.h>
#include "test_support.hpp"
#include <iostream>
#include <bit>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>

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
  source.display_dynamic_range=DisplayDynamicRange::hdr;
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
void frozen_sdr_jpeg_clamps_output_and_statistics() {
  for (const float excursion : {1.0040311F, 1.5F}) {
    CanonicalFrameView source{};
    source.size_px={2,1};
    source.encoding=WindowsColor::linear_p3_encoding();
    source.display_dynamic_range=DisplayDynamicRange::sdr;
    source.rgba_float.assign({1.0F,1.0F,excursion,1.0F,
                              2.0F,2.0F,2.0F,1.0F});
    const auto before=source.rgba_float;
    auto roi=FrameCropper::view(source);
    AnnotationPixelPlan coverage{0,{2,1},{{0,0,1}},
        {{0,1,1,ObjectId{1},0xff0000,{{{0.4F,0,0,0.5F}}}}}};
    const auto jpeg=gpu->render({&roi,&coverage});
    HDRSHOT_CHECK(jpeg.has_value());
    HDRSHOT_CHECK(jpeg_output_kind(jpeg.value())==JpegOutputKind::display_p3_sdr);
    HDRSHOT_CHECK_NEAR(jpeg.value().maximum_linear_component,1.0,0);
    HDRSHOT_CHECK_NEAR(*jpeg.value().source_visible_maximum_linear_component,1.0,0);
    for(std::size_t p=0;p<2;++p) for(std::size_t c=0;c<3;++c)
      HDRSHOT_CHECK(ExtendedP3Mapper::decode_binary16(jpeg.value().rgba_half[p*4+c]).value()<=1.0F);
    HDRSHOT_CHECK(source.rgba_float==before);
    // The PNG path independently clips the same frozen SDR source.
    const auto png=gpu->process({&roi,&coverage,{},PqDiffuseWhite::nits_203,HdrPqPrecision::bits_10});
    HDRSHOT_CHECK(png && !png.value().content_light);
    HDRSHOT_CHECK(png.value().rgb_u16[2]==65535U);
    HDRSHOT_CHECK(source.rgba_float==before);
  }
  CanonicalFrameView rounded{};
  rounded.size_px={1,1};
  rounded.encoding=WindowsColor::linear_p3_encoding();
  rounded.display_dynamic_range=DisplayDynamicRange::sdr;
  rounded.rgba_float.assign({0.9999F,0.0F,-0.2F,1.0F});
  auto roi=FrameCropper::view(rounded);
  auto ownership=plan(rounded.size_px);
  const auto quantized=gpu->render({&roi,&ownership});
  HDRSHOT_CHECK(quantized.has_value());
  const auto stored=ExtendedP3Mapper::decode_binary16(quantized.value().rgba_half[0]).value();
  HDRSHOT_CHECK_NEAR(quantized.value().maximum_linear_component,stored,0);
  HDRSHOT_CHECK_NEAR(*quantized.value().source_visible_maximum_linear_component,stored,0);
  HDRSHOT_CHECK(stored>0.0F && stored<=1.0F);
  HDRSHOT_CHECK(quantized.value().rgba_half[2]==0U);
}
void captured_hdr_jpeg_threshold() {
  auto source=frame({1.0F});
  source.display_dynamic_range=DisplayDynamicRange::hdr;
  auto coverage=plan(source.size_px);
  auto roi=FrameCropper::view(source);
  const auto white=gpu->render({&roi,&coverage});
  HDRSHOT_CHECK(white && jpeg_output_kind(white.value())==JpegOutputKind::display_p3_sdr);
  source=frame({1.25F});
  source.display_dynamic_range=DisplayDynamicRange::hdr;
  roi=FrameCropper::view(source);
  const auto extended=gpu->render({&roi,&coverage});
  HDRSHOT_CHECK(extended && jpeg_output_kind(extended.value())==JpegOutputKind::ultra_hdr);
  HDRSHOT_CHECK_NEAR(*extended.value().source_visible_maximum_linear_component,1.25,0);
}
CanonicalFrameView fp32_capture(float maximum) {
  CanonicalFrameView source{};
  source.size_px={32,32};
  source.encoding=WindowsColor::linear_p3_encoding();
  source.display_dynamic_range=DisplayDynamicRange::hdr;
  source.capture_sdr_tolerance=true;
  source.rgba_float.resize(32U*32U*4U);
  for(std::size_t p=0;p<32U*32U;++p) {
    source.rgba_float[p*4]=maximum;
    source.rgba_float[p*4+1]=maximum;
    source.rgba_float[p*4+2]=maximum;
    source.rgba_float[p*4+3]=1.0F;
  }
  return source;
}
void save_evidence(const std::string& name, const std::vector<std::uint8_t>& bytes) {
  const char* root=std::getenv("HDRSHOT_CAPTURE_EVIDENCE_DIR");
  if(!root) return;
  std::filesystem::create_directories(root);
  std::ofstream file(std::filesystem::path(root)/name,std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
  HDRSHOT_CHECK(file.good());
}
// Decode actual PNG IDAT and filters rather than trusting the render array.
std::vector<std::uint16_t> decode_png_rgb16(const std::vector<std::uint8_t>& bytes,
    PixelSize size, bool expect_hdr) {
  auto be32=[&](std::size_t p) { return (std::uint32_t(bytes[p])<<24U)|
      (std::uint32_t(bytes[p+1])<<16U)|(std::uint32_t(bytes[p+2])<<8U)|bytes[p+3]; };
  std::vector<std::uint8_t> compressed;
  bool cicp=false, icc=false, clli=false;
  for(std::size_t p=8;p<bytes.size();) {
    const auto count=be32(p); HDRSHOT_CHECK(p+12U+count<=bytes.size());
    const std::string type(reinterpret_cast<const char*>(bytes.data()+p+4),4);
    if(type=="IDAT") compressed.insert(compressed.end(),bytes.begin()+p+8,bytes.begin()+p+8+count);
    if(type=="cICP") {
      HDRSHOT_CHECK(count==4 && bytes[p+8]==12 && bytes[p+9]==(expect_hdr?16:13) &&
          bytes[p+10]==0 && bytes[p+11]==1); cicp=true;
    }
    if(type=="iCCP") icc=true;
    if(type=="cLLI") clli=true;
    p+=12U+count;
  }
  HDRSHOT_CHECK(cicp && (expect_hdr || icc) && clli==expect_hdr);
  const auto row=static_cast<std::size_t>(size.width)*6U;
  std::vector<std::uint8_t> filtered((row+1U)*size.height), raw(row*size.height);
  uLongf count=static_cast<uLongf>(filtered.size());
  HDRSHOT_CHECK(uncompress(filtered.data(),&count,compressed.data(),static_cast<uLong>(compressed.size()))==Z_OK);
  HDRSHOT_CHECK(count==filtered.size());
  for(int y=0;y<size.height;++y) for(std::size_t x=0;x<row;++x) {
    const int a=x>=6?raw[std::size_t(y)*row+x-6]:0;
    const int b=y?raw[(std::size_t(y)-1)*row+x]:0;
    const int c=y && x>=6?raw[(std::size_t(y)-1)*row+x-6]:0;
    const int p=a+b-c, da=std::abs(p-a), db=std::abs(p-b), dc=std::abs(p-c);
    const int filter=filtered[std::size_t(y)*(row+1)];
    HDRSHOT_CHECK(filter>=0 && filter<=4);
    const int predictor=filter==0?0:filter==1?a:filter==2?b:filter==3?(a+b)/2:
        da<=db && da<=dc?a:db<=dc?b:c;
    raw[std::size_t(y)*row+x]=static_cast<std::uint8_t>(filtered[std::size_t(y)*(row+1)+x+1]+predictor);
  }
  std::vector<std::uint16_t> samples(raw.size()/2);
  for(std::size_t i=0;i<samples.size();++i) samples[i]=static_cast<std::uint16_t>((raw[2*i]<<8U)|raw[2*i+1]);
  return samples;
}
void capture_sdr_actual_files_clip_and_source_stays_immutable() {
  LibUltraHdrEncoder encoder;
  WindowsLinearP3RangeProbe range;
  for(const float maximum:{1.001F,kCaptureSdrMaximumEdr}) {
    auto source=fp32_capture(maximum), clipped=fp32_capture(1.0F);
    const auto before=source.rgba_float;
    auto roi=FrameCropper::view(source), reference=FrameCropper::view(clipped);
    const auto ownership=plan(source.size_px);
    const auto fit=range.probe(roi,ownership,{});
    HDRSHOT_CHECK(fit && fit.value().fits_sdr);
    const auto rendered=gpu->render({&roi,&ownership});
    const auto rendered_reference=gpu->render({&reference,&ownership});
    HDRSHOT_CHECK(rendered && rendered_reference && rendered.value().capture_sdr_tolerance);
    HDRSHOT_CHECK(ExtendedP3Mapper::decode_binary16(rendered.value().rgba_half[0]).value()>1.0F);
    HDRSHOT_CHECK_NEAR(*rendered.value().source_visible_maximum_linear_component,maximum,0);
    HDRSHOT_CHECK_NEAR(rendered.value().maximum_linear_component,maximum,0);
    for(const auto quality:{UltraHdrJpegQuality::maximum,UltraHdrJpegQuality::balanced,UltraHdrJpegQuality::compact}) {
      auto encoded=encoder.encode({&rendered.value(),quality});
      auto expected=encoder.encode({&rendered_reference.value(),quality});
      HDRSHOT_CHECK(encoded && expected && encoded.value().kind==JpegOutputKind::display_p3_sdr);
      HDRSHOT_CHECK(encoded.value().bytes==expected.value().bytes);
      HDRSHOT_CHECK(is_uhdr_image(encoded.value().bytes.data(),static_cast<int>(encoded.value().bytes.size()))==0);
      ultrahdr::JpegDecoderHelper decoder;
      HDRSHOT_CHECK(decoder.decompressImage(encoded.value().bytes.data(),encoded.value().bytes.size(),
          ultrahdr::DECODE_TO_RGB_CS).error_code==UHDR_CODEC_OK);
      HDRSHOT_CHECK(decoder.getXMPSize()==0 && decoder.getIsoMetadataSize()==0);
      uhdr_mem_block_t block{}; block.data=encoded.value().bytes.data(); block.data_sz=encoded.value().bytes.size();
      HDRSHOT_CHECK(std::ranges::equal(test::jpeg_icc(&block),DisplayP3IccProfile::bytes()));
      const auto image=decoder.getDecompressedImage();
      const auto* rgb=static_cast<const std::uint8_t*>(image.planes[UHDR_PLANE_PACKED]);
      const unsigned channels=image.fmt==UHDR_IMG_FMT_24bppRGB888?3U:4U;
      for(unsigned y=0;y<32;++y) for(unsigned x=0;x<32;++x) for(unsigned c=0;c<3;++c)
        HDRSHOT_CHECK(rgb[(y*image.stride[UHDR_PLANE_PACKED]+x)*channels+c]==255);
      save_evidence("sdr_"+std::to_string(std::bit_cast<std::uint32_t>(maximum))+"_q"+
          std::to_string(ultra_hdr_jpeg_quality_value(quality))+".jpg",encoded.value().bytes);
    }
    for(const auto white:{PqDiffuseWhite::nits_100,PqDiffuseWhite::nits_203}) {
      const auto pixels=gpu->process({&roi,&ownership,{},white,HdrPqPrecision::bits_16});
      const auto expected=gpu->process({&reference,&ownership,{},white,HdrPqPrecision::bits_16});
      HDRSHOT_CHECK(pixels && expected && !pixels.value().content_light);
      HDRSHOT_CHECK(pixels.value().rgb_u16==expected.value().rgb_u16);
      const auto metadata=PngColorMetadata{kDisplayP3SrgbFullRange,
          IccProfilePayload{"Display P3",DisplayP3IccProfile::bytes()}};
      const auto png=PngEncoder::encode_16bit({32,32,PngColorType::rgb,pixels.value().rgb_u16,metadata});
      const auto reference_png=PngEncoder::encode_16bit({32,32,PngColorType::rgb,expected.value().rgb_u16,metadata});
      HDRSHOT_CHECK(png && reference_png && png.value().bytes==reference_png.value().bytes);
      const auto decoded=decode_png_rgb16(png.value().bytes,source.size_px,false);
      for(const auto code:decoded) HDRSHOT_CHECK(code==65535U);
      save_evidence("sdr_"+std::to_string(std::bit_cast<std::uint32_t>(maximum))+"_w"+
          std::to_string(int(pq_diffuse_white_nits(white)))+".png",png.value().bytes);
    }
    HDRSHOT_CHECK(std::memcmp(before.data(),source.rgba_float.data(),before.size()*sizeof(float))==0);
    std::cout<<"Capture SDR clip: FP32="<<maximum<<" half>1; JPEG q100/95/85 equals preclipped bytes, decoded RGB8=255; PNG w100/203 equals preclipped bytes, decoded RGB16=65535; source bitwise unchanged\n";
  }
}
void capture_fp32_neighbors_and_hdr_near_white_preserved() {
  LibUltraHdrEncoder encoder;
  for(const float maximum:{kCaptureSdrMaximumEdr,std::bit_cast<float>(kCaptureSdrMaximumEdrBits+1U)}) {
    auto source=fp32_capture(1.001F);
    source.rgba_float[0]=maximum;
    source.rgba_float[1]=0.1F; source.rgba_float[2]=-0.2F;
    const auto before=source.rgba_float;
    auto roi=FrameCropper::view(source); const auto ownership=plan(source.size_px);
    const auto rendered=gpu->render({&roi,&ownership});
    HDRSHOT_CHECK(rendered.has_value());
    HDRSHOT_CHECK_NEAR(*rendered.value().source_visible_maximum_linear_component,maximum,0);
    const auto expected=source_requires_hdr(maximum,true)?JpegOutputKind::ultra_hdr:JpegOutputKind::display_p3_sdr;
    HDRSHOT_CHECK(jpeg_output_kind(rendered.value())==expected);
    HDRSHOT_CHECK(rendered.value().rgba_half[4]==ExtendedP3Mapper::encode_binary16(1.001F));
    auto encoded=encoder.encode({&rendered.value(),UltraHdrJpegQuality::maximum});
    HDRSHOT_CHECK(encoded && encoded.value().kind==expected);
    HDRSHOT_CHECK(std::memcmp(before.data(),source.rgba_float.data(),before.size()*sizeof(float))==0);
    std::cout<<"Capture neighbor bits="<<std::bit_cast<std::uint32_t>(maximum)<<" half="<<rendered.value().rgba_half[0]<<" kind="<<int(expected)<<'\n';
  }
  auto source=fp32_capture(1.001F); source.rgba_float[0]=4.0F;
  const auto before=source.rgba_float;
  auto roi=FrameCropper::view(source); const auto ownership=plan(source.size_px);
  const auto rendered=gpu->render({&roi,&ownership});
  HDRSHOT_CHECK(rendered && jpeg_output_kind(rendered.value())==JpegOutputKind::ultra_hdr);
  HDRSHOT_CHECK_NEAR(rendered.value().maximum_linear_component,4.0,0);
  for(std::size_t p=1;p<32U*32U;++p) for(std::size_t c=0;c<3;++c)
    HDRSHOT_CHECK(rendered.value().rgba_half[p*4+c]==ExtendedP3Mapper::encode_binary16(1.001F));
  auto encoded=encoder.encode({&rendered.value(),UltraHdrJpegQuality::maximum});
  HDRSHOT_CHECK(encoded && encoded.value().kind==JpegOutputKind::ultra_hdr);
  std::unique_ptr<uhdr_codec_private_t,decltype(&uhdr_release_decoder)> decoder{uhdr_create_decoder(),&uhdr_release_decoder};
  uhdr_compressed_image_t input{}; input.data=encoded.value().bytes.data(); input.data_sz=input.capacity=encoded.value().bytes.size();
  HDRSHOT_CHECK(uhdr_dec_set_image(decoder.get(),&input).error_code==UHDR_CODEC_OK);
  HDRSHOT_CHECK(uhdr_dec_probe(decoder.get()).error_code==UHDR_CODEC_OK);
  test::check_dual_metadata(decoder.get()); test::check_p3_base_and_alternate(decoder.get());
  HDRSHOT_CHECK_NEAR(uhdr_dec_get_gainmap_metadata(decoder.get())->hdr_capacity_max,4.0,1e-5);
  save_evidence("hdr_single_pixel_and_near_white.jpg",encoded.value().bytes);
  for(const auto white:{PqDiffuseWhite::nits_100,PqDiffuseWhite::nits_203}) {
    auto clipped=fp32_capture(1.0F); clipped.rgba_float[0]=4.0F;
    auto clipped_roi=FrameCropper::view(clipped);
    const auto clipped_pixels=gpu->process({&clipped_roi,&ownership,
        {OutputClass::hdr,EncodingIntent::hdr_pq,16,"test"},white,HdrPqPrecision::bits_16});
    HDRSHOT_CHECK(clipped_pixels.has_value());
    const auto png_pixels=gpu->process({&roi,&ownership,
        {OutputClass::hdr,EncodingIntent::hdr_pq,16,"test"},white,HdrPqPrecision::bits_16});
    HDRSHOT_CHECK(png_pixels && png_pixels.value().content_light.has_value());
    const auto levels=finalize_content_light(*png_pixels.value().content_light);
    HDRSHOT_CHECK(levels.has_value());
    const auto png=PngEncoder::encode_16bit({32,32,PngColorType::rgb,png_pixels.value().rgb_u16,
        PngColorMetadata{kDisplayP3PqFullRange,std::nullopt,levels}});
    HDRSHOT_CHECK(png.has_value());
    const auto decoded=decode_png_rgb16(png.value().bytes,source.size_px,true);
    const auto near_white=PqReferenceWhiteMapper::map_linear_edr(1.001F,pq_diffuse_white_nits(white),HdrPqPrecision::bits_16).value().png_u16;
    const auto clipped_white=PqReferenceWhiteMapper::map_linear_edr(1.0F,pq_diffuse_white_nits(white),HdrPqPrecision::bits_16).value().png_u16;
    HDRSHOT_CHECK(near_white>clipped_white);
    // GPU PQ's established approximation tolerance is eight UNORM16 codes.
    for(std::size_t i=3;i<decoded.size();++i) HDRSHOT_CHECK_NEAR(decoded[i],near_white,8);
    HDRSHOT_CHECK(decoded[3]>clipped_pixels.value().rgb_u16[3]);
    HDRSHOT_CHECK(decoded[0]>decoded[3]);
    save_evidence("hdr_single_pixel_and_near_white_w"+std::to_string(int(pq_diffuse_white_nits(white)))+".png",png.value().bytes);
  }
  HDRSHOT_CHECK(std::memcmp(before.data(),source.rgba_float.data(),before.size()*sizeof(float))==0);
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
      {"UHDR AA coverage and nonfinite rejection",uhdr_aa_ownership_and_invalid_source},
      {"frozen SDR JPEG range and PNG consistency",frozen_sdr_jpeg_clamps_output_and_statistics},
      {"captured HDR JPEG threshold",captured_hdr_jpeg_threshold},
      {"capture SDR actual PNG JPEG clipping",capture_sdr_actual_files_clip_and_source_stays_immutable},
      {"capture FP32 neighbors and HDR near-white",capture_fp32_neighbors_and_hdr_near_white_preserved}});
}
