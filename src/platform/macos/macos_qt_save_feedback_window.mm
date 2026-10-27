#include "platform/macos/macos_qt_save_feedback_window.hpp"
#include <QGuiApplication>
#include <QWidget>
#import <AppKit/AppKit.h>

namespace hdrshot {
void configure_macos_save_feedback_window(QWidget &window) {
  if (QGuiApplication::platformName() != "cocoa" || !window.internalWinId()) return;
  NSView *view = (__bridge NSView *)(void *)window.internalWinId();
  if (!view || ![view isKindOfClass:NSView.class]) return;
  NSWindow *native = view.window;
  if (native) native.animationBehavior = NSWindowAnimationBehaviorNone;
}
} // namespace hdrshot
