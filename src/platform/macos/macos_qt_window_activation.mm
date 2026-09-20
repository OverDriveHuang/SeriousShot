#include "platform/macos/macos_qt_window_activation.hpp"
#import <AppKit/AppKit.h>
namespace hdrshot {
void MacQtWindowActivation::activate(QWidget& window) {
  window.showNormal();
  NSView* view = (__bridge NSView*)(reinterpret_cast<void*>(window.winId()));
  NSWindow* native = view.window;
  if (native != nil) {
    native.hidesOnDeactivate = NO;
    native.collectionBehavior = NSWindowCollectionBehaviorMoveToActiveSpace |
        NSWindowCollectionBehaviorFullScreenAuxiliary;
    if (native.miniaturized) [native deminiaturize:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [native makeKeyAndOrderFront:nil];
  }
  window.raise();
  window.activateWindow();
}
}  // namespace hdrshot
