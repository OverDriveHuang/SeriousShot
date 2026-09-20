#include "platform/macos/metal_edr_presenter.hpp"
#include "platform/macos/screen_capture_kit_adapter.hpp"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

NSScreen* choose_xdr_screen() {
  NSScreen* best = NSScreen.mainScreen;
  CGFloat best_headroom = best.maximumPotentialExtendedDynamicRangeColorComponentValue;
  for (NSScreen* screen in NSScreen.screens) {
    if (screen.maximumPotentialExtendedDynamicRangeColorComponentValue > best_headroom) {
      best = screen;
      best_headroom = screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
    }
  }
  return best;
}

std::uint32_t display_id(NSScreen* screen) {
  const auto key = NSDeviceDescriptionKey(@"NSScreenNumber");
  NSNumber* number = screen.deviceDescription[key];
  return number == nil ? 0U : number.unsignedIntValue;
}

void print_error(const hdrshot::Error& error) {
  std::cerr << hdrshot::to_string(error.code);
  for (const auto& [key, value] : error.safe_context) {
    std::cerr << ' ' << key << '=' << value;
  }
  std::cerr << '\n';
}

}  // namespace

int main() {
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSScreen* target_screen = choose_xdr_screen();
    if (target_screen == nil) {
      std::cerr << "No screen available\n";
      return 1;
    }
    const auto target_display_id = display_id(target_screen);
    if (target_display_id == 0) {
      std::cerr << "Unable to resolve target display ID\n";
      return 2;
    }
    std::cout << "captureTargetDisplayId=" << target_display_id
              << " logicalPt=" << target_screen.frame.size.width << 'x'
              << target_screen.frame.size.height
              << " backingScale=" << target_screen.backingScaleFactor
              << " maximumPotentialEDR="
              << target_screen.maximumPotentialExtendedDynamicRangeColorComponentValue << '\n';

    hdrshot::MacScreenCaptureKitAdapter::capture_display(
        target_display_id,
        [target_screen](auto capture_result) mutable {
      if (!capture_result) {
        std::cerr << "Capture failed: ";
        print_error(capture_result.error());
        dispatch_async(dispatch_get_main_queue(), ^{
          [NSApp stop:nil];
        });
        return;
      }
      auto captured = std::make_shared<hdrshot::MacCapturedFrame>(
          std::move(capture_result.value()));
      dispatch_async(dispatch_get_main_queue(), ^{
        const auto frame_width = captured->descriptor.width_px;
        const auto frame_height = captured->descriptor.height_px;
        std::cout << "capturedPx=" << frame_width << 'x' << frame_height
                  << " sourceWhiteNits=" << captured->descriptor.source_reference_white_nits
                  << " transfer=" << captured->descriptor.transfer_function << '\n';

        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:target_screen.frame
                      styleMask:NSWindowStyleMaskBorderless
                        backing:NSBackingStoreBuffered
                          defer:NO
                         screen:target_screen];
        window.releasedWhenClosed = NO;
        window.opaque = YES;
        window.backgroundColor = NSColor.blackColor;
        window.level = NSScreenSaverWindowLevel;
        window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                    NSWindowCollectionBehaviorFullScreenAuxiliary;
        [window setFrame:target_screen.frame display:YES];

        NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(
            0, 0, target_screen.frame.size.width, target_screen.frame.size.height)];
        view.wantsLayer = YES;
        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.frame = view.bounds;
        layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
        layer.contentsScale = target_screen.backingScaleFactor;
        layer.drawableSize = CGSizeMake(
            static_cast<CGFloat>(frame_width), static_cast<CGFloat>(frame_height));
        view.layer = layer;
        window.contentView = view;
        [window orderFrontRegardless];
        [NSApp activateIgnoringOtherApps:YES];

        auto created = hdrshot::MacMetalEdrPresenter::create();
        if (!created) {
          std::cerr << "Presenter creation failed: ";
          print_error(created.error());
          [NSApp stop:nil];
          return;
        }
        auto presenter = std::shared_ptr<hdrshot::MacMetalEdrPresenter>(
            std::move(created.value()));
        auto request = std::make_shared<hdrshot::MacMetalOverlayRequest>(
            hdrshot::MacMetalOverlayRequest{
                frame_width,
                frame_height,
                std::shared_ptr<const std::vector<std::uint16_t>>(
                    captured, &captured->rgba_half_extended_p3),
                hdrshot::PixelRect{
                    static_cast<std::int32_t>(frame_width / 5U),
                    static_cast<std::int32_t>(frame_height / 5U),
                    static_cast<std::int32_t>(frame_width * 3U / 5U),
                    static_cast<std::int32_t>(frame_height * 3U / 5U),
                },
                0.35F,
                2.03F,
                4,
                hdrshot::MacMetalSurfaceRange::edr,
                0,
                true,
            });

        const auto configured = presenter->configure_target_layer(
            (__bridge void*)layer, request->target_surface_range);
        if (!configured) {
          print_error(configured.error());
          [NSApp terminate:nil];
          return;
        }
        auto receipt = presenter->present_to_layer((__bridge void*)layer, *request);
        if (!receipt) {
          std::cerr << "Initial presentation failed: ";
          print_error(receipt.error());
          [NSApp stop:nil];
          return;
        }
        std::cout << "currentEDR_t0="
                  << target_screen.maximumExtendedDynamicRangeColorComponentValue << '\n';

        dispatch_after(
            dispatch_time(DISPATCH_TIME_NOW, static_cast<std::int64_t>(2 * NSEC_PER_SEC)),
            dispatch_get_main_queue(), ^{
          auto second = presenter->present_to_layer((__bridge void*)layer, *request);
          if (!second) {
            std::cerr << "Second presentation failed: ";
            print_error(second.error());
          }
          std::cout << "currentEDR_t2="
                    << target_screen.maximumExtendedDynamicRangeColorComponentValue << '\n';
        });
        dispatch_after(
            dispatch_time(DISPATCH_TIME_NOW, static_cast<std::int64_t>(5 * NSEC_PER_SEC)),
            dispatch_get_main_queue(), ^{
          std::cout << "currentEDR_t5="
                    << target_screen.maximumExtendedDynamicRangeColorComponentValue << '\n';
        });
        dispatch_after(
            dispatch_time(DISPATCH_TIME_NOW, static_cast<std::int64_t>(10 * NSEC_PER_SEC)),
            dispatch_get_main_queue(), ^{
          std::cout << "currentEDR_t10="
                    << target_screen.maximumExtendedDynamicRangeColorComponentValue << '\n';
        });
        dispatch_after(
            dispatch_time(DISPATCH_TIME_NOW, static_cast<std::int64_t>(20 * NSEC_PER_SEC)),
            dispatch_get_main_queue(), ^{
          std::cout << "currentEDR_t20="
                    << target_screen.maximumExtendedDynamicRangeColorComponentValue << '\n';
          [window orderOut:nil];
          [NSApp stop:nil];
          NSEvent* wake_event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                                   location:NSZeroPoint
                                              modifierFlags:0
                                                  timestamp:0
                                               windowNumber:0
                                                    context:nil
                                                    subtype:0
                                                      data1:0
                                                      data2:0];
          [NSApp postEvent:wake_event atStart:NO];
        });
      });
    });

    [NSApp run];
    return 0;
  }
}
