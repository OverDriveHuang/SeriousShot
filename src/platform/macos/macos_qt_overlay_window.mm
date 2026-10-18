#include "platform/macos/macos_qt_overlay_window.hpp"
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#include <iostream>
#include <QApplication>
#include <QPointer>
#include <QTimer>
#include <QVariant>
// A passive card must never become an input target or create a fullscreen Space.
@interface SeriousShotMagnifierWindow : NSWindow
@end
@implementation SeriousShotMagnifierWindow
- (BOOL)canBecomeKeyWindow { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }
@end
namespace hdrshot {
struct MacQtOverlayWindow::Impl : std::enable_shared_from_this<Impl> {
  NSScreen* native_screen_{nil};
  CAMetalLayer* layer_{nil};
  NSWindow* card_window_{nil};
  CAMetalLayer* card_layer_{nil};
  std::optional<Error> preparation_error_;
  QPointer<QWidget> host_;
  __weak NSWindow* watched_window_{nil};
  id inactive_observer_{nil};
  id key_observer_{nil};
  bool presented_{false}, system_dialog_active_{false};
  std::function<void()> input_interrupted_;

  ~Impl() {
    disconnect();
    [card_window_ orderOut:nil];
    [card_window_.parentWindow removeChildWindow:card_window_];
    [card_window_ close];
  }
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
    card_window_ = [[SeriousShotMagnifierWindow alloc]
        initWithContentRect:NSMakeRect(0,0,1,1) styleMask:NSWindowStyleMaskBorderless
        backing:NSBackingStoreBuffered defer:NO screen:native_screen_];
    card_window_.releasedWhenClosed = NO;
    card_window_.ignoresMouseEvents = YES;
    card_window_.hasShadow = NO;
    card_window_.opaque = YES;
    card_window_.hidesOnDeactivate = NO;
    card_window_.movable = NO;
    card_window_.level = NSScreenSaverWindowLevel;
    card_window_.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
        NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorFullScreenAuxiliary |
        NSWindowCollectionBehaviorIgnoresCycle;
    card_layer_ = [CAMetalLayer layer];
    card_layer_.maximumDrawableCount = 2;
    card_layer_.allowsNextDrawableTimeout = YES;
    card_layer_.actions = @{@"position":[NSNull null], @"bounds":[NSNull null],
        @"contents":[NSNull null], @"drawableSize":[NSNull null]};
    card_layer_.contentsScale = native_screen_.backingScaleFactor;
    card_layer_.drawableSize = CGSizeMake(1,1);
    card_window_.contentView.wantsLayer = YES;
    card_window_.contentView.layer = card_layer_;
    if (!layer_ || !card_layer_ || !card_window_)
      preparation_error_ = Error{ErrorCode::presenter_failed,"MacQtOverlayWindow",
          Retryability::after_recreate,{{"reason","native_surface_creation_failed"}}};
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
std::optional<Error> MacQtOverlayWindow::preparation_error() const { return impl_->preparation_error_; }
void* MacQtOverlayWindow::magnifier_surface() const { return (__bridge void*)impl_->card_layer_; }
void MacQtOverlayWindow::move_magnifier(QWidget& host, const QRect& rect) {
  NSView* view = (__bridge NSView*)reinterpret_cast<void*>(host.winId());
  // Qt rect is local top-left logical coordinates. Convert via the actual host
  // window, preserving negative origins and AppKit's bottom-left convention.
  const auto parent = view.window.frame;
  const NSRect frame = NSMakeRect(NSMinX(parent)+rect.x(),NSMaxY(parent)-rect.y()-rect.height(),
      rect.width(),rect.height());
  [CATransaction begin]; [CATransaction setDisableActions:YES];
  [impl_->card_window_ setFrame:frame display:NO animate:NO];
  impl_->card_layer_.frame = impl_->card_window_.contentView.bounds;
  impl_->card_layer_.contentsScale = host.devicePixelRatioF();
  impl_->card_layer_.drawableSize = CGSizeMake(qRound(rect.width()*host.devicePixelRatioF()),
      qRound(rect.height()*host.devicePixelRatioF()));
  [CATransaction commit];
}
void MacQtOverlayWindow::show_magnifier() {
  if (!impl_->host_ || !impl_->host_->isVisible() || impl_->system_dialog_active_) return;
  NSView* view = (__bridge NSView*)reinterpret_cast<void*>(impl_->host_->winId());
  if (impl_->card_window_.parentWindow != view.window)
    [view.window addChildWindow:impl_->card_window_ ordered:NSWindowAbove];
  [impl_->card_window_ orderFront:nil];
}
void MacQtOverlayWindow::hide_magnifier() {
  [impl_->card_window_ orderOut:nil];
  [impl_->card_window_.parentWindow removeChildWindow:impl_->card_window_];
}
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
