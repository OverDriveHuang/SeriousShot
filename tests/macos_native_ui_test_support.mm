#include "macos_native_ui_test_support.hpp"
#include <QWidget>
#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <iostream>
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include "test_support.hpp"
namespace hdrshot::test {
namespace {
bool activate_pid(std::int64_t pid) {
  NSRunningApplication *app=[NSRunningApplication runningApplicationWithProcessIdentifier:static_cast<pid_t>(pid)];
  if(!app || ![app activateWithOptions:0]) return false;
  QElapsedTimer deadline; deadline.start();
  while(deadline.elapsed()<1000) {
    QApplication::processEvents();
    if(NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier==pid) return true;
    QThread::msleep(5);
  }
  return false;
}
}
NativeBackgroundScope::NativeBackgroundScope()
  : original_(NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier) {
  // Only existing regular applications; prefer Finder, never launch or open.
  for(NSRunningApplication *app in NSWorkspace.sharedWorkspace.runningApplications) {
    if(app.activationPolicy==NSApplicationActivationPolicyRegular &&
       app.processIdentifier!=NSProcessInfo.processInfo.processIdentifier && !app.terminated) {
      if(!external_ || [app.bundleIdentifier isEqualToString:@"com.apple.finder"])
        external_=app.processIdentifier;
      if([app.bundleIdentifier isEqualToString:@"com.apple.finder"]) break;
    }
  }
}
NativeBackgroundScope::~NativeBackgroundScope() {
  if(original_ && !activate_pid(original_)) std::cerr << "foreground restore failed pid=" << original_ << std::endl;
}
bool NativeBackgroundScope::activate() {
  std::cout << "native-background original-pid=" << original_
            << " own-pid=" << NSProcessInfo.processInfo.processIdentifier
            << " external-pid=" << external_ << std::endl;
  return external_ && activate_pid(external_);
}
NativeWindowState native_window_state(QWidget &widget) {
  NSView *view=(__bridge NSView *)(void *)widget.winId();
  NSWindow *window=view.window;
  bool on_screen=false;
  CFArrayRef windows=CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly,kCGNullWindowID);
  if(windows) {
    for(NSDictionary *info in (__bridge NSArray *)windows)
      if([info[(__bridge NSString *)kCGWindowNumber] integerValue]==window.windowNumber) on_screen=true;
    CFRelease(windows);
  }
  return {bool(window.visible),on_screen,bool(window.hidesOnDeactivate),window.windowNumber};
}
NativeWindowContract native_window_contract(QWidget &widget) {
  if(QGuiApplication::platformName()!="cocoa" || !widget.internalWinId()) return {};
  NSView *view=(__bridge NSView *)(void *)widget.internalWinId();
  if(!view || ![view isKindOfClass:NSView.class]) return {};
  NSWindow *window=view.window;
  if(!window || ![window isKindOfClass:NSWindow.class])
    return {reinterpret_cast<std::uintptr_t>((__bridge void *)view)};
  return {reinterpret_cast<std::uintptr_t>((__bridge void *)view),
          reinterpret_cast<std::uintptr_t>((__bridge void *)window),
          bool(window.hidesOnDeactivate),bool(window.canBecomeKeyWindow)};
}
bool native_desktop_ready() {
  CFDictionaryRef session=CGSessionCopyCurrentDictionary();
  bool unlocked=false;
  if(session) {
    unlocked=CFDictionaryGetValue(session,CFSTR("CGSSessionScreenIsLocked"))!=kCFBooleanTrue &&
             CFDictionaryGetValue(session,kCGSessionOnConsoleKey)==kCFBooleanTrue;
    CFRelease(session);
  }
  const bool screen_access=CGPreflightScreenCaptureAccess();
  const bool ready=QGuiApplication::platformName()=="cocoa" && unlocked && screen_access;
  std::cout << "native-desktop platform=" << QGuiApplication::platformName().toStdString()
            << " unlocked-console=" << unlocked << " screen-access=" << screen_access << std::endl;
  if(!ready) std::cerr << "[INCOMPLETE] Native UI requires Cocoa, unlocked console session and screen capture permission; no prompt issued" << std::endl;
  return ready;
}
NativeFocus native_focus() {
  return {NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier,
          reinterpret_cast<std::uintptr_t>((__bridge void *)NSApp.keyWindow)};
}
bool native_make_key(QWidget &window) {
  auto *root = (__bridge NSView *)(void *)window.winId();
  [NSApp activateIgnoringOtherApps:YES];
  [root.window makeKeyAndOrderFront:nil];
  QElapsedTimer deadline; deadline.start();
  while(deadline.elapsed()<1000) {
    QApplication::processEvents();
    if(root.window.keyWindow && NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == NSProcessInfo.processInfo.processIdentifier) return true;
    QThread::msleep(5);
  }
  std::cerr << "native-key-baseline failed key=" << root.window.keyWindow
            << " native-window=" << (__bridge void *)root.window
            << " front-pid=" << NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier
            << " own-pid=" << NSProcessInfo.processInfo.processIdentifier
            << " active=" << NSApp.active << " policy=" << NSApp.activationPolicy << std::endl;
  return false;
}
void native_mouse(QWidget &target, QPoint point, bool down, bool right) {
  QWidget *window = target.window();
  NSView *root = (__bridge NSView *)(void *)window->winId();
  const auto in_root = target.mapTo(window, point);
  NSPoint local = NSMakePoint(in_root.x(), root.flipped ? in_root.y() : root.bounds.size.height-in_root.y());
  NSView *hit = [root hitTest:local]; HDRSHOT_CHECK(hit != nil);
  NSEventType type = right ? (down ? NSEventTypeRightMouseDown : NSEventTypeRightMouseUp)
                          : (down ? NSEventTypeLeftMouseDown : NSEventTypeLeftMouseUp);
  NSEvent *event = [NSEvent mouseEventWithType:type location:[root convertPoint:local toView:nil]
      modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:root.window.windowNumber
      context:nil eventNumber:0 clickCount:1 pressure:down?1:0];
  if(right) { if(down) [hit rightMouseDown:event]; else [hit rightMouseUp:event]; }
  else { if(down) [hit mouseDown:event]; else [hit mouseUp:event]; }
  QApplication::processEvents();
}
void native_space(QWidget &target, bool down) {
  NSView *root = (__bridge NSView *)(void *)target.window()->winId();
  NSEvent *event = [NSEvent keyEventWithType:down?NSEventTypeKeyDown:NSEventTypeKeyUp
      location:NSZeroPoint modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime
      windowNumber:root.window.windowNumber context:nil characters:@" " charactersIgnoringModifiers:@" "
      isARepeat:NO keyCode:49];
  [NSApp sendEvent:event]; QApplication::processEvents();
}
}
