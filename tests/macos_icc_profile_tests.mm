#include "domain/color/bt2020_srgb_icc_profile.hpp"
#include "domain/color/display_p3_icc_profile.hpp"
#include "test_support.hpp"

#include <CoreGraphics/CoreGraphics.h>

namespace {

void colorsync_accepts_bt2020_srgb_profile() {
  const auto profile = hdrshot::Bt2020SrgbIccProfile::bytes();
  CFDataRef data = CFDataCreate(
      kCFAllocatorDefault,
      profile.data(),
      static_cast<CFIndex>(profile.size()));
  HDRSHOT_CHECK(data != nullptr);
  CGColorSpaceRef color_space = CGColorSpaceCreateWithICCData(data);
  HDRSHOT_CHECK(color_space != nullptr);
  HDRSHOT_CHECK(CGColorSpaceGetModel(color_space) == kCGColorSpaceModelRGB);
  HDRSHOT_CHECK(CGColorSpaceGetNumberOfComponents(color_space) == 3U);

  CFDataRef round_trip = CGColorSpaceCopyICCData(color_space);
  HDRSHOT_CHECK(round_trip != nullptr);
  HDRSHOT_CHECK(CFDataGetLength(round_trip) >= 128);

  CFRelease(round_trip);
  CGColorSpaceRelease(color_space);
  CFRelease(data);
}

void colorsync_accepts_display_p3_profile() {
  const auto profile = hdrshot::DisplayP3IccProfile::bytes();
  CFDataRef data = CFDataCreate(
      kCFAllocatorDefault,
      profile.data(),
      static_cast<CFIndex>(profile.size()));
  HDRSHOT_CHECK(data != nullptr);
  CGColorSpaceRef color_space = CGColorSpaceCreateWithICCData(data);
  HDRSHOT_CHECK(color_space != nullptr);
  HDRSHOT_CHECK(CGColorSpaceGetModel(color_space) == kCGColorSpaceModelRGB);
  HDRSHOT_CHECK(CGColorSpaceGetNumberOfComponents(color_space) == 3U);
  CFRelease(color_space);
  CFRelease(data);
}

}  // namespace

int main() {
  return hdrshot::test::run({
      {"Mac ColorSync accepts BT.2020+sRGB ICC", colorsync_accepts_bt2020_srgb_profile},
      {"Mac ColorSync accepts Display P3 ICC", colorsync_accepts_display_p3_profile},
  });
}
