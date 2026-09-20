#include "adapters/shared/libultrahdr_encoder.hpp"
#include "domain/annotation/annotation_render_plan.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "domain/png/streaming_png_encoder.hpp"
#include "platform/macos/macos_metal_export_pixel_processor.hpp"

#import <Foundation/Foundation.h>

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>
#include <zlib.h>

// Offline stage benchmark, not a timing assertion or a production logger.
// No capture, windows, clipboard, or filesystem output. Compare the checksums
// and maxima as well as time before accepting any executor optimization.
namespace {
using namespace hdrshot;

template<class Action> auto timed(Action&& action) {
  const auto start = std::chrono::steady_clock::now();
  auto result = action();
  const auto ms = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start).count();
  if (!result) {
    std::cerr << to_string(result.error().code) << '\n';
    for (const auto& [key, value] : result.error().safe_context)
      std::cerr << key << '=' << value << '\n';
    throw std::runtime_error("benchmark stage failed");
  }
  return std::pair{std::move(result.value()), ms};
}

template<class T> unsigned long checksum(const std::vector<T>& data) {
  return crc32(0, reinterpret_cast<const Bytef*>(data.data()),
               static_cast<uInt>(data.size() * sizeof(T)));
}

void run(const int width, const int height) {
  auto [metal, create_ms] = timed([] { return MacMetalExportPixelProcessor::create(); });
  std::cout << "create_ms=" << create_ms << '\n';
  LibUltraHdrEncoder codec;
  for (const bool hdr : {false, true}) {
    CanonicalFrameView source{FrameId{1}, 1, 1, {0, 0, width, height},
        {width, height}, 1.0,
        {ColorPrimaries::display_p3, TransferFunction::extended_srgb,
         AlphaMode::straight, 0.0}, DisplayDynamicRange::hdr, {}};
    const auto pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    source.rgba_half.resize(pixels * 4);
    std::uint32_t seed = 0xC001D00D;
    for (std::size_t i = 0; i < pixels; ++i) {
      seed = seed * 1664525U + 1013904223U;
      const float noise = float((seed >> 16) & 255U) / 255.0F;
      const float ramp = float(i % static_cast<std::size_t>(width)) / float(width);
      for (std::size_t c = 0; c < 3; ++c) {
        const float value = (0.1F + ramp * 0.75F + noise * 0.05F) *
            (1.0F - float(c) * 0.12F) * (hdr ? 2.0F : 1.0F);
        source.rgba_half[i * 4 + c] = ExtendedP3Mapper::encode_binary16(value);
      }
      source.rgba_half[i * 4 + 3] = 0x3C00;
    }
    const auto view = FrameCropper::view(source);
    auto plan = AnnotationRenderPlanner::build_pixel_plan({0, {width, height}, {}});
    if (!plan) throw std::runtime_error("fixture plan failed");
    for (int iteration = 0; iteration < 3; ++iteration) {
      @autoreleasepool {
        auto [linear, render_ms] = timed([&] { return metal->render({&view, &plan.value(), 203.0}); });
        auto [jpeg, jpeg_ms] = timed([&] { return codec.encode({&linear, UltraHdrJpegQuality::balanced}); });
        const auto linear_crc = checksum(linear.rgba_half);
        const auto actual_max = linear.maximum_linear_component;
        const auto visible_max = linear.source_visible_maximum_linear_component.value();
        // These vectors retain their capacity until this iteration ends; their
        // contents are no longer needed when exercising the PNG branch.
        const auto jpeg_bytes = jpeg.bytes.size();
        const auto jpeg_crc = checksum(jpeg.bytes);
        linear.rgba_half = {};
        jpeg.bytes = {};
        const OutputPlan output{hdr ? OutputClass::hdr : OutputClass::wide_gamut_sdr,
            hdr ? EncodingIntent::hdr_pq : EncodingIntent::wide_gamut_sdr, 16, "benchmark"};
        auto [rgb, process_ms] = timed([&] { return metal->process({&view, &plan.value(), output,
            PqDiffuseWhite::nits_203, HdrPqPrecision::bits_10}); });
        auto [png, png_ms] = timed([&] {
          return StreamingPngEncoder::encode_16bit_rgb({static_cast<std::uint32_t>(width),
              static_cast<std::uint32_t>(height),
              PngColorMetadata{hdr ? kDisplayP3PqFullRange : kDisplayP3SrgbFullRange, std::nullopt},
              [&](const std::uint32_t y) -> Result<std::vector<std::uint16_t>, Error> {
                const auto begin = rgb.rgb_u16.begin() + static_cast<std::ptrdiff_t>(
                    static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 3);
                return Result<std::vector<std::uint16_t>, Error>::success({begin, begin + width * 3});
              }, {}, static_cast<std::uint8_t>(hdr ? 10U : 16U)});
        });
        std::cout << width << ',' << height << ',' << (hdr ? "HDR" : "SDR") << ',' << iteration
            << ',' << render_ms << ',' << jpeg_ms << ',' << process_ms << ',' << png_ms
            << ',' << linear_crc << ',' << actual_max << ',' << visible_max
            << ',' << jpeg_bytes << ',' << jpeg_crc << ',' << png.bytes.size()
            << ',' << checksum(rgb.rgb_u16) << '\n' << std::flush;
      }
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--5k")) {
    std::cerr << "usage: hdrshot_macos_export_benchmark [--5k]\n";
    return 2;
  }
  std::cout << std::setprecision(10)
      << "width,height,range,iteration,jpeg_input_ms,jpeg_codec_ms,png_pixels_ms,png_codec_ms,"
         "linear_crc,actual_max,visible_max,jpeg_bytes,jpeg_crc,png_bytes,rgb16_crc\n";
  try {
    @autoreleasepool { run(argc == 2 ? 5120 : 2048, argc == 2 ? 2880 : 2048); }
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
