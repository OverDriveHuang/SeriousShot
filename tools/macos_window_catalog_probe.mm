#include "platform/macos/macos_window_catalog.hpp"
#import <CoreGraphics/CoreGraphics.h>
#include <chrono>
#include <future>
#include <iostream>

// Read-only metadata smoke, usable while locked. No capture, windows, titles,
// permission prompts or user input; an unavailable catalog is a fallback result.
int main() {
  using namespace hdrshot;
  using namespace std::chrono;
  const auto start = steady_clock::now();
  CGDirectDisplayID ids[32]{};
  uint32_t count{};
  if (CGGetActiveDisplayList(32, ids, &count) != kCGErrorSuccess) {
    std::cout << "display_catalog=unavailable\n";
    return 0;
  }
  SnapshotWindowsRequest request{{1}, {1}, {1, {}}};
  for (uint32_t i = 0; i < count; ++i) {
    const auto rect = CGDisplayBounds(ids[i]);
    request.displays.displays.push_back({DisplayId{ids[i]},
        {rect.origin.x, rect.origin.y, rect.size.width, rect.size.height}, 1,
        {static_cast<int32_t>(CGDisplayPixelsWide(ids[i])),
         static_cast<int32_t>(CGDisplayPixelsHigh(ids[i]))}});
  }
  auto promise = std::make_shared<std::promise<Result<WindowSnapshot, Error>>>();
  auto ready = promise->get_future();
  MacWindowCatalogPort catalog;
  catalog.snapshot_windows(request, [promise](auto result) { promise->set_value(std::move(result)); });
  if (ready.wait_for(seconds{2}) != std::future_status::ready) {
    std::cout << "contract_timeout=true\n";
    return 1;
  }
  const auto result = ready.get();
  std::cout << "displays=" << count << " status=" << (result ? "ok" : "manual_fallback")
            << " fragments=" << (result ? result.value().candidates.size() : 0)
            << " elapsed_ms=" << duration_cast<milliseconds>(steady_clock::now() - start).count() << '\n';
  return 0;
}
