#include "platform/macos/macos_launch_origin.hpp"
#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>
#include <optional>
namespace hdrshot {
struct MacLaunchOriginObserver::Impl {
  id token;
  std::optional<MacLaunchOrigin> origin;
  std::function<void(MacLaunchOrigin)> consumer;
  bool delivered{};
  void deliver() {
    if (origin && consumer && !delivered) {
      delivered = true;
      auto callback = std::move(consumer);
      callback(*origin);
    }
  }
};
MacLaunchOriginObserver::MacLaunchOriginObserver() : impl_(std::make_unique<Impl>()) {
  auto* state = impl_.get();
  state->token = [NSNotificationCenter.defaultCenter
      addObserverForName:NSApplicationDidFinishLaunchingNotification object:nil queue:nil
      usingBlock:^(NSNotification*) {
        if (state->origin) return;
        NSAppleEventDescriptor* event = NSAppleEventManager.sharedAppleEventManager.currentAppleEvent;
        const bool login = event && event.eventClass == kCoreEventClass &&
            event.eventID == kAEOpenApplication &&
            [event paramDescriptorForKeyword:keyAEPropData].enumCodeValue == keyAELaunchedAsLogInItem;
        state->origin = login ? MacLaunchOrigin::login : MacLaunchOrigin::manual;
        state->deliver();
      }];
}
MacLaunchOriginObserver::~MacLaunchOriginObserver() {
  [NSNotificationCenter.defaultCenter removeObserver:impl_->token];
}
void MacLaunchOriginObserver::when_ready(std::function<void(MacLaunchOrigin)> consumer) {
  impl_->consumer = std::move(consumer);
  impl_->deliver();
}
} // namespace hdrshot
