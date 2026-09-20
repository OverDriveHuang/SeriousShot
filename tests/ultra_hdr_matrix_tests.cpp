#include "test_support.hpp"
#include <ultrahdr/gainmapmath.h>
#include <ultrahdr/jpegencoderhelper.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>

namespace {
void jpeg_matrix_matches_independent_oracle() {
  using namespace ultrahdr;
  // Include SIMD tails and primary/secondary/neutral colors, not only gray.
  for (auto gamut : {UHDR_CG_DISPLAY_P3, UHDR_CG_BT_709, UHDR_CG_BT_2100})
  for (unsigned width : {31U,32U,65U}) {
    const unsigned height=(4096U+width-1U)/width;
    uhdr_raw_image_ext_t source(UHDR_IMG_FMT_32bppRGBA8888,gamut,
        UHDR_CT_SRGB,UHDR_CR_FULL_RANGE,width,height,64);
    auto* rgba=static_cast<unsigned char*>(source.planes[UHDR_PLANE_PACKED]);
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
      const unsigned n=(y*width+x)%4096U;
      auto* p=rgba+(y*source.stride[UHDR_PLANE_PACKED]+x)*4;
      p[0]=static_cast<unsigned char>((n%16)*17);
      p[1]=static_cast<unsigned char>(((n/16)%16)*17);
      p[2]=static_cast<unsigned char>(((n/256)%16)*17); p[3]=255;
    }
    auto scalar=convert_raw_input_to_ycbcr(&source,false);
    HDRSHOT_CHECK(scalar && scalar->fmt==UHDR_IMG_FMT_24bppYCbCr444);
#if defined(UHDR_ENABLE_INTRINSICS) && (defined(__ARM_NEON__) || defined(__ARM_NEON))
    auto optimized=convert_raw_input_to_ycbcr_neon(&source);
    HDRSHOT_CHECK(optimized && optimized->fmt==UHDR_IMG_FMT_24bppYCbCr444);
    std::array<uhdr_raw_image_t*,2> variants{scalar.get(),optimized.get()};
#else
    std::array<uhdr_raw_image_t*,1> variants{scalar.get()};
#endif
    int max_error=0;
    for (auto* dst : variants) for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
      auto* p=rgba+(y*source.stride[UHDR_PLANE_PACKED]+x)*4;
      const double kr=gamut==UHDR_CG_DISPLAY_P3 ? .299 : gamut==UHDR_CG_BT_709 ? .2126 : .2627;
      const double kb=gamut==UHDR_CG_DISPLAY_P3 ? .114 : gamut==UHDR_CG_BT_709 ? .0722 : .0593;
      const double yy=kr*p[0]+(1-kr-kb)*p[1]+kb*p[2];
      const std::array<double,3> expected{yy,(p[2]-yy)/(2-2*kb)+128,(p[0]-yy)/(2-2*kr)+128};
      for (unsigned c=0;c<3;++c) {
        const auto* plane=static_cast<unsigned char*>(dst->planes[c]);
        const int actual=plane[y*dst->stride[c]+x];
        const int oracle=static_cast<int>(std::clamp(std::floor(expected[c]+.5),0.,255.));
        max_error=std::max(max_error,std::abs(actual-oracle));
      }
    }
    std::cout<<"JPEG gamut="<<gamut<<" width="<<width<<" variants="<<variants.size()<<" max_code_error="<<max_error<<'\n';
    HDRSHOT_CHECK(max_error<=1);
  }
}
void ordinary_jpeg_base_preserves_p3_codes() {
  using namespace ultrahdr;
  constexpr std::array<std::array<unsigned char,3>,8> colors{{
      {255,0,0},{0,255,0},{0,0,255},{255,255,0},
      {255,0,255},{0,255,255},{128,128,128},{255,255,255}}};
  uhdr_raw_image_ext_t source(UHDR_IMG_FMT_32bppRGBA8888,UHDR_CG_DISPLAY_P3,
      UHDR_CT_SRGB,UHDR_CR_FULL_RANGE,256,32,64);
  auto* rgba=static_cast<unsigned char*>(source.planes[UHDR_PLANE_PACKED]);
  for (unsigned y=0;y<32;++y) for (unsigned x=0;x<256;++x) {
    auto* p=rgba+(y*source.stride[UHDR_PLANE_PACKED]+x)*4;
    for (unsigned c=0;c<3;++c) p[c]=colors[x/32][c]; p[3]=255;
  }
#if defined(UHDR_ENABLE_INTRINSICS) && (defined(__ARM_NEON__) || defined(__ARM_NEON))
  auto yuv=convert_raw_input_to_ycbcr_neon(&source);
#else
  auto yuv=convert_raw_input_to_ycbcr(&source,false);
#endif
  HDRSHOT_CHECK(yuv != nullptr);
  for (int q : {100,95,85}) {
    JpegEncoderHelper encoder;
    HDRSHOT_CHECK(encoder.compressImage(yuv.get(),q,nullptr,0).error_code==UHDR_CODEC_OK);
    const auto encoded=encoder.getCompressedImage();
    JpegDecoderHelper decoder;
    HDRSHOT_CHECK(decoder.decompressImage(encoded.data,encoded.data_sz,DECODE_TO_RGB_CS).error_code==UHDR_CODEC_OK);
    const auto decoded=decoder.getDecompressedImage();
    HDRSHOT_CHECK(decoded.fmt==UHDR_IMG_FMT_24bppRGB888 || decoded.fmt==UHDR_IMG_FMT_32bppRGBA8888);
    const unsigned channels=decoded.fmt==UHDR_IMG_FMT_24bppRGB888 ? 3U : 4U;
    const auto* rgb=static_cast<unsigned char*>(decoded.planes[UHDR_PLANE_PACKED]);
    int max_error=0;
    for (unsigned y=8;y<24;++y) for (unsigned x=0;x<256;++x) {
      if (x%32<8 || x%32>=24) continue;
      for (unsigned c=0;c<3;++c) {
        const int actual=rgb[(y*decoded.stride[UHDR_PLANE_PACKED]+x)*channels+c];
        max_error=std::max(max_error,std::abs(actual-colors[x/32][c]));
      }
    }
    std::cout<<"ordinary JPEG P3 base q="<<q<<" max_code_error="<<max_error<<'\n';
    HDRSHOT_CHECK(max_error<=3);
  }
}
}
int main() {
  return hdrshot::test::run({
      {"JPEG matrix scalar/SIMD matches independent oracle",jpeg_matrix_matches_independent_oracle},
      {"Ordinary JPEG P3 base preserves color codes",ordinary_jpeg_base_preserves_p3_codes}});
}
