#pragma once
#include "platform/macos/screen_capture_kit_adapter.hpp"
namespace hdrshot {
// Test seam at the two system submission boundaries. The production work state,
// cancellation gates, progress ordering and drain tickets remain in the adapter.
using MacImageCallback = std::function<void(CGImageRef, std::optional<Error>)>;
using MacImageSubmit = std::function<void(MacImageCallback)>;
using MacContentCallback = std::function<void(Result<MacImageSubmit, Error>)>;
struct MacNativeCaptureFunctions {
  // Contract: return after submitting exactly one callback, or throw before submission.
  std::function<void(std::uint32_t, MacContentCallback)> content;
};
void mac_capture_with_native(std::uint32_t display_id,
    MacScreenCaptureKitAdapter::CaptureCompletion completion,
    MacCaptureHooks hooks, MacNativeCaptureFunctions native);
}

namespace hdrshot {
using MacEdrRanges = std::map<std::uint32_t, std::pair<double, double>>;
struct MacNativeDisplayFunctions {
  std::function<MacEdrRanges()> snapshot_ranges;
  std::function<void(std::function<void()>)> submit;
  std::function<void(MacScreenCaptureKitAdapter::DisplayListCompletion)> content;
};
// Called on the owner thread: freeze scalar EDR first, then submit native work.
void mac_enumerate_with_native(MacScreenCaptureKitAdapter::DisplayListCompletion completion,
    std::function<bool()> cancelled, std::function<void(const char*, const char*)> stage,
    MacNativeDisplayFunctions native);
}
