#include "platform/macos/macos_qt_overlay_window.hpp"
#include "test_support.hpp"

#include <QApplication>
#include <QWidget>
#import <AppKit/AppKit.h>

namespace {
NSWindow* native_window(QWidget& host) {
  NSView* view = (__bridge NSView*)reinterpret_cast<void*>(host.winId());
  return view.window;
}
void prepare_host(QWidget& host) {
  host.setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
  host.setAttribute(Qt::WA_NativeWindow);
  const auto size = NSScreen.mainScreen.frame.size;
  host.resize(qRound(size.width), qRound(size.height));
}
void check_locked(QWidget& host) {
  NSWindow* window = native_window(host);
  HDRSHOT_CHECK(window != nil);
  HDRSHOT_CHECK(!window.visible);
  HDRSHOT_CHECK(!window.movable && !window.movableByWindowBackground);
  HDRSHOT_CHECK((window.styleMask & NSWindowStyleMaskResizable) == 0);
  HDRSHOT_CHECK((window.styleMask & NSWindowStyleMaskFullScreen) == 0);
  HDRSHOT_CHECK(host.minimumSize() == host.maximumSize());
  HDRSHOT_CHECK(host.size() == host.minimumSize());
}
void hidden_overlay_is_fixed_without_native_fullscreen() {
  QWidget host;
  prepare_host(host);
  hdrshot::MacQtOverlayWindow adapter((__bridge void*)NSScreen.mainScreen);
  adapter.prepare(host);
  check_locked(host);
  HDRSHOT_CHECK(adapter.native_surface() != nullptr);
  const auto fixed_size = host.size();
  host.resize(fixed_size + QSize(100, 100));
  HDRSHOT_CHECK(host.size() == fixed_size);
  host.resize(fixed_size - QSize(100, 100));
  HDRSHOT_CHECK(host.size() == fixed_size);
  HDRSHOT_CHECK(native_window(host).level == NSScreenSaverWindowLevel);
}
void reconfiguration_reasserts_native_flags_without_rebuilding_surface() {
  QWidget host;
  prepare_host(host);
  hdrshot::MacQtOverlayWindow adapter((__bridge void*)NSScreen.mainScreen);
  adapter.prepare(host);
  void* surface = adapter.native_surface();
  // Simulate a native style reset. Configure is also called after QWidget::show.
  NSWindow* window = native_window(host);
  window.styleMask |= NSWindowStyleMaskResizable;
  window.movable = YES;
  window.movableByWindowBackground = YES;
  adapter.configure(host, false);
  check_locked(host);
  HDRSHOT_CHECK(adapter.native_surface() == surface);
  adapter.resize(host);
  check_locked(host);
}
void lowering_for_save_dialog_does_not_unlock_overlay() {
  QWidget host;
  prepare_host(host);
  hdrshot::MacQtOverlayWindow adapter((__bridge void*)NSScreen.mainScreen);
  adapter.prepare(host);
  adapter.set_system_dialog_active(host, true, false);
  check_locked(host);
  HDRSHOT_CHECK(native_window(host).level == NSNormalWindowLevel);
  // Restore using the non-showing configuration entry, not orderFrontRegardless.
  adapter.configure(host, false);
  check_locked(host);
  HDRSHOT_CHECK(native_window(host).level == NSScreenSaverWindowLevel);
}
void hidden_and_destroyed_observers_do_not_report_interruption() {
  QWidget host;
  prepare_host(host);
  int interruptions = 0;
  {
    hdrshot::MacQtOverlayWindow adapter((__bridge void*)NSScreen.mainScreen);
    adapter.set_input_interrupted([&] { ++interruptions; });
    adapter.prepare(host);
    adapter.configure(host, false); // must not duplicate observers
    [[NSNotificationCenter defaultCenter]
        postNotificationName:NSWindowDidResignKeyNotification object:native_window(host)];
    QApplication::processEvents();
    HDRSHOT_CHECK(interruptions == 0);
    adapter.restore_input_focus(host); // hidden windows must not activate
    check_locked(host);
    adapter.set_input_interrupted({});
  }
  [[NSNotificationCenter defaultCenter]
      postNotificationName:NSWindowDidResignKeyNotification object:native_window(host)];
  QApplication::processEvents();
  HDRSHOT_CHECK(interruptions == 0);
  HDRSHOT_CHECK(!native_window(host).visible);
}
} // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  if (QApplication::platformName() != QStringLiteral("cocoa") || !NSScreen.mainScreen) {
    std::cout << "SKIP: native Cocoa window/screen unavailable\n";
    return 77;
  }
  return hdrshot::test::run({
      {"hidden native overlay is fixed", hidden_overlay_is_fixed_without_native_fullscreen},
      {"reconfiguration preserves fixed geometry and surface", reconfiguration_reasserts_native_flags_without_rebuilding_surface},
      {"hidden and destroyed focus observers remain inert", hidden_and_destroyed_observers_do_not_report_interruption},
      {"save dialog lowering keeps geometry locked", lowering_for_save_dialog_does_not_unlock_overlay}});
}
