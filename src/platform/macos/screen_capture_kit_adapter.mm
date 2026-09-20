#include "platform/macos/screen_capture_kit_adapter.hpp"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace hdrshot {
namespace {

constexpr double kNoAbsoluteSourceWhiteNits = 0.0;

Error adapter_error(
    const ErrorCode code,
    const Retryability retryability,
    std::map<std::string, std::string> context = {},
    std::source_location origin = std::source_location::current()) {
  return Error{code, "MacScreenCaptureKitAdapter", retryability, std::move(context), origin};
}

Error map_error(NSError* error,
    std::source_location origin = std::source_location::current()) {
  if (error == nil) {
    return adapter_error(ErrorCode::capture_failed, Retryability::same_input);
  }
  const bool stream_domain = [error.domain isEqualToString:SCStreamErrorDomain];
  auto mapped = MacScreenCaptureKitAdapter::map_stream_error_code(
      static_cast<std::int64_t>(error.code), stream_domain);
  mapped.safe_context["nativeCode"] = std::to_string(error.code);
  mapped.origin = origin;
  return mapped;
}

SCDisplay* find_display(SCShareableContent* content, const std::uint32_t display_id) {
  for (SCDisplay* display in content.displays) {
    if (display.displayID == display_id) {
      return display;
    }
  }
  return nil;
}

std::pair<double, double> edr_range_info(const std::uint32_t display_id) {
  __block double current = 1.0;
  __block double potential = 1.0;
  const auto query = ^{
    const auto key = NSDeviceDescriptionKey(@"NSScreenNumber");
    for (NSScreen* screen in NSScreen.screens) {
      NSNumber* number = screen.deviceDescription[key];
      if (number != nil && number.unsignedIntValue == display_id) {
        current = static_cast<double>(
            screen.maximumExtendedDynamicRangeColorComponentValue);
        potential = static_cast<double>(
            screen.maximumPotentialExtendedDynamicRangeColorComponentValue);
        break;
      }
    }
  };
  if (NSThread.isMainThread) {
    query();
  } else {
    dispatch_sync(dispatch_get_main_queue(), query);
  }
  return {current, potential};
}

void request_shareable_content(
    void (^completion)(SCShareableContent* content, NSError* error)) {
  [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                            onScreenWindowsOnly:NO
                                               completionHandler:completion];
}

std::pair<std::size_t, std::size_t> capture_pixel_size(SCContentFilter* filter) {
  const auto* info = [SCShareableContent infoForFilter:filter];
  const auto scale = static_cast<double>(info.pointPixelScale);
  const auto width = static_cast<double>(info.contentRect.size.width) * scale;
  const auto height = static_cast<double>(info.contentRect.size.height) * scale;
  if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0 || height <= 0.0) {
    return {0, 0};
  }
  return {
      static_cast<std::size_t>(std::llround(width)),
      static_cast<std::size_t>(std::llround(height)),
  };
}


}  // namespace

Result<MacCapturedFrame, Error> MacScreenCaptureKitAdapter::copy_image(
    CGImageRef image, const std::uint32_t display_id) {
  const auto fail = [](const char* reason,
      std::source_location origin = std::source_location::current()) {
    return Result<MacCapturedFrame, Error>::failure(adapter_error(
        ErrorCode::invalid_color_contract, Retryability::never, {{"reason", reason}}, origin));
  };
  if (image == nullptr) return fail("missing_cg_image");
  const auto bitmap = CGImageGetBitmapInfo(image);
  const auto alpha = CGImageGetAlphaInfo(image);
  // RGBA half-float in native little endian. No silent ColorSync conversion or
  // reinterpretation of an 8-bit/packed/planar return. ICC validation is T18.
  if (CGImageGetBitsPerComponent(image) != 16 || CGImageGetBitsPerPixel(image) != 64 ||
      !(bitmap & kCGBitmapFloatComponents) ||
      (bitmap & kCGBitmapByteOrderMask) != kCGBitmapByteOrder16Little ||
      (alpha != kCGImageAlphaPremultipliedLast && alpha != kCGImageAlphaLast &&
       alpha != kCGImageAlphaNoneSkipLast))
    return fail("unsupported_cg_image_layout");

  MacCaptureFrameDescriptor descriptor{
      display_id, CGImageGetWidth(image), CGImageGetHeight(image),
      CGImageGetBytesPerRow(image), kMacRgbaHalfFourcc, "P3_D65", "Extended_SRGB",
      kNoAbsoluteSourceWhiteNits, 0, {}};
  descriptor.bitmap_info = bitmap;
  const auto space = CGImageGetColorSpace(image);
  const auto name = space ? CGColorSpaceGetName(space) : nullptr;
  // Observe only known system identifiers. Unknown/private profiles are never
  // dumped or rejected here; request-vs-ICC validation remains pending (T18).
  descriptor.returned_space = !space ? "absent" :
      name && CFEqual(name, kCGColorSpaceExtendedLinearDisplayP3) ? "ExtendedLinearDisplayP3" :
      name && CFEqual(name, kCGColorSpaceExtendedDisplayP3) ? "ExtendedDisplayP3" :
      name && CFEqual(name, kCGColorSpaceDisplayP3) ? "DisplayP3" : "other";
  auto validated = validate_descriptor(descriptor);
  if (!validated) return Result<MacCapturedFrame, Error>::failure(validated.error());
  const auto width = descriptor.width_px;
  const auto height = descriptor.height_px;
  if (height > std::numeric_limits<std::size_t>::max() / descriptor.source_bytes_per_row)
    return fail("frame_size_overflow");
  CGDataProviderRef provider = CGImageGetDataProvider(image);
  CFDataRef data = provider ? CGDataProviderCopyData(provider) : nullptr;
  if (!data) return fail("missing_cg_image_data");
  const auto needed = (height - 1) * descriptor.source_bytes_per_row + width * 8;
  if (static_cast<std::size_t>(CFDataGetLength(data)) < needed) {
    CFRelease(data);
    return fail("truncated_cg_image_data");
  }
  std::vector<std::uint16_t> samples(width * height * 4);
  const auto* bytes = CFDataGetBytePtr(data);
  for (std::size_t y = 0; y < height; ++y)
    std::memcpy(samples.data() + y * width * 4,
                bytes + y * descriptor.source_bytes_per_row, width * 8);
  CFRelease(data);
  // ADR-018: desktop RGB is authoritative; ignore returned Alpha even when the
  // image is labelled premultiplied. Do not reject, divide RGB by A, or composite
  // another background. The fourth component is only our opaque storage lane.
  for (std::size_t pixel = 0; pixel < width * height; ++pixel) {
    samples[pixel * 4 + 3] = 0x3c00;
  }
  return Result<MacCapturedFrame, Error>::success(
      MacCapturedFrame{std::move(descriptor), std::move(samples)});
}

bool MacScreenCaptureKitAdapter::has_capture_access() noexcept {
  return CGPreflightScreenCaptureAccess();
}

void MacScreenCaptureKitAdapter::enumerate_displays(DisplayListCompletion completion) {
  request_shareable_content(^(
      SCShareableContent* content,
      NSError* error) {
    if (error != nil || content == nil) {
      completion(Result<std::vector<MacDisplayInfo>, Error>::failure(map_error(error)));
      return;
    }
    std::vector<MacDisplayInfo> displays;
    displays.reserve(content.displays.count);
    for (SCDisplay* display in content.displays) {
      SCContentFilter* filter =
          [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
      const auto* info = [SCShareableContent infoForFilter:filter];
      const auto [capture_width, capture_height] = capture_pixel_size(filter);
      const auto [current_edr, potential_edr] = edr_range_info(display.displayID);
      displays.push_back(MacDisplayInfo{
          display.displayID,
          static_cast<double>(display.frame.origin.x),
          static_cast<double>(display.frame.origin.y),
          static_cast<double>(display.frame.size.width),
          static_cast<double>(display.frame.size.height),
          static_cast<double>(info.pointPixelScale),
          capture_width,
          capture_height,
          current_edr,
          potential_edr,
      });
    }
    std::sort(displays.begin(), displays.end(), [](const auto& left, const auto& right) {
      return left.display_id < right.display_id;
    });
    completion(Result<std::vector<MacDisplayInfo>, Error>::success(std::move(displays)));
  });
}

void MacScreenCaptureKitAdapter::capture_display(
    const std::uint32_t display_id,
    CaptureCompletion completion) {
  request_shareable_content(^(
      SCShareableContent* content,
      NSError* content_error) {
    if (content_error != nil || content == nil) {
      completion(Result<MacCapturedFrame, Error>::failure(map_error(content_error)));
      return;
    }
    SCDisplay* display = find_display(content, display_id);
    if (display == nil) {
      completion(Result<MacCapturedFrame, Error>::failure(adapter_error(
          ErrorCode::object_not_found,
          Retryability::after_user_action,
          {{"displayId", std::to_string(display_id)}})));
      return;
    }

    SCContentFilter* filter =
        [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
    const auto [capture_width, capture_height] = capture_pixel_size(filter);
    if (capture_width == 0 || capture_height == 0) {
      completion(Result<MacCapturedFrame, Error>::failure(adapter_error(
          ErrorCode::invalid_input,
          Retryability::after_recreate,
          {{"reason", "invalid_capture_scale"}})));
      return;
    }
    SCStreamConfiguration* configuration = [SCStreamConfiguration
        streamConfigurationWithPreset:
            SCStreamConfigurationPresetCaptureHDRScreenshotCanonicalDisplay];
    configuration.width = capture_width;
    configuration.height = capture_height;
    configuration.pixelFormat = kCVPixelFormatType_64RGBAHalf;
    configuration.colorSpaceName = kCGColorSpaceExtendedDisplayP3;
    configuration.captureDynamicRange = SCCaptureDynamicRangeHDRCanonicalDisplay;
    configuration.capturesAudio = NO;
    configuration.showsCursor = NO;
    configuration.shouldBeOpaque = YES;

    [SCScreenshotManager captureImageWithFilter:filter
                                         configuration:configuration
                                     completionHandler:^(
        CGImageRef image,
        NSError* capture_error) {
      if (capture_error != nil) {
        completion(Result<MacCapturedFrame, Error>::failure(map_error(capture_error)));
        return;
      }
      completion(copy_image(image, display_id));
    }];
  });
}

Result<MacCaptureFrameDescriptor, Error> MacScreenCaptureKitAdapter::validate_descriptor(
    MacCaptureFrameDescriptor descriptor) {
  if (descriptor.width_px == 0 || descriptor.height_px == 0) {
    return Result<MacCaptureFrameDescriptor, Error>::failure(adapter_error(
        ErrorCode::invalid_color_contract,
        Retryability::never,
        {{"reason", "empty_dimensions"}}));
  }
  constexpr std::size_t bytes_per_pixel = 8;
  if (descriptor.width_px > std::numeric_limits<std::size_t>::max() / bytes_per_pixel ||
      descriptor.source_bytes_per_row < descriptor.width_px * bytes_per_pixel) {
    return Result<MacCaptureFrameDescriptor, Error>::failure(adapter_error(
        ErrorCode::invalid_color_contract,
        Retryability::never,
        {{"reason", "invalid_row_stride"}}));
  }
  if (descriptor.pixel_format_fourcc != kMacRgbaHalfFourcc) {
    return Result<MacCaptureFrameDescriptor, Error>::failure(adapter_error(
        ErrorCode::invalid_color_contract,
        Retryability::never,
        {{"reason", "pixel_format_not_RGhA"}}));
  }
  if (descriptor.color_primaries != "P3_D65" ||
      descriptor.transfer_function != "Extended_SRGB" ||
      descriptor.source_reference_white_nits != kNoAbsoluteSourceWhiteNits) {
    return Result<MacCaptureFrameDescriptor, Error>::failure(adapter_error(
        ErrorCode::invalid_color_contract,
        Retryability::never,
        {{"reason", "unexpected_extended_display_p3_contract"}}));
  }
  return Result<MacCaptureFrameDescriptor, Error>::success(std::move(descriptor));
}

Error MacScreenCaptureKitAdapter::map_stream_error_code(
    const std::int64_t code,
    const bool stream_error_domain) {
  if (stream_error_domain && code == -3801) {
    return adapter_error(ErrorCode::permission_denied, Retryability::after_user_action);
  }
  if (stream_error_domain && (code == -3813 || code == -3814 || code == -3815)) {
    return adapter_error(ErrorCode::object_not_found, Retryability::after_user_action);
  }
  return adapter_error(ErrorCode::capture_failed, Retryability::same_input);
}

}  // namespace hdrshot
