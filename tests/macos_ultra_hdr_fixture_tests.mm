#include "adapters/shared/libultrahdr_encoder.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "domain/output/ultra_hdr_input_renderer.hpp"
#include "test_support.hpp"
#include "ultra_hdr_metadata_test_support.hpp"

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <ultrahdr_api.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace hdrshot;
using DecoderHandle =
    std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)>;

struct SourceFixture {
  PixelSize size_px{};
  std::vector<std::uint16_t> rgba_half;
};

void inspect_apple_decode(NSData* bytes, const char* label) {
  CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)bytes, nullptr);
  HDRSHOT_CHECK(source != nullptr);
  for (CFStringRef mode : {kCGImageSourceDecodeToSDR, kCGImageSourceDecodeToHDR}) {
    NSDictionary* options = @{(__bridge NSString*)kCGImageSourceDecodeRequest:
        (__bridge NSString*)mode};
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, (__bridge CFDictionaryRef)options);
    HDRSHOT_CHECK(image != nullptr);
    CGColorSpaceRef space = CGImageGetColorSpace(image);
    HDRSHOT_CHECK(space != nullptr && CGColorSpaceGetModel(space) == kCGColorSpaceModelRGB);
    CFStringRef name = CGColorSpaceCopyName(space);
    if (mode == kCGImageSourceDecodeToSDR) {
      HDRSHOT_CHECK(name != nullptr && CFEqual(name, kCGColorSpaceDisplayP3));
    } else {
      // ImageIO may return an unnamed adaptive ICC instead of a named color
      // space. Verify its actual P3/PQ declaration, not an optional label.
      CFDataRef profile = CGColorSpaceCopyICCData(space);
      HDRSHOT_CHECK(profile != nullptr);
      const std::span<const std::uint8_t> icc{
          CFDataGetBytePtr(profile), static_cast<std::size_t>(CFDataGetLength(profile))};
      const auto cicp = hdrshot::test::icc_tag(icc, "cicp");
      const bool valid = cicp.size() == 12U && cicp[8] == 12 && cicp[9] == 16 &&
          cicp[10] == 0 && cicp[11] == 1;
      CFRelease(profile);
      HDRSHOT_CHECK(valid);
    }
    NSString* text = (__bridge NSString*)name;
    std::cout << "Apple " << label << " "
              << (mode == kCGImageSourceDecodeToHDR ? "HDR" : "SDR")
              << " colorspace=" << (text != nil ? text.UTF8String : "(unnamed)")
              << " bits=" << CGImageGetBitsPerComponent(image) << '\n';
    if (name) CFRelease(name);
    CGImageRelease(image);
  }
  CFRelease(source);
}

bool contains_ascii(
    const std::span<const std::uint8_t> bytes,
    const std::string& needle) {
  return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) !=
      bytes.end();
}

SourceFixture load_pq_png_as_extended_display_p3(const char* path) {
  NSString* native_path = [NSString stringWithUTF8String:path];
  HDRSHOT_CHECK(native_path != nil);
  NSData* source_bytes = [NSData dataWithContentsOfFile:native_path];
  HDRSHOT_CHECK(source_bytes != nil);
  HDRSHOT_CHECK(contains_ascii(
      {static_cast<const std::uint8_t*>(source_bytes.bytes), source_bytes.length},
      "kCGColorSpaceITUR_2100_PQ"));

  NSURL* url = [NSURL fileURLWithPath:native_path];
  CGImageSourceRef source = CGImageSourceCreateWithURL(
      (__bridge CFURLRef)url, nullptr);
  HDRSHOT_CHECK(source != nullptr);
  CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0U, nullptr);
  CFRelease(source);
  HDRSHOT_CHECK(image != nullptr);
  const auto width = CGImageGetWidth(image);
  const auto height = CGImageGetHeight(image);
  HDRSHOT_CHECK(width == 2048U && height == 2048U);
  HDRSHOT_CHECK(CGImageGetBitsPerComponent(image) == 16U);
  HDRSHOT_CHECK(width <= static_cast<std::size_t>(kUltraHdrMaximumDimension));
  HDRSHOT_CHECK(height <= static_cast<std::size_t>(kUltraHdrMaximumDimension));

  const auto sample_count = width * height * 4U;
  std::vector<float> extended_p3(sample_count);
  CGColorSpaceRef color_space =
      CGColorSpaceCreateWithName(kCGColorSpaceExtendedDisplayP3);
  HDRSHOT_CHECK(color_space != nullptr);
  const CGBitmapInfo bitmap_info =
      static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) |
      static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little) |
      static_cast<CGBitmapInfo>(kCGBitmapFloatComponents);
  CGContextRef context = CGBitmapContextCreate(
      extended_p3.data(), width, height, 32U, width * sizeof(float) * 4U,
      color_space, bitmap_info);
  CGColorSpaceRelease(color_space);
  HDRSHOT_CHECK(context != nullptr);
  CGContextSetBlendMode(context, kCGBlendModeCopy);
  CGContextSetInterpolationQuality(context, kCGInterpolationNone);
  CGContextDrawImage(
      context, CGRectMake(0.0, 0.0, static_cast<double>(width),
                          static_cast<double>(height)),
      image);
  CGContextRelease(context);
  CGImageRelease(image);

  SourceFixture fixture{
      PixelSize{static_cast<std::int32_t>(width),
                static_cast<std::int32_t>(height)},
      std::vector<std::uint16_t>(sample_count)};
  float maximum = 0.0F;
  for (std::size_t pixel = 0; pixel < width * height; ++pixel) {
    for (std::size_t channel = 0; channel < 3U; ++channel) {
      const auto value = extended_p3[pixel * 4U + channel];
      HDRSHOT_CHECK(std::isfinite(value));
      maximum = std::max(maximum, value);
      fixture.rgba_half[pixel * 4U + channel] =
          ExtendedP3Mapper::encode_binary16(value);
    }
    fixture.rgba_half[pixel * 4U + 3U] =
        ExtendedP3Mapper::encode_binary16(1.0F);
  }
  HDRSHOT_CHECK(maximum > 1.0F);
  std::cout << "fixture extended-P3 maximum=" << maximum << '\n';
  return fixture;
}

LinearDisplayP3HalfImage render_linear(SourceFixture& fixture) {
  AnnotationPixelPlan plan;
  plan.output_size_px = fixture.size_px;
  for (std::int32_t y = 0; y < fixture.size_px.height; ++y) {
    plan.source_visible_spans.push_back(
        SourceVisibleSpan{y, 0, fixture.size_px.width});
  }
  SelectionRoiView view{
      FrameId{1}, DisplayGeneration{1}, SelectionRevision{1},
      PixelRect{0, 0, fixture.size_px.width, fixture.size_px.height},
      fixture.size_px, 1.0,
      ColorEncoding{ColorPrimaries::display_p3,
                    TransferFunction::extended_srgb, AlphaMode::straight, 0.0},
      DisplayDynamicRange::hdr, fixture.rgba_half, 0U,
      static_cast<std::size_t>(fixture.size_px.width) * 4U};
  CpuUltraHdrInputRenderer renderer;
  auto rendered = renderer.render(
      UltraHdrInputRenderRequest{&view, &plan, kUltraHdrReferenceWhiteNits});
  HDRSHOT_CHECK(rendered.has_value());
  HDRSHOT_CHECK(rendered.value().maximum_linear_component > 1.0);
  return std::move(rendered.value());
}

void validate_round_trip(const char* path) {
  auto fixture = load_pq_png_as_extended_display_p3(path);
  auto linear = render_linear(fixture);
  LibUltraHdrEncoder encoder;
  auto encoded = encoder.encode(
      UltraHdrEncodeRequest{&linear, UltraHdrJpegQuality::balanced});
  HDRSHOT_CHECK(encoded.has_value());
  NSData* output_data = [NSData dataWithBytes:encoded.value().bytes.data()
                                      length:encoded.value().bytes.size()];
  inspect_apple_decode(output_data, "P3/PQ alternate");
  if (const char* output = std::getenv("HDRSHOT_ULTRAHDR_OUTPUT")) {
    NSString* output_path = [NSString stringWithUTF8String:output];
    HDRSHOT_CHECK(![[NSFileManager defaultManager] fileExistsAtPath:output_path]);
    HDRSHOT_CHECK([output_data writeToFile:output_path atomically:YES]);
  }
  HDRSHOT_CHECK(encoded.value().bytes.size() <
                static_cast<std::size_t>(std::numeric_limits<int>::max()));
  HDRSHOT_CHECK(is_uhdr_image(
      static_cast<void*>(encoded.value().bytes.data()),
      static_cast<int>(encoded.value().bytes.size())) == 1);
  HDRSHOT_CHECK(contains_ascii(
      encoded.value().bytes, "http://ns.adobe.com/hdr-gain-map/1.0/"));
  HDRSHOT_CHECK(contains_ascii(
      encoded.value().bytes, "http://ns.adobe.com/xap/1.0/"));
  HDRSHOT_CHECK(contains_ascii(
      encoded.value().bytes, "urn:iso:std:iso:ts:21496:-1"));

  DecoderHandle decoder{uhdr_create_decoder(), &uhdr_release_decoder};
  HDRSHOT_CHECK(decoder != nullptr);
  uhdr_compressed_image_t input{};
  input.data = encoded.value().bytes.data();
  input.data_sz = encoded.value().bytes.size();
  input.capacity = input.data_sz;
  input.cg = UHDR_CG_UNSPECIFIED;
  input.ct = UHDR_CT_UNSPECIFIED;
  input.range = UHDR_CR_UNSPECIFIED;
  HDRSHOT_CHECK(
      uhdr_dec_set_image(decoder.get(), &input).error_code == UHDR_CODEC_OK);
  HDRSHOT_CHECK(
      uhdr_dec_set_out_img_format(
          decoder.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat).error_code ==
      UHDR_CODEC_OK);
  HDRSHOT_CHECK(
      uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR).error_code ==
      UHDR_CODEC_OK);
  const auto boost = static_cast<float>(std::clamp(
      linear.maximum_linear_component, 1.0,
      10000.0 / kUltraHdrReferenceWhiteNits));
  HDRSHOT_CHECK(
      uhdr_dec_set_out_max_display_boost(decoder.get(), boost).error_code ==
      UHDR_CODEC_OK);
  HDRSHOT_CHECK(uhdr_decode(decoder.get()).error_code == UHDR_CODEC_OK);
  hdrshot::test::check_p3_base_and_alternate(decoder.get());
  hdrshot::test::check_dual_metadata(decoder.get());
  const auto* gain = uhdr_get_decoded_gainmap_image(decoder.get());
  HDRSHOT_CHECK(gain != nullptr && gain->fmt == UHDR_IMG_FMT_32bppRGBA8888);
  HDRSHOT_CHECK(gain->w == static_cast<unsigned int>(fixture.size_px.width));
  HDRSHOT_CHECK(gain->h == static_cast<unsigned int>(fixture.size_px.height));
  const auto* decoded = uhdr_get_decoded_image(decoder.get());
  HDRSHOT_CHECK(decoded != nullptr);
  HDRSHOT_CHECK(decoded->fmt == UHDR_IMG_FMT_64bppRGBAHalfFloat);
  HDRSHOT_CHECK(decoded->cg == UHDR_CG_DISPLAY_P3);
  HDRSHOT_CHECK(decoded->ct == UHDR_CT_LINEAR);
  HDRSHOT_CHECK(decoded->w == static_cast<unsigned int>(fixture.size_px.width));
  HDRSHOT_CHECK(decoded->h == static_cast<unsigned int>(fixture.size_px.height));
  const auto* decoded_half = static_cast<const std::uint16_t*>(
      decoded->planes[UHDR_PLANE_PACKED]);
  HDRSHOT_CHECK(decoded_half != nullptr);
  const auto decoded_stride =
      static_cast<std::size_t>(decoded->stride[UHDR_PLANE_PACKED]);

  double relative_error_sum = 0.0;
  double maximum_relative_error = 0.0;
  PixelPoint maximum_error_position{};
  double maximum_error_source_peak = 0.0;
  double maximum_error_decoded_peak = 0.0;
  std::size_t compared = 0U;
  for (std::int32_t y = 32; y < fixture.size_px.height; y += 64) {
    for (std::int32_t x = 32; x < fixture.size_px.width; x += 64) {
      const auto source_pixel =
          static_cast<std::size_t>(y * fixture.size_px.width + x) * 4U;
      const auto decoded_pixel =
          (static_cast<std::size_t>(y) * decoded_stride +
           static_cast<std::size_t>(x)) * 4U;
      double source_peak = 0.0;
      double decoded_peak = 0.0;
      for (std::size_t channel = 0U; channel < 3U; ++channel) {
        const auto source_value = ExtendedP3Mapper::decode_binary16(
            linear.rgba_half[source_pixel + channel]);
        const auto decoded_value = ExtendedP3Mapper::decode_binary16(
            decoded_half[decoded_pixel + channel]);
        HDRSHOT_CHECK(source_value.has_value() && decoded_value.has_value());
        source_peak = std::max(
            source_peak, static_cast<double>(source_value.value()));
        decoded_peak = std::max(
            decoded_peak, static_cast<double>(decoded_value.value()));
      }
      if (source_peak < 0.02 || source_peak >= boost * 0.98) continue;
      const auto relative_error =
          std::abs(decoded_peak - source_peak) / std::max(0.05, source_peak);
      relative_error_sum += relative_error;
      if (relative_error > maximum_relative_error) {
        maximum_relative_error = relative_error;
        maximum_error_position = PixelPoint{x, y};
        maximum_error_source_peak = source_peak;
        maximum_error_decoded_peak = decoded_peak;
      }
      ++compared;
    }
  }
  HDRSHOT_CHECK(compared >= 100U);
  const auto mean_relative_error = relative_error_sum /
      static_cast<double>(compared);
  std::cout << "fixture encoded_bytes=" << encoded.value().bytes.size()
            << " compared_samples=" << compared
            << " mean_peak_relative_error=" << mean_relative_error
            << " max_peak_relative_error=" << maximum_relative_error
            << " max_error_xy=" << maximum_error_position.x << ','
            << maximum_error_position.y
            << " source_peak=" << maximum_error_source_peak
            << " decoded_peak=" << maximum_error_decoded_peak << '\n';
  std::cout << "max_error_linear_p3 source=";
  for (std::size_t c = 0; c < 3; ++c) {
    const auto p = static_cast<std::size_t>(maximum_error_position.y) *
        static_cast<std::size_t>(fixture.size_px.width) +
        static_cast<std::size_t>(maximum_error_position.x);
    std::cout << ExtendedP3Mapper::decode_binary16(linear.rgba_half[p * 4U + c]).value() << ',';
  }
  std::cout << " decoded=";
  for (std::size_t c = 0; c < 3; ++c) {
    const auto p = static_cast<std::size_t>(maximum_error_position.y) * decoded_stride +
        static_cast<std::size_t>(maximum_error_position.x);
    std::cout << ExtendedP3Mapper::decode_binary16(decoded_half[p * 4U + c]).value() << ',';
  }
  std::cout << '\n';
  // Sparse smoke guard only. Full original-PNG RGB/patch and boundary analysis
  // lives in analyze_ultra_hdr_color_patches.py; this is NOT its quality gate.
  HDRSHOT_CHECK(mean_relative_error < 0.02);
  HDRSHOT_CHECK(maximum_relative_error < 0.05);
}

}  // namespace

int main() {
  @autoreleasepool {
    const char* fixture = std::getenv("HDRSHOT_ULTRAHDR_FIXTURE");
    if (fixture == nullptr || fixture[0] == '\0') {
      std::cout << "[SKIP] HDRSHOT_ULTRAHDR_FIXTURE is not set\n";
      return 77;
    }
    return hdrshot::test::run({
        {"specified PQ PNG survives Ultra HDR encode/decode",
         [fixture] { validate_round_trip(fixture); }},
    });
  }
}
