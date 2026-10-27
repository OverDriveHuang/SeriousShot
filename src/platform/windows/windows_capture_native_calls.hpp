#pragma once
#include <functional>
#include <winrt/Windows.Graphics.Capture.h>
namespace hdrshot {
// Narrow production seam. Default-empty functions always execute native calls.
// Tests can hold/throw at either boundary without replacing the adapter, its
// frame validation, call provenance, cancellation checks or work tickets.
struct WindowsCaptureNativeCalls {
  std::function<void(const char*, bool)> boundary;
  std::function<winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame(
      const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool&)> fetch;
};
}
