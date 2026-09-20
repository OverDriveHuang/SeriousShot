#include "platform/macos/screen_capture_kit_adapter.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <array>
#include <cstring>

namespace {

using namespace hdrshot;

MacCaptureFrameDescriptor valid_descriptor() {
  return MacCaptureFrameDescriptor{
      42,
      3840,
      2160,
      3840 * 8,
      kMacRgbaHalfFourcc,
      "P3_D65",
      "Extended_SRGB",
      0.0,
  };
}

void frozen_contract_is_accepted() {
  const auto result = MacScreenCaptureKitAdapter::validate_descriptor(valid_descriptor());
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value().pixel_format_fourcc == kMacRgbaHalfFourcc);
}

void invalid_pixel_and_metadata_are_rejected() {
  auto descriptor = valid_descriptor();
  descriptor.pixel_format_fourcc = 0;
  const auto bad_pixel = MacScreenCaptureKitAdapter::validate_descriptor(descriptor);
  HDRSHOT_CHECK(!bad_pixel.has_value());
  HDRSHOT_CHECK(bad_pixel.error().code == ErrorCode::invalid_color_contract);

  descriptor = valid_descriptor();
  descriptor.transfer_function = "SMPTE_ST_2084_PQ";
  const auto bad_transfer = MacScreenCaptureKitAdapter::validate_descriptor(descriptor);
  HDRSHOT_CHECK(!bad_transfer.has_value());
  HDRSHOT_CHECK(bad_transfer.error().code == ErrorCode::invalid_color_contract);

  descriptor = valid_descriptor();
  descriptor.transfer_function = "Linear";
  HDRSHOT_CHECK(!MacScreenCaptureKitAdapter::validate_descriptor(descriptor));
}

void row_stride_and_dimensions_are_checked() {
  auto descriptor = valid_descriptor();
  descriptor.source_bytes_per_row -= 1;
  HDRSHOT_CHECK(!MacScreenCaptureKitAdapter::validate_descriptor(descriptor).has_value());

  descriptor = valid_descriptor();
  descriptor.height_px = 0;
  HDRSHOT_CHECK(!MacScreenCaptureKitAdapter::validate_descriptor(descriptor).has_value());
}

void native_errors_map_to_stable_errors() {
  const auto denied = MacScreenCaptureKitAdapter::map_stream_error_code(-3801, true);
  HDRSHOT_CHECK(denied.code == ErrorCode::permission_denied);
  HDRSHOT_CHECK(denied.retryability == Retryability::after_user_action);

  const auto no_display = MacScreenCaptureKitAdapter::map_stream_error_code(-3814, true);
  HDRSHOT_CHECK(no_display.code == ErrorCode::object_not_found);

  const auto other = MacScreenCaptureKitAdapter::map_stream_error_code(-3811, true);
  HDRSHOT_CHECK(other.code == ErrorCode::capture_failed);
}

void cg_image_half_layout_and_lifetime() {
  // Two padded rows, with encoded Extended P3 values (not linear EDR).
  std::array<std::uint16_t, 24> samples{
      0x3400,0x3c00,0x4000,0x3c00, 0x4400,0x4000,0x3400,0x3c00, 0,0,0,0,
      0x4800,0x3c00,0x3400,0x3c00, 0x4000,0x4400,0x4800,0x3c00, 0,0,0,0};
  auto space = CGColorSpaceCreateWithName(kCGColorSpaceExtendedDisplayP3);
  auto data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(samples.data()), sizeof(samples));
  auto provider = CGDataProviderCreateWithCFData(data);
  auto bitmap = static_cast<CGBitmapInfo>(kCGBitmapFloatComponents |
      kCGBitmapByteOrder16Little | kCGImageAlphaPremultipliedLast);
  auto image = CGImageCreate(2, 2, 16, 64, 24, space, bitmap, provider,
      nullptr, false, kCGRenderingIntentDefault);
  HDRSHOT_CHECK(image != nullptr);
  auto result = MacScreenCaptureKitAdapter::copy_image(image, 42);
  CGImageRelease(image); CGDataProviderRelease(provider); CFRelease(data); CGColorSpaceRelease(space);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value().descriptor.transfer_function == "Extended_SRGB");
  HDRSHOT_CHECK(result.value().descriptor.returned_space == "ExtendedDisplayP3");
  HDRSHOT_CHECK(result.value().descriptor.bitmap_info == bitmap);
  HDRSHOT_CHECK(result.value().rgba_half_extended_p3.size() == 16);
  for (std::size_t y = 0; y < 2; ++y)
    for (std::size_t x = 0; x < 8; ++x)
      HDRSHOT_CHECK(result.value().rgba_half_extended_p3[y*8+x] == samples[y*12+x]);
  HDRSHOT_CHECK(!MacScreenCaptureKitAdapter::copy_image(nullptr, 42));

  auto sdr = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  std::array<std::uint8_t, 4> bytes{255, 0, 0, 255};
  auto p = CGDataProviderCreateWithData(nullptr, bytes.data(), bytes.size(), nullptr);
  auto wrong = CGImageCreate(1,1,8,32,4,sdr,kCGImageAlphaLast,p,nullptr,false,kCGRenderingIntentDefault);
  HDRSHOT_CHECK(wrong);
  HDRSHOT_CHECK(!MacScreenCaptureKitAdapter::copy_image(wrong, 42));
  CGImageRelease(wrong); CGDataProviderRelease(p); CGColorSpaceRelease(sdr);
}

void cg_image_alpha_is_ignored_without_changing_rgb() {
  // Include the adjacent FP16 value below 1, substantial transparency and
  // non-finite Alpha: none is part of the desktop RGB contract. No unpremultiply.
  constexpr std::array<std::uint16_t, 6> alphas{0x3bff, 0x3800, 0x0000, 0x7e00, 0x7c00, 0xbc00};
  for (const auto alpha_info : {kCGImageAlphaPremultipliedLast, kCGImageAlphaLast,
                                kCGImageAlphaNoneSkipLast}) {
    std::array<std::uint16_t, 32> samples{}; // 3 pixels/row with 8 bytes padding.
    for (std::size_t i = 0; i < alphas.size(); ++i) {
      const auto offset = (i / 3) * 16 + (i % 3) * 4;
      samples[offset] = 0x3800;     // 0.5 encoded; dividing by Alpha would change it.
      samples[offset + 1] = 0x3e00; // HDR code > 1 must remain untouched.
      samples[offset + 2] = 0xb400; // Signed extended sample must not be clamped here.
      samples[offset + 3] = alphas[i];
    }
    auto space = CGColorSpaceCreateWithName(kCGColorSpaceExtendedDisplayP3);
    auto data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(samples.data()), sizeof(samples));
    auto provider = CGDataProviderCreateWithCFData(data);
    const auto bitmap = static_cast<CGBitmapInfo>(kCGBitmapFloatComponents |
        kCGBitmapByteOrder16Little | alpha_info);
    auto image = CGImageCreate(3, 2, 16, 64, 32, space, bitmap, provider,
        nullptr, false, kCGRenderingIntentDefault);
    HDRSHOT_CHECK(image);
    auto result = MacScreenCaptureKitAdapter::copy_image(image, 42);
    CGImageRelease(image); CGDataProviderRelease(provider); CFRelease(data); CGColorSpaceRelease(space);
    HDRSHOT_CHECK(result.has_value());
    HDRSHOT_CHECK(result.value().rgba_half_extended_p3.size() == 24);
    for (std::size_t i = 0; i < alphas.size(); ++i) {
      const auto& rgb = result.value().rgba_half_extended_p3;
      HDRSHOT_CHECK(rgb[i*4] == 0x3800);
      HDRSHOT_CHECK(rgb[i*4+1] == 0x3e00);
      HDRSHOT_CHECK(rgb[i*4+2] == 0xb400);
      HDRSHOT_CHECK(rgb[i*4+3] == 0x3c00);
      HDRSHOT_CHECK(samples[(i/3)*16+(i%3)*4+3] == alphas[i]);
    }
  }
}

}  // namespace

int main() {
  using hdrshot::test::TestCase;
  return hdrshot::test::run(std::vector<TestCase>{
      {"macOS frozen capture contract", frozen_contract_is_accepted},
      {"CGImage FP16 padded layout and owned lifetime", cg_image_half_layout_and_lifetime},
      {"CGImage ignores Alpha and preserves signed HDR RGB", cg_image_alpha_is_ignored_without_changing_rgb},
      {"macOS rejects wrong pixel metadata", invalid_pixel_and_metadata_are_rejected},
      {"macOS checks dimensions and stride", row_stride_and_dimensions_are_checked},
      {"macOS maps native errors", native_errors_map_to_stable_errors},
  });
}
