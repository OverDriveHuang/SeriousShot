#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <dispatch/dispatch.h>

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

std::string color_space_name(CGColorSpaceRef color_space) {
  if (color_space == nullptr) {
    return {};
  }
  CFStringRef name = CGColorSpaceCopyName(color_space);
  if (name == nullptr) {
    return {};
  }
  char buffer[256]{};
  const bool converted = CFStringGetCString(
      name,
      buffer,
      static_cast<CFIndex>(sizeof(buffer)),
      kCFStringEncodingUTF8);
  CFRelease(name);
  return converted ? std::string(buffer) : std::string{};
}

}  // namespace

int main() {
  [SCShareableContent getShareableContentWithCompletionHandler:^(
      SCShareableContent* content,
      NSError* content_error) {
    if (content_error != nil || content == nil) {
      std::cerr << "enumerate failed code=" << content_error.code << '\n';
      std::exit(2);
    }
    if (content.displays.count < 2) {
      std::cerr << "need at least two displays, got=" << content.displays.count << '\n';
      std::exit(3);
    }

    CGRect region = CGRectNull;
    for (SCDisplay* display in content.displays) {
      region = CGRectIsNull(region) ? display.frame : CGRectUnion(region, display.frame);
      std::cout << "displayId=" << display.displayID
                << " frame=" << display.frame.origin.x << ',' << display.frame.origin.y << ','
                << display.frame.size.width << ',' << display.frame.size.height << '\n';
    }
    std::cout << "union=" << region.origin.x << ',' << region.origin.y << ','
              << region.size.width << ',' << region.size.height << '\n';

    if (@available(macOS 15.2, *)) {
      [SCScreenshotManager captureImageInRect:region completionHandler:^(
        CGImageRef image,
        NSError* capture_error) {
      if (capture_error != nil || image == nullptr) {
        std::cerr << "atomic capture failed code=" << capture_error.code << '\n';
        std::exit(4);
      }
      std::cout << "atomicCapture=success"
                << " size=" << CGImageGetWidth(image) << 'x' << CGImageGetHeight(image)
                << " bitsPerComponent=" << CGImageGetBitsPerComponent(image)
                << " bitsPerPixel=" << CGImageGetBitsPerPixel(image)
                << " bytesPerRow=" << CGImageGetBytesPerRow(image)
                << " colorSpace=" << color_space_name(CGImageGetColorSpace(image))
                << " contentHeadroom=" << std::fixed << std::setprecision(6)
                << CGImageGetContentHeadroom(image);
      if (@available(macOS 26.0, *)) {
        std::cout << " calculatedHeadroom=" << CGImageCalculateContentHeadroom(image);
      } else {
        std::cout << " calculatedHeadroom=unavailable";
      }
      std::cout << '\n';
      std::exit(0);
      }];
    } else {
      std::cerr << "atomic region probe requires macOS 15.2 or newer\n";
      std::exit(5);
    }
  }];
  dispatch_main();
}
