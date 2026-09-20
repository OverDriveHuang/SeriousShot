#include "platform/macos/macos_global_hotkey_port.hpp"

#import <AppKit/AppKit.h>

#include <atomic>
#include <iostream>
#include <memory>

namespace {

void stop_application_loop() {
  [NSApp stop:nil];
  NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                      location:NSZeroPoint
                                 modifierFlags:0
                                     timestamp:0
                                  windowNumber:0
                                       context:nil
                                       subtype:0
                                         data1:0
                                         data2:0];
  [NSApp postEvent:wake atStart:NO];
}

}  // namespace

int main() {
  std::cout << std::unitbuf;
  [NSApplication sharedApplication];
  [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

  auto triggered = std::make_shared<std::atomic<bool>>(false);
  hdrshot::MacGlobalHotkeyPort hotkey;
  hotkey.set_trigger_handler([triggered] {
    triggered->store(true);
    std::cout << "globalHotkeyTriggered=1 hotkey=Command+Shift+2\n";
    stop_application_loop();
  });
  const auto registered = hotkey.register_hotkey("Command+Shift+2");
  if (!registered) {
    std::cerr << "globalHotkeyRegistered=0 error="
              << hdrshot::to_string(registered.error().code) << '\n';
    return 1;
  }
  std::cout << "globalHotkeyRegistered=1 hotkey=" << registered.value().active_hotkey
            << " timeoutSeconds=30\n";
  dispatch_after(
      dispatch_time(DISPATCH_TIME_NOW, 30LL * NSEC_PER_SEC),
      dispatch_get_main_queue(),
      ^{
        if (!triggered->load()) {
          std::cout << "globalHotkeyTriggered=0 reason=timeout\n";
        }
        stop_application_loop();
      });
  [NSApp run];
  const auto unregistered = hotkey.unregister_hotkey();
  if (!unregistered) {
    std::cerr << "globalHotkeyUnregistered=0 error="
              << hdrshot::to_string(unregistered.error().code) << '\n';
    return 2;
  }
  std::cout << "globalHotkeyUnregistered=1\n";
  return triggered->load() ? 0 : 3;
}
