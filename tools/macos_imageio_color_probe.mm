// Standalone ImageIO consumer; deliberately not linked with libultrahdr/libjpeg.
// clang++ -std=c++20 -fobjc-arc tools/macos_imageio_color_probe.mm \
//   -framework Foundation -framework CoreGraphics -framework ImageIO \
//   -framework ColorSync -o /tmp/imageio_probe
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <ColorSync/ColorSync.h>
#include <iostream>
#include <vector>

int main(int argc, const char* argv[]) {
  @autoreleasepool {
    if (argc != 2) return 2;
    auto url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
    NSData* file = [NSData dataWithContentsOfURL:url];
    auto bytes = static_cast<const unsigned char*>(file.bytes);
    std::vector<unsigned char> base{0xff, 0xd8};
    for (std::size_t start = 0; start + 3 < file.length; ++start) {
      if (bytes[start] != 0xff || bytes[start+1] != 0xd8 || bytes[start+2] != 0xff) continue;
      for (std::size_t at = start+2; at+4 <= file.length;) {
        auto marker = bytes[at+1];
        if (bytes[at] != 0xff || marker == 0xd9) break;
        if (marker == 0xda) {
          if (start == 0) {
            std::size_t end = at;
            while (end + 1 < file.length && !(bytes[end] == 0xff && bytes[end+1] == 0xd9)) ++end;
            base.insert(base.end(), bytes+at, bytes+end+2);
          }
          break;
        }
        auto length = (std::size_t{bytes[at+2]} << 8U) | bytes[at+3];
        if (length < 2 || length > file.length-at-2) break;
        bool icc = marker == 0xe2 && length > 16 && memcmp(bytes+at+4, "ICC_PROFILE\0", 12) == 0;
        if (icc) {
          auto data = [NSData dataWithBytes:bytes+at+18 length:length-16];
          auto color = CGColorSpaceCreateWithICCData((__bridge CFDataRef)data);
          CFErrorRef errors = nullptr, warnings = nullptr;
          auto profile = ColorSyncProfileCreate((__bridge CFDataRef)data, &errors);
          const bool verified = profile && ColorSyncProfileVerify(profile, &errors, &warnings);
          std::cout << "ICC JPEG_start=" << start << " bytes=" << data.length
                    << " CGColorSpace_valid=" << (color != nullptr)
                    << " ColorSync_verified=" << verified << '\n';
          if (errors) { CFShow(errors); CFRelease(errors); }
          if (warnings) { CFShow(warnings); CFRelease(warnings); }
          if (profile) CFRelease(profile);
          if (color) CGColorSpaceRelease(color);
        }
        if (start == 0 && (marker == 0xe0 || marker < 0xe0 || icc))
          base.insert(base.end(), bytes+at, bytes+at+length+2);
        at += length+2;
      }
    }
    auto base_data = [NSData dataWithBytes:base.data() length:base.size()];
    auto base_source = CGImageSourceCreateWithData((__bridge CFDataRef)base_data, nullptr);
    auto base_image = base_source ? CGImageSourceCreateImageAtIndex(base_source, 0, nullptr) : nullptr;
    std::cout << "isolated_base_decode=" << (base_image != nullptr) << '\n';
    if (base_image) CGImageRelease(base_image);
    if (base_source) CFRelease(base_source);
    CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
    if (!source) return 3;
    std::cout << "file=" << argv[1] << " image_count=" << CGImageSourceGetCount(source) << '\n';
    int failures = 0;
    for (int mode = 0; mode < 3; ++mode) {
      NSDictionary* options = mode == 0 ? @{} : @{
          (__bridge NSString*)kCGImageSourceDecodeRequest:
              (__bridge NSString*)(mode == 1 ? kCGImageSourceDecodeToSDR : kCGImageSourceDecodeToHDR)};
      CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, (__bridge CFDictionaryRef)options);
      if (!image) {
        std::cout << "mode=" << mode << " decode_failed status="
                  << CGImageSourceGetStatusAtIndex(source, 0) << '\n';
        ++failures;
        continue;
      }
      CFStringRef name = CGColorSpaceCopyName(CGImageGetColorSpace(image));
      NSString* text = (__bridge NSString*)name;
      CFDataRef icc = CGColorSpaceCopyICCData(CGImageGetColorSpace(image));
      std::cout << "mode=" << mode << " colorspace="
                << (text ? text.UTF8String : "(unnamed)")
                << " bits=" << CGImageGetBitsPerComponent(image)
                << " icc_bytes=" << (icc ? CFDataGetLength(icc) : 0)
                << " size=" << CGImageGetWidth(image) << 'x' << CGImageGetHeight(image) << '\n';
      if (name) CFRelease(name);
      if (icc) CFRelease(icc);
      CGImageRelease(image);
    }
    CFRelease(source);
    return failures ? 1 : 0;
  }
}
