#include "platform/macos/macos_qt_overlay_window.hpp"
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <iostream>
#include <QApplication>
#include <QPointer>
#include <QTimer>
#include <QVariant>
namespace hdrshot {
struct MacQtOverlayWindow::Impl : std::enable_shared_from_this<Impl> {
  NSScreen* native_screen_{nil};
  CAMetalLayer* layer_{nil};
  QPointer<QWidget> host_;
  __weak NSWindow* watched_window_{nil};
  id inactive_observer_{nil};
  id key_observer_{nil};
  bool presented_{false}, system_dialog_active_{false};
  std::function<void()> input_interrupted_;

  ~Impl() { disconnect(); }
  void disconnect() {
    if (inactive_observer_) [[NSNotificationCenter defaultCenter] removeObserver:inactive_observer_];
    if (key_observer_) [[NSNotificationCenter defaultCenter] removeObserver:key_observer_];
    inactive_observer_ = key_observer_ = nil;
  }
  bool watching_input() const {
    return presented_ && host_ && host_->isVisible() && !system_dialog_active_;
  }
  void report_interruption() {
    if (watching_input() && input_interrupted_) input_interrupted_();
  }
  bool internal_focus_transfer() const {
    if (QApplication::activePopupWidget() || QApplication::activeModalWidget() ||
        NSApp.modalWindow) return true;
    // Resigning key is normal when another display's Overlay becomes key.
    for (QWidget* top : QApplication::topLevelWidgets()) {
      if (!top->isVisible() || !top->property("seriousshotCaptureOverlay").toBool()) continue;
      NSView* view = (__bridge NSView*)reinterpret_cast<void*>(top->winId());
      if (view.window == NSApp.keyWindow && NSApp.active) return true;
    }
    return false;
  }
  void watch(QWidget& host) {
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(host.winId());
    if (watched_window_ == view.window && inactive_observer_) return;
    disconnect();
    host_ = &host;
    watched_window_ = view.window;
    const auto weak = weak_from_this();
    inactive_observer_ = [[NSNotificationCenter defaultCenter]
        addObserverForName:NSApplicationDidResignActiveNotification object:NSApp
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*) {
      if (auto self = weak.lock()) self->report_interruption();
    }];
    key_observer_ = [[NSNotificationCenter defaultCenter]
        addObserverForName:NSWindowDidResignKeyNotification object:view.window
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*) {
      auto self = weak.lock();
      if (!self || !self->watching_input()) return;
      // AppKit transfers key between windows in separate notifications. Check
      // after this event, never poll or force activation while the user is away.
      QTimer::singleShot(0, self->host_, [weak] {
        if (auto state = weak.lock(); state && !state->internal_focus_transfer())
          state->report_interruption();
      });
    }];
  }
  void configure(QWidget& host, const bool order_front) {
    // The desktop surface is fixed; only the shared editor's selection changes.
    // FramelessWindowHint alone still produces a resizable Cocoa window. Keep
    // both Qt constraints and native flags locked, including after show/reset.
    if (native_screen_ != nil) {
      const auto size = native_screen_.frame.size;
      host.setFixedSize(qRound(size.width), qRound(size.height));
    } else {
      host.setFixedSize(host.size());
    }
    NSView* native_view = (__bridge NSView*)reinterpret_cast<void*>(host.winId());
    NSWindow* native_window = native_view.window;
    native_window.styleMask &= ~NSWindowStyleMaskResizable;
    native_window.movable = NO;
    native_window.movableByWindowBackground = NO;
    native_window.level = NSScreenSaverWindowLevel;
    native_window.collectionBehavior =
        NSWindowCollectionBehaviorCanJoinAllSpaces |
        NSWindowCollectionBehaviorStationary |
        NSWindowCollectionBehaviorFullScreenAuxiliary |
        NSWindowCollectionBehaviorIgnoresCycle;
    native_window.opaque = YES;
    native_window.hasShadow = NO;
    native_window.hidesOnDeactivate = NO;
    native_window.backgroundColor = NSColor.blackColor;
    [native_window setFrame:native_screen_.frame display:YES];
    if (order_front) {
      presented_ = true;
      [NSApp activateIgnoringOtherApps:YES];
      [native_window makeKeyAndOrderFront:nil];
      [native_window orderFrontRegardless];
    }

    const bool native_fullscreen =
        (native_window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    const bool native_panel = [native_window isKindOfClass:[NSPanel class]];
    std::cout << "qtHostWindowMode=borderless_overlay"
              << " nativeFullscreen=" << (native_fullscreen ? 1 : 0)
              << " nativePanel=" << (native_panel ? 1 : 0)
              << " hidesOnDeactivate=" << (native_window.hidesOnDeactivate ? 1 : 0)
              << " windowLevel=" << native_window.level
              << " joinsAllSpaces=1 stationary=1 orderedFront=" << (order_front ? 1 : 0)
              << '\n';
  }

  void create_layer(QWidget& host) {
    NSView* native_view = (__bridge NSView*)reinterpret_cast<void*>(host.winId());
    native_view.wantsLayer = YES;
    layer_ = [CAMetalLayer layer];
    layer_.frame = native_view.bounds;
    layer_.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    layer_.contentsScale = native_screen_.backingScaleFactor;
    const auto backing = [native_view convertRectToBacking:native_view.bounds];
    layer_.drawableSize = backing.size;
    [native_view.layer addSublayer:layer_];
  }


};
MacQtOverlayWindow::MacQtOverlayWindow(void* screen) : impl_(std::make_shared<Impl>()) {
  impl_->native_screen_ = (__bridge NSScreen*)screen;
}
MacQtOverlayWindow::~MacQtOverlayWindow() = default;
void MacQtOverlayWindow::prepare(QWidget& host) {
  impl_->configure(host, false);
  impl_->create_layer(host);
  impl_->watch(host);
}
void MacQtOverlayWindow::configure(QWidget& host, bool front) {
  impl_->configure(host, front);
  impl_->watch(host);
}
void MacQtOverlayWindow::set_input_interrupted(std::function<void()> callback) {
  impl_->input_interrupted_ = std::move(callback);
}
void MacQtOverlayWindow::restore_input_focus(QWidget& host) {
  NSView* view = (__bridge NSView*)reinterpret_cast<void*>(host.winId());
  if (!host.isVisible() || impl_->system_dialog_active_ || !view.window) return;
  [NSApp activateIgnoringOtherApps:YES];
  [view.window makeKeyAndOrderFront:nil];
  host.activateWindow();
  host.setFocus(Qt::MouseFocusReason);
}
void* MacQtOverlayWindow::native_surface() const { return (__bridge void*)impl_->layer_; }
void MacQtOverlayWindow::resize(QWidget& host) {
  if (impl_->layer_ == nil) return;
  NSView* view = (__bridge NSView*)reinterpret_cast<void*>(host.winId());
  impl_->layer_.frame = view.bounds;
  impl_->layer_.drawableSize = [view convertRectToBacking:view.bounds].size;
}
void MacQtOverlayWindow::set_system_dialog_active(QWidget& host, bool active, bool restore_focus) {
  impl_->system_dialog_active_ = active;
  NSView* view = (__bridge NSView*)reinterpret_cast<void*>(host.winId());
  NSWindow* window = view.window;
  if (window == nil) return;
  if (active) {
    window.level = NSNormalWindowLevel;
  } else {
    window.level = NSScreenSaverWindowLevel;
    [window orderFrontRegardless];
    if (restore_focus) {
      [NSApp activateIgnoringOtherApps:YES];
      [window makeKeyAndOrderFront:nil];
      host.setFocus();
    }
  }
}
} // namespace hdrshot
