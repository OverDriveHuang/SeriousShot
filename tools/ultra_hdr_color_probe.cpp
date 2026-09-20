// Offline numerical probe. Input is little-endian float32 linear P3 RGB,
// with 1 = 203 nit. No capture, window, or platform color-management dependency.
#include "adapters/shared/libultrahdr_encoder.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "domain/output/ultra_hdr_input_renderer.hpp"
#include <ultrahdr_api.h>
#include <ultrahdr/jpegdecoderhelper.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace hdrshot;
namespace fs = std::filesystem;
static void check(bool ok) { if (!ok) throw std::runtime_error("probe operation failed"); }
template<class T> static void write(const fs::path& path, const std::vector<T>& values) {
  check(!fs::exists(path));
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(values.data()),
            static_cast<std::streamsize>(values.size() * sizeof(T)));
  check(out.good());
}
int main(int argc, char** argv) try {
  if (argc != 5) { std::cerr << "usage: probe width height linear_p3_f32_rgb output_dir\n"; return 2; }
  const int w = std::stoi(argv[1]), h = std::stoi(argv[2]);
  check(w > 0 && h > 0 && w <= 16384 && h <= 16384);
  const auto pixels = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  check(fs::file_size(argv[3]) == pixels * 3 * sizeof(float));
  std::vector<float> reference(pixels * 3);
  std::ifstream in(argv[3], std::ios::binary);
  in.read(reinterpret_cast<char*>(reference.data()), static_cast<std::streamsize>(reference.size() * sizeof(float)));
  check(in.good());
  const fs::path dir = argv[4];
  check(!fs::exists(dir));
  fs::create_directories(dir);

  // Simulate canonical FP16 Extended P3, then use the production CPU reference
  // renderer. Compare its conversion loss separately from codec round-trip loss.
  std::vector<std::uint16_t> extended(pixels * 4);
  for (std::size_t p = 0; p < pixels; ++p) {
    for (std::size_t c = 0; c < 3; ++c)
      extended[p*4+c] = ExtendedP3Mapper::encode_binary16(
          ExtendedP3Mapper::encode_extended_srgb(reference[p*3+c]));
    extended[p*4+3] = ExtendedP3Mapper::encode_binary16(1.0F);
  }
  AnnotationPixelPlan plan;
  plan.output_size_px = {w,h};
  for (int y = 0; y < h; ++y) plan.source_visible_spans.push_back({y,0,w});
  SelectionRoiView view{FrameId{1},DisplayGeneration{1},SelectionRevision{1},
      PixelRect{0,0,w,h},PixelSize{w,h},1.0,
      ColorEncoding{ColorPrimaries::display_p3,TransferFunction::extended_srgb,AlphaMode::straight,0.0},
      DisplayDynamicRange::hdr,extended,0,static_cast<std::size_t>(w)*4};
  CpuUltraHdrInputRenderer renderer;
  auto rendered = renderer.render({&view,&plan,kUltraHdrReferenceWhiteNits});
  check(rendered.has_value());
  std::vector<float> input(pixels*3);
  for (std::size_t p=0; p<pixels; ++p)
    for (std::size_t c=0; c<3; ++c)
      input[p*3+c] = ExtendedP3Mapper::decode_binary16(rendered.value().rgba_half[p*4+c]).value();
  write(dir/"codec_input.f32", input);
  LibUltraHdrEncoder encoder;
  for (auto quality : {UltraHdrJpegQuality::maximum, UltraHdrJpegQuality::balanced, UltraHdrJpegQuality::compact}) {
    const auto q = std::to_string(ultra_hdr_jpeg_quality_value(quality));
    const auto start = std::chrono::steady_clock::now();
    auto encoded = encoder.encode({&rendered.value(),quality});
    check(encoded.has_value());
    const auto end = std::chrono::steady_clock::now();
    write(dir/("q"+q+".jpg"), encoded.value().bytes);
    if (encoded.value().kind == JpegOutputKind::display_p3_sdr) {
      ultrahdr::JpegDecoderHelper decoder;
      check(decoder.decompressImage(encoded.value().bytes.data(),
          encoded.value().bytes.size(), ultrahdr::DECODE_TO_RGB_CS).error_code == UHDR_CODEC_OK);
      const auto output = decoder.getDecompressedImage();
      check(output.w == static_cast<unsigned>(w) && output.h == static_cast<unsigned>(h));
      const unsigned channels = output.fmt == UHDR_IMG_FMT_24bppRGB888 ? 3U : 4U;
      const auto* rgb = static_cast<const std::uint8_t*>(output.planes[UHDR_PLANE_PACKED]);
      std::vector<float> decoded(pixels * 3U);
      for (unsigned y = 0; y < output.h; ++y) for (unsigned x = 0; x < output.w; ++x)
        for (unsigned c = 0; c < 3; ++c) {
          const float code = rgb[(y * output.stride[UHDR_PLANE_PACKED] + x) * channels + c] / 255.F;
          decoded[(static_cast<std::size_t>(y) * output.w + x) * 3U + c] =
              ExtendedP3Mapper::inverse_extended_srgb(code);
        }
      write(dir/("q"+q+".f32"), decoded);
      std::cout << "kind=ordinary_sdr quality=" << q << " bytes=" << encoded.value().bytes.size()
                << " encode_ms=" << std::chrono::duration<double,std::milli>(end-start).count() << '\n';
      continue;
    }
    std::unique_ptr<uhdr_codec_private_t,decltype(&uhdr_release_decoder)> decoder(uhdr_create_decoder(),uhdr_release_decoder);
    check(decoder != nullptr);
    uhdr_compressed_image_t image{};
    image.data = encoded.value().bytes.data();
    image.data_sz = image.capacity = encoded.value().bytes.size();
    image.cg=UHDR_CG_UNSPECIFIED; image.ct=UHDR_CT_UNSPECIFIED; image.range=UHDR_CR_UNSPECIFIED;
    check(uhdr_dec_set_image(decoder.get(),&image).error_code==UHDR_CODEC_OK);
    check(uhdr_dec_set_out_img_format(decoder.get(),UHDR_IMG_FMT_64bppRGBAHalfFloat).error_code==UHDR_CODEC_OK);
    check(uhdr_dec_set_out_color_transfer(decoder.get(),UHDR_CT_LINEAR).error_code==UHDR_CODEC_OK);
    // Full reconstruction, NOT a simulated display with restricted headroom.
    check(uhdr_dec_set_out_max_display_boost(decoder.get(),100.0F).error_code==UHDR_CODEC_OK);
    check(uhdr_decode(decoder.get()).error_code==UHDR_CODEC_OK);
    const auto* output=uhdr_get_decoded_image(decoder.get());
    check(output && output->cg==UHDR_CG_DISPLAY_P3 && output->ct==UHDR_CT_LINEAR);
    check(output->w==static_cast<unsigned>(w) && output->h==static_cast<unsigned>(h));
    auto* half=static_cast<const std::uint16_t*>(output->planes[UHDR_PLANE_PACKED]);
    std::vector<float> decoded(pixels*3);
    for (int y=0; y<h; ++y) for (int x=0; x<w; ++x) for (std::size_t c=0;c<3;++c) {
      const auto s=(static_cast<std::size_t>(y)*output->stride[UHDR_PLANE_PACKED]+static_cast<std::size_t>(x))*4+c;
      const auto d=(static_cast<std::size_t>(y)*static_cast<std::size_t>(w)+static_cast<std::size_t>(x))*3+c;
      decoded[d]=ExtendedP3Mapper::decode_binary16(half[s]).value();
    }
    write(dir/("q"+q+".f32"),decoded);
    std::cout << "quality="<<q<<" bytes="<<encoded.value().bytes.size()
              <<" encode_ms="<<std::chrono::duration<double,std::milli>(end-start).count()<<'\n';
  }
  return 0;
} catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
