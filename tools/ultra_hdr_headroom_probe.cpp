// Offline diagnostic: production CPU input renderer -> shared encoder ->
// serialized ISO metadata -> decoder. No screen capture or product changes.
#include "adapters/shared/libultrahdr_encoder.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "domain/output/ultra_hdr_input_renderer.hpp"
#include <ultrahdr_api.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>

using namespace hdrshot;
static void check(bool ok) { if (!ok) throw std::runtime_error("headroom probe failed"); }
int main() try {
  std::cout << std::setprecision(10);
  for (float peak : {1.F, 2.F, 4.F, 8.F, 16.F, 10000.F / 203.F}) {
    constexpr int w = 64, h = 32;
    std::vector<std::uint16_t> rgba(w * h * 4, 0x3C00U);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
      for (int c = 0; c < 3; ++c) rgba[(y * w + x) * 4 + c] =
          ExtendedP3Mapper::encode_binary16(ExtendedP3Mapper::encode_extended_srgb(
              x >= w / 2 ? peak : .25F));
    AnnotationPixelPlan plan;
    plan.output_size_px = {w, h};
    for (int y = 0; y < h; ++y) plan.source_visible_spans.push_back({y, 0, w});
    SelectionRoiView view{FrameId{1}, DisplayGeneration{1}, SelectionRevision{1},
        PixelRect{0,0,w,h}, PixelSize{w,h}, 1.,
        ColorEncoding{ColorPrimaries::display_p3, TransferFunction::extended_srgb,
                      AlphaMode::straight, 0.},
        DisplayDynamicRange::hdr, rgba, 0, w * 4};
    CpuUltraHdrInputRenderer renderer;
    auto linear = renderer.render({&view, &plan, 203.});
    check(linear.has_value());
    LibUltraHdrEncoder encoder;
    auto result = encoder.encode({&linear.value(), UltraHdrJpegQuality::balanced});
    check(result.has_value());
    std::cout << "fixture_peak=" << peak << " renderer_peak=" << linear.value().maximum_linear_component;
    if (result.value().kind == JpegOutputKind::display_p3_sdr) {
      std::cout << " kind=ordinary_sdr no_gainmap\n";
      continue;
    }
    std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)>
        decoder(uhdr_create_decoder(), uhdr_release_decoder);
    check(decoder != nullptr);
    uhdr_compressed_image_t jpeg{};
    jpeg.data = result.value().bytes.data(); jpeg.data_sz = jpeg.capacity = result.value().bytes.size();
    check(uhdr_dec_set_image(decoder.get(), &jpeg).error_code == UHDR_CODEC_OK);
    check(uhdr_dec_set_out_img_format(decoder.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat).error_code == UHDR_CODEC_OK);
    check(uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR).error_code == UHDR_CODEC_OK);
    check(uhdr_dec_set_out_max_display_boost(decoder.get(), 100.F).error_code == UHDR_CODEC_OK);
    check(uhdr_dec_probe(decoder.get()).error_code == UHDR_CODEC_OK);
    const auto* m = uhdr_dec_get_gainmap_metadata(decoder.get());
    check(m != nullptr);
    const double expected = std::clamp(linear.value().maximum_linear_component, 1., 10000. / 203.);
    check(std::abs(m->hdr_capacity_max - expected) < .0001);
    std::cout << " capacity=" << m->hdr_capacity_max
              << " alt_headroom_ev=" << std::log2(m->hdr_capacity_max)
              << " gainmap_max_ev=" << std::log2(m->max_content_boost[0])
              << " automatic_W_for_display_5.8="
              << std::clamp(std::log2(5.8) / std::log2(m->hdr_capacity_max), 0., 1.);
    check(uhdr_decode(decoder.get()).error_code == UHDR_CODEC_OK);
    std::cout << " full_hdr_decode=ok\n";
  }
  return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
