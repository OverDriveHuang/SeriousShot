#include "platform/macos/metal_edr_presenter.hpp"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace {

std::uint16_t float_to_half_bits(const float value) {
  const _Float16 half = static_cast<_Float16>(value);
  std::uint16_t bits{};
  static_assert(sizeof(half) == sizeof(bits));
  std::memcpy(&bits, &half, sizeof(bits));
  return bits;
}

float linear_to_extended_srgb(const float linear) {
  return linear <= 0.0031308F
      ? linear * 12.92F
      : 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
}

NSScreen* choose_edr_screen() {
  NSScreen* best = NSScreen.mainScreen;
  CGFloat best_headroom = best.maximumPotentialExtendedDynamicRangeColorComponentValue;
  for (NSScreen* screen in NSScreen.screens) {
    const auto headroom = screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
    if (headroom > best_headroom) {
      best = screen;
      best_headroom = headroom;
    }
  }
  return best;
}

hdrshot::MacMetalOverlayRequest make_fixture(const std::size_t width, const std::size_t height) {
  std::vector<std::uint16_t> samples;
  samples.resize(width * height * 4U);
  for (std::size_t y = 0; y < height; ++y) {
    for (std::size_t x = 0; x < width; ++x) {
      const auto band = std::min<std::size_t>(3, x * 4U / width);
      constexpr float edr_levels[4] = {0.25F, 1.0F, 2.0F, 4.0F};
      const auto edr = edr_levels[band];
      const auto encoded = float_to_half_bits(linear_to_extended_srgb(edr));
      const auto offset = (y * width + x) * 4U;
      samples[offset] = encoded;
      samples[offset + 1U] = encoded;
      samples[offset + 2U] = encoded;
      samples[offset + 3U] = float_to_half_bits(1.0F);
    }
  }
  const auto selection_x = static_cast<std::int32_t>(width / 5U);
  const auto selection_y = static_cast<std::int32_t>(height / 5U);
  const auto selection_width = static_cast<std::int32_t>(width * 3U / 5U);
  const auto selection_height = static_cast<std::int32_t>(height * 3U / 5U);
  return hdrshot::MacMetalOverlayRequest{
      width,
      height,
      std::make_shared<const std::vector<std::uint16_t>>(std::move(samples)),
      hdrshot::PixelRect{selection_x, selection_y, selection_width, selection_height},
      0.35F,
      2.03F,
      3,
  };
}

}  // namespace

int main() {
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    NSScreen* screen = choose_edr_screen();
    if (screen == nil) {
      std::cerr << "No screen available\n";
      return 1;
    }
    const NSRect frame = screen.frame;
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskBorderless
                    backing:NSBackingStoreBuffered
                      defer:NO
                     screen:screen];
    window.releasedWhenClosed = NO;
    window.opaque = YES;
    window.backgroundColor = NSColor.blackColor;
    window.level = NSScreenSaverWindowLevel;
    window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                NSWindowCollectionBehaviorFullScreenAuxiliary;
    [window setFrame:screen.frame display:YES];

    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, frame.size.width, frame.size.height)];
    view.wantsLayer = YES;
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.frame = view.bounds;
    layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    layer.contentsScale = screen.backingScaleFactor;
    layer.drawableSize = CGSizeMake(
        frame.size.width * screen.backingScaleFactor,
        frame.size.height * screen.backingScaleFactor);
    view.layer = layer;
    window.contentView = view;
    [window orderFrontRegardless];
    [NSApp activateIgnoringOtherApps:YES];

    auto presenter_result = hdrshot::MacMetalEdrPresenter::create();
    if (!presenter_result) {
      std::cerr << "Presenter creation failed\n";
      return 2;
    }
    auto presenter = std::move(presenter_result.value());
    auto fixture = make_fixture(1200, 700);
    const auto configured = presenter->configure_target_layer(
        (__bridge void*)layer, fixture.target_surface_range);
    if (!configured) {
      std::cerr << "Layer configuration failed: "
                << hdrshot::to_string(configured.error().code) << '\n';
      [NSApp terminate:nil];
      return 3;
    }
    auto receipt = presenter->present_to_layer((__bridge void*)layer, fixture);
    if (!receipt) {
      std::cerr << "Presentation failed\n";
      return 3;
    }

    std::cout << "screenMaxEDRAtPresent="
              << screen.maximumExtendedDynamicRangeColorComponentValue
              << " screenMaxPotentialEDR="
              << screen.maximumPotentialExtendedDynamicRangeColorComponentValue
              << " drawable=" << receipt.value().drawable_width_px << "x"
              << receipt.value().drawable_height_px
              << " format=" << receipt.value().texture_format
              << " colorspace=" << receipt.value().layer_color_space
              << " opticalOutputScaleNits=" << receipt.value().optical_output_scale_nits
              << "\n";
    std::cout << "Visual contract: center selection is undimmed; outside is linear-light 35%; "
                 "the rightmost 400-nit band must remain visibly brighter than SDR white.\n";

    const auto pump_events_until = ^(NSDate* end) {
      while (window.visible && end.timeIntervalSinceNow > 0.0) {
      NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                          untilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]
                                             inMode:NSDefaultRunLoopMode
                                            dequeue:YES];
      if (event != nil) {
        [NSApp sendEvent:event];
      }
      [NSApp updateWindows];
      }
    };
    pump_events_until([NSDate dateWithTimeIntervalSinceNow:2.0]);
    receipt = presenter->present_to_layer((__bridge void*)layer, fixture);
    if (!receipt) {
      std::cerr << "Second presentation failed\n";
      return 4;
    }
    std::cout << "screenMaxEDRAfterSecondFrame="
              << screen.maximumExtendedDynamicRangeColorComponentValue << "\n";
    pump_events_until([NSDate dateWithTimeIntervalSinceNow:18.0]);
    std::cout << "screenMaxEDRAfterDisplay="
              << screen.maximumExtendedDynamicRangeColorComponentValue << "\n";
    [window orderOut:nil];
    return 0;
  }
}
