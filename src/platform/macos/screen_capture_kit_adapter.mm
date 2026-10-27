#include "platform/macos/screen_capture_kit_adapter.hpp"
#include "platform/macos/screen_capture_work.hpp"
#include <atomic>
#include <mutex>
#include <stdexcept>

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
  const char* domain_text = error.domain.UTF8String;
  const std::string domain = domain_text ? domain_text : "";
  if ((stream_domain || [error.domain isEqualToString:NSCocoaErrorDomain] ||
       [error.domain isEqualToString:NSPOSIXErrorDomain] || [error.domain isEqualToString:NSOSStatusErrorDomain]) &&
      !domain.empty() && domain.size() <= 64 && domain.find_first_not_of(
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == std::string::npos)
    mapped.safe_context["nativeDomain"] = domain;
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

using EdrRanges = std::map<std::uint32_t, std::pair<double, double>>;
EdrRanges snapshot_edr_ranges() {
  EdrRanges ranges;
  const auto key = NSDeviceDescriptionKey(@"NSScreenNumber");
  for (NSScreen* screen in NSScreen.screens) {
    NSNumber* number = screen.deviceDescription[key];
    if (number) ranges[number.unsignedIntValue] = {
        static_cast<double>(screen.maximumExtendedDynamicRangeColorComponentValue),
        static_cast<double>(screen.maximumPotentialExtendedDynamicRangeColorComponentValue)};
  }
  return ranges;
}

void request_shareable_content(
    void (^completion)(SCShareableContent* content, NSError* error)) {
  @try {
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                              onScreenWindowsOnly:NO
                                                 completionHandler:completion];
  } @catch (NSException*) { throw std::runtime_error("native_submit_exception"); }
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
    CGImageRef image, const std::uint32_t display_id, std::function<bool()> cancelled) {
  const auto fail = [](const char* reason,
      std::source_location origin = std::source_location::current()) {
    return Result<MacCapturedFrame, Error>::failure(adapter_error(
        ErrorCode::invalid_color_contract, Retryability::never, {{"reason", reason}}, origin));
  };
  if (cancelled && cancelled()) return Result<MacCapturedFrame, Error>::failure(adapter_error(ErrorCode::operation_cancelled, Retryability::never));
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
  struct DataOwner { CFDataRef value; ~DataOwner() { CFRelease(value); } } data_owner{data};
  if (cancelled && cancelled()) return Result<MacCapturedFrame, Error>::failure(adapter_error(ErrorCode::operation_cancelled, Retryability::never));
  const auto needed = (height - 1) * descriptor.source_bytes_per_row + width * 8;
  if (static_cast<std::size_t>(CFDataGetLength(data)) < needed) {
    return fail("truncated_cg_image_data");
  }
  std::vector<std::uint16_t> samples(width * height * 4);
  const auto* bytes = CFDataGetBytePtr(data);
  for (std::size_t y = 0; y < height; ++y) {
    if ((y % 64) == 0 && cancelled && cancelled()) {
        return Result<MacCapturedFrame, Error>::failure(adapter_error(ErrorCode::operation_cancelled, Retryability::never));
    }
    std::memcpy(samples.data() + y * width * 4,
                bytes + y * descriptor.source_bytes_per_row, width * 8);
  }
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

void mac_enumerate_with_native(MacScreenCaptureKitAdapter::DisplayListCompletion completion,
    std::function<bool()> cancelled, std::function<void(const char*, const char*)> stage,
    MacNativeDisplayFunctions native) {
  if (cancelled && cancelled()) return;
  MacEdrRanges ranges;
  try { ranges = native.snapshot_ranges(); } // owner thread, before any native submit
  catch (...) {
    completion(Result<std::vector<MacDisplayInfo>, Error>::failure(adapter_error(ErrorCode::capture_failed,
        Retryability::after_user_action, {{"reason", "display_snapshot_exception"}}))); return;
  }
  auto once = std::make_shared<std::atomic_bool>(false);
  auto deliver = [completion = std::move(completion), cancelled, once](auto result) {
    if (once->exchange(true) || (cancelled && cancelled())) return;
    completion(std::move(result));
  };
  try {
    native.submit([ranges, native, cancelled, stage, deliver] {
      if (cancelled && cancelled()) return;
      try {
        if (stage) stage("getShareableContent", "call_enter");
        native.content([ranges, cancelled, stage, deliver](Result<std::vector<MacDisplayInfo>, Error> result) mutable {
          if (cancelled && cancelled()) return;
          if (result) for (auto& display : result.value()) {
            const auto found = ranges.find(display.display_id);
            if (found != ranges.end()) {
              display.current_maximum_edr = found->second.first;
              display.maximum_potential_edr = found->second.second;
            }
          }
          if (stage) stage("getShareableContent", "callback_received");
          deliver(std::move(result));
        });
        if (stage) stage("getShareableContent", "call_return");
      } catch (...) {
        deliver(Result<std::vector<MacDisplayInfo>, Error>::failure(adapter_error(ErrorCode::capture_failed,
            Retryability::after_user_action, {{"reason", "native_submit_exception"}, {"api", "getShareableContent"}})));
      }
    });
  } catch (...) {
    deliver(Result<std::vector<MacDisplayInfo>, Error>::failure(adapter_error(ErrorCode::capture_failed,
        Retryability::after_user_action, {{"reason", "native_enqueue_exception"}})));
  }
}
void MacScreenCaptureKitAdapter::enumerate_displays(DisplayListCompletion completion, std::function<bool()> cancelled,
    std::function<void(const char*, const char*)> stage) {
  auto initialize = ^{
    MacNativeDisplayFunctions native;
    native.snapshot_ranges = snapshot_edr_ranges;
    native.submit = [](std::function<void()> task) {
      dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ task(); });
    };
    native.content = [cancelled](DisplayListCompletion done) {
      request_shareable_content(^(SCShareableContent* content, NSError* error) {
        try { @try {
        if (cancelled && cancelled()) return;
        if (error || !content) {
          auto mapped = map_error(error); mapped.safe_context["api"] = "getShareableContent";
          done(Result<std::vector<MacDisplayInfo>, Error>::failure(mapped)); return;
        }
        std::vector<MacDisplayInfo> displays;
        displays.reserve(content.displays.count);
        for (SCDisplay* display in content.displays) {
          SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
          const auto* info = [SCShareableContent infoForFilter:filter];
          const auto [width, height] = capture_pixel_size(filter);
          displays.push_back(MacDisplayInfo{display.displayID,
              static_cast<double>(display.frame.origin.x), static_cast<double>(display.frame.origin.y),
              static_cast<double>(display.frame.size.width), static_cast<double>(display.frame.size.height),
              static_cast<double>(info.pointPixelScale), width, height, 1.0, 1.0});
        }
        std::sort(displays.begin(), displays.end(), [](const auto& left, const auto& right) { return left.display_id < right.display_id; });
        done(Result<std::vector<MacDisplayInfo>, Error>::success(std::move(displays)));
        } @catch (NSException*) {
          done(Result<std::vector<MacDisplayInfo>, Error>::failure(adapter_error(ErrorCode::capture_failed,
              Retryability::after_user_action, {{"reason", "native_callback_exception"}, {"api", "getShareableContent"}})));
        } } catch (...) {
          done(Result<std::vector<MacDisplayInfo>, Error>::failure(adapter_error(ErrorCode::capture_failed,
              Retryability::after_user_action, {{"reason", "native_callback_exception"}, {"api", "getShareableContent"}})));
        }
      });
    };
    mac_enumerate_with_native(completion, cancelled, stage, std::move(native));
  };
  if (NSThread.isMainThread) initialize();
  else dispatch_async(dispatch_get_main_queue(), initialize);
}

namespace {
template<class F, class... A> void safe_hook(const F& f, A&&... args) noexcept {
  try { if (f) f(std::forward<A>(args)...); } catch (...) {}
}
struct ScreenCaptureWork : std::enable_shared_from_this<ScreenCaptureWork> {
  std::mutex mutex;
  std::size_t submissions{}, callbacks{};
  bool result_finished{}, drained_once{};
  std::uint32_t display{};
  MacCaptureHooks hooks;
  MacScreenCaptureKitAdapter::CaptureCompletion completion;
  bool cancelled() const { try { return hooks.cancelled && hooks.cancelled(); } catch (...) { return true; } }
  Error cancellation() const { return adapter_error(ErrorCode::operation_cancelled, Retryability::never); }
  void drain() {
    bool fire{};
    { std::lock_guard lock(mutex);
      if (result_finished && !submissions && !callbacks && !drained_once) fire = drained_once = true;
    }
    if (fire) safe_hook(hooks.drained);
  }
  void finish(Result<MacCapturedFrame, Error> result) {
    MacScreenCaptureKitAdapter::CaptureCompletion done;
    { std::lock_guard lock(mutex); if (result_finished) return;
      result_finished = true; done = std::move(completion);
    }
    if (!result && result.error().code != ErrorCode::operation_cancelled) safe_hook(hooks.failed, result.error());
    safe_hook(done, std::move(result)); drain();
  }
  struct Ticket {
    std::shared_ptr<ScreenCaptureWork> work;
    std::atomic_bool claimed{};
    std::atomic_bool returned{};
    bool enter() { return !claimed.exchange(true); }
    void release() {
      if (returned.exchange(true)) return;
      { std::lock_guard lock(work->mutex); --work->callbacks; }
      work->drain();
    }
  };
  struct CallbackScope { std::shared_ptr<Ticket> ticket; ~CallbackScope() { ticket->release(); } };
  template<class Submit> void submit(const char* api, Submit submitter) {
    bool permit = !cancelled();
    try { if (permit && hooks.try_begin_step) permit = hooks.try_begin_step(api); }
    catch (...) { permit = false; }
    if (!permit) { finish(Result<MacCapturedFrame, Error>::failure(cancellation())); return; }
    auto ticket = std::make_shared<Ticket>(); ticket->work = shared_from_this();
    { std::lock_guard lock(mutex); ++submissions; ++callbacks; }
    safe_hook(hooks.stage, api, "call_enter");
    try { submitter(ticket); safe_hook(hooks.stage, api, "call_return"); }
    catch (...) {
      // Native seam contract: throwing means callback was not submitted.
      ticket->release();
      finish(Result<MacCapturedFrame, Error>::failure(adapter_error(ErrorCode::capture_failed,
          Retryability::after_user_action, {{"reason", "native_submit_exception"}, {"api", api}})));
    }
    { std::lock_guard lock(mutex); --submissions; }
    drain();
  }
};
}
void mac_capture_with_native(std::uint32_t display_id,
    MacScreenCaptureKitAdapter::CaptureCompletion completion,
    MacCaptureHooks hooks, MacNativeCaptureFunctions native) {
  auto work = std::make_shared<ScreenCaptureWork>();
  work->display = display_id; work->completion = std::move(completion); work->hooks = std::move(hooks);
  work->submit("getShareableContent", [work, native = std::move(native)](auto ticket) {
    native.content(work->display, [work, ticket](Result<MacImageSubmit, Error> prepared) mutable {
      if (!ticket->enter()) return;
      ScreenCaptureWork::CallbackScope scope{ticket};
      try {
        if (!prepared) { work->finish(Result<MacCapturedFrame, Error>::failure(prepared.error()));
          safe_hook(work->hooks.stage, "getShareableContent", "callback_received"); return; }
        if (work->cancelled()) { work->finish(Result<MacCapturedFrame, Error>::failure(work->cancellation())); return; }
        safe_hook(work->hooks.stage, "getShareableContent", "callback_received");
        // Destroy filter/configuration captured by the submit closure before the content ticket returns.
        {
          auto image_submit = std::move(prepared.value());
          work->submit("captureImageWithFilter", [work, image_submit = std::move(image_submit)](auto image_ticket) {
            image_submit([work, image_ticket](CGImageRef image, std::optional<Error> error) {
              if (!image_ticket->enter()) return;
              ScreenCaptureWork::CallbackScope image_scope{image_ticket};
              try {
                if (error) { work->finish(Result<MacCapturedFrame, Error>::failure(*error));
                  safe_hook(work->hooks.stage, "captureImageWithFilter", "callback_received"); return; }
                if (!image || !CGImageGetWidth(image) || !CGImageGetHeight(image)) {
                  work->finish(Result<MacCapturedFrame, Error>::failure(adapter_error(ErrorCode::capture_failed,
                      Retryability::after_user_action, {{"reason", "missing_cg_image"}, {"api", "captureImageWithFilter"}}))); return;
                }
                if (work->cancelled()) { work->finish(Result<MacCapturedFrame, Error>::failure(work->cancellation())); return; }
                safe_hook(work->hooks.image_received); // Before diagnostics, provider access, copying or layout validation.
                safe_hook(work->hooks.stage, "captureImageWithFilter", "callback_received");
                if (work->cancelled()) { work->finish(Result<MacCapturedFrame, Error>::failure(work->cancellation())); return; }
                safe_hook(work->hooks.stage, "copy_image", "call_enter");
                work->finish(MacScreenCaptureKitAdapter::copy_image(image, work->display, [work] { return work->cancelled(); }));
                safe_hook(work->hooks.stage, "cleanup", "callback_tail");
              } catch (...) {
                work->finish(Result<MacCapturedFrame, Error>::failure(adapter_error(ErrorCode::capture_failed,
                    Retryability::after_user_action, {{"reason", "native_callback_exception"}})));
              }
            });
          });
        }
      } catch (...) {
        work->finish(Result<MacCapturedFrame, Error>::failure(adapter_error(ErrorCode::capture_failed,
            Retryability::after_user_action, {{"reason", "native_callback_exception"}})));
      }
    });
  });
}
void MacScreenCaptureKitAdapter::capture_display(std::uint32_t display_id,
    CaptureCompletion completion, MacCaptureHooks hooks) {
  MacNativeCaptureFunctions native;
  native.content = [cancelled = hooks.cancelled](std::uint32_t id, MacContentCallback callback) {
    request_shareable_content(^(SCShareableContent* content, NSError* error) {
      @try {
        if (error || !content) {
          auto mapped = map_error(error); mapped.safe_context["api"] = "getShareableContent";
          callback(Result<MacImageSubmit, Error>::failure(mapped)); return;
        }
        if (cancelled && cancelled()) {
          callback(Result<MacImageSubmit, Error>::failure(adapter_error(ErrorCode::operation_cancelled, Retryability::never))); return;
        }
        auto prepared = [&]() -> Result<MacImageSubmit, Error> {
        @autoreleasepool {
        SCDisplay* display = find_display(content, id);
        if (!display) { return Result<MacImageSubmit, Error>::failure(adapter_error(ErrorCode::object_not_found,
            Retryability::after_user_action, {{"displayId", std::to_string(id)}})); }
        SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
        const auto [width, height] = capture_pixel_size(filter);
        if (!width || !height) { return Result<MacImageSubmit, Error>::failure(adapter_error(ErrorCode::invalid_input,
            Retryability::after_recreate, {{"reason", "invalid_capture_scale"}})); }
        SCStreamConfiguration* config = [SCStreamConfiguration streamConfigurationWithPreset:
            SCStreamConfigurationPresetCaptureHDRScreenshotCanonicalDisplay];
        config.width = width; config.height = height;
        config.pixelFormat = kCVPixelFormatType_64RGBAHalf;
        config.colorSpaceName = kCGColorSpaceExtendedDisplayP3;
        config.captureDynamicRange = SCCaptureDynamicRangeHDRCanonicalDisplay;
        config.capturesAudio = NO; config.showsCursor = NO; config.shouldBeOpaque = YES;
        return Result<MacImageSubmit, Error>::success([filter, config](MacImageCallback image_callback) {
          @try {
            [SCScreenshotManager captureImageWithFilter:filter configuration:config
                completionHandler:^(CGImageRef image, NSError* image_error) {
              if (image_error) {
                auto mapped = map_error(image_error); mapped.safe_context["api"] = "captureImageWithFilter";
                image_callback(image, mapped);
              } else image_callback(image, {});
            }];
          } @catch (NSException*) { throw std::runtime_error("native_submit_exception"); }
        });
        }
        }();
        callback(std::move(prepared)); // helper's strong native locals are already released
      } @catch (NSException*) {
        callback(Result<MacImageSubmit, Error>::failure(adapter_error(ErrorCode::capture_failed,
            Retryability::after_user_action, {{"reason", "native_callback_exception"}})));
      }
    });
  };
  mac_capture_with_native(display_id, std::move(completion), std::move(hooks), std::move(native));
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
