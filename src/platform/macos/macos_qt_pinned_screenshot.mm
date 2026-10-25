#include "platform/macos/macos_qt_pinned_screenshot.hpp"
#include "platform/macos/macos_analysis_backend.hpp"
#include "platform/macos/macos_metal_export_pixel_processor.hpp"
#include "ui/qt/pinned_window_style.hpp"
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <dispatch/dispatch.h>
#include <QApplication>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QWindow>
#include <iterator>

namespace {
NSCursor *pinned_cursor(Qt::CursorShape shape) {
  switch (shape) {
  case Qt::OpenHandCursor: return NSCursor.openHandCursor;
  case Qt::ClosedHandCursor: return NSCursor.closedHandCursor;
  case Qt::SizeHorCursor:
    return [NSCursor frameResizeCursorFromPosition:NSCursorFrameResizePositionLeft
        inDirections:NSCursorFrameResizeDirectionsAll];
  case Qt::SizeVerCursor:
    return [NSCursor frameResizeCursorFromPosition:NSCursorFrameResizePositionTop
        inDirections:NSCursorFrameResizeDirectionsAll];
  case Qt::SizeFDiagCursor:
    return [NSCursor frameResizeCursorFromPosition:NSCursorFrameResizePositionTopLeft
        inDirections:NSCursorFrameResizeDirectionsAll];
  case Qt::SizeBDiagCursor:
    return [NSCursor frameResizeCursorFromPosition:NSCursorFrameResizePositionTopRight
        inDirections:NSCursorFrameResizeDirectionsAll];
  case Qt::SizeAllCursor: {
    // AppKit has no four-way move cursor. Keep the shared SizeAll meaning,
    // without using Qt's private Cocoa cursor conversion or changing focus.
    static NSCursor *move = [] {
      NSImage *image = [NSImage imageWithSize:NSMakeSize(24,24) flipped:YES
          drawingHandler:^BOOL(NSRect) {
        NSBezierPath *path = [NSBezierPath bezierPath];
        const NSPoint points[] = {{12,2},{7,7},{10,7},{10,10},{7,10},{7,7},
            {2,12},{7,17},{7,14},{10,14},{10,17},{7,17},{12,22},
            {17,17},{14,17},{14,14},{17,14},{17,17},{22,12},
            {17,7},{17,10},{14,10},{14,7},{17,7}};
        [path moveToPoint:points[0]];
        for (std::size_t i=1;i<std::size(points);++i) [path lineToPoint:points[i]];
        [path closePath];
        [NSColor.whiteColor setStroke]; path.lineWidth=2; [path stroke];
        [NSColor.blackColor setFill]; [path fill];
        return YES;
      }];
      return [[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(12,12)];
    }();
    return move;
  }
  default: return NSCursor.arrowCursor;
  }
}
}

// Qt's Cocoa plugin intentionally drops tracking-area callbacks while NSApp is
// inactive. A local area bridges only passive hover for this particular window.
// Clicks, wheel/gestures, keyboard input and the active-app route stay with Qt.
@interface HDRShotPinnedHover : NSObject {
@public
  QPointer<QWidget> widget;
  QPointer<QWindow> entered;
  __weak NSView *view;
  NSTrackingArea *area;
}
- (void)attach:(NSView *)newView;
- (void)detach;
- (void)leave;
- (void)mouseMoved:(NSEvent *)event;
- (void)mouseEntered:(NSEvent *)event;
- (void)mouseExited:(NSEvent *)event;
- (void)cursorUpdate:(NSEvent *)event;
@end

namespace {
__weak HDRShotPinnedHover *current_pinned_hover;
}

@implementation HDRShotPinnedHover
- (void)attach:(NSView *)newView {
  if (view==newView) return;
  [self detach];
  view=newView;
  if (!view) return;
  area=[[NSTrackingArea alloc] initWithRect:NSZeroRect
      options:NSTrackingActiveAlways | NSTrackingMouseEnteredAndExited |
              NSTrackingMouseMoved | NSTrackingCursorUpdate | NSTrackingInVisibleRect
      owner:self userInfo:nil];
  [view addTrackingArea:area];
}
- (void)detach {
  if (current_pinned_hover==self) current_pinned_hover=nil;
  if (area) [view removeTrackingArea:area];
  area=nil; view=nil; entered=nullptr;
}
- (void)leave {
  if (current_pinned_hover!=self) { entered=nullptr; return; }
  current_pinned_hover=nil;
  const QPointer<QWindow> target=entered;
  entered=nullptr;
  if (target) {
    QEvent event(QEvent::Leave);
    QCoreApplication::sendEvent(target,&event);
  }
}
- (void)mouseMoved:(NSEvent *)event {
  if (NSApp.active || !widget || !widget->isVisible() || !view.window ||
      event.window!=view.window || NSEvent.pressedMouseButtons ||
      QWidget::mouseGrabber() || QApplication::activePopupWidget() ||
      QApplication::activeModalWidget()) return;
  const NSPoint point=[view convertPoint:event.locationInWindow fromView:nil];
  const QPointF local(point.x,view.isFlipped ? point.y : view.bounds.size.height-point.y);
  if (!widget->rect().contains(local.toPoint())) { [self leave]; return; }
  if (current_pinned_hover!=self) {
    [current_pinned_hover leave];
    current_pinned_hover=self;
  }
  const QPointF global=widget->mapToGlobal(local);
  // Enter can reveal the toolbar, changing the child under the same point.
  // Hit-test once more so its first hover has the right target immediately.
  for (int pass=0;pass<2;++pass) {
    QWidget *target=widget->childAt(local);
    if (!target) target=widget;
    QWidget *native=target;
    while (native && !native->windowHandle()) native=native->parentWidget();
    if (!native) return;
    QWindow *window=native->windowHandle();
    if (entered==window) break;
    [self leave];
    current_pinned_hover=self;
    entered=window;
    const QPointF position=native->mapFromGlobal(global);
    QEnterEvent enter(position,position,global);
    QCoreApplication::sendEvent(window,&enter);
    if (!widget || !widget->isVisible()) return;
  }
  if (!entered) return;
  const QPointF position=entered->mapFromGlobal(global);
  QMouseEvent move(QEvent::MouseMove,position,position,global,
                   Qt::NoButton,Qt::NoButton,Qt::NoModifier);
  move.setTimestamp(static_cast<ulong>(event.timestamp*1000));
  QCoreApplication::sendEvent(entered,&move);
  if (!widget || !widget->isVisible()) return;
  QWidget *target=widget->childAt(local);
  if (!target) target=widget;
  [pinned_cursor(target->cursor().shape()) set];
}
- (void)mouseEntered:(NSEvent *)event { [self mouseMoved:event]; }
- (void)cursorUpdate:(NSEvent *)event { [self mouseMoved:event]; }
- (void)mouseExited:(NSEvent *)event {
  if (!NSApp.active && event.window==view.window) [self leave];
}
@end

// Pure composition layers have no NSView input target. Keep the rings above
// native Metal/Qt child layers without a transparent QWidget backing-store copy.
@interface HDRShotPinnedBorderLayer : CALayer {
  CAShapeLayer *outer_;
  CAShapeLayer *inner_;
}
@property(nonatomic) CGFloat lineWidthPoints;
@end

@implementation HDRShotPinnedBorderLayer
- (instancetype)init {
  self=[super init];
  if(self) {
    self.name=@"SeriousShotPinnedBorder";
    self.zPosition=1;
    self.autoresizingMask=kCALayerWidthSizable | kCALayerHeightSizable;
    self.actions=@{@"bounds":NSNull.null,@"position":NSNull.null,@"sublayers":NSNull.null};
    outer_=[CAShapeLayer layer]; inner_=[CAShapeLayer layer];
    for(CAShapeLayer *ring in @[outer_,inner_]) {
      ring.fillRule=kCAFillRuleEvenOdd;
      ring.actions=@{@"path":NSNull.null,@"bounds":NSNull.null,@"position":NSNull.null};
      [self addSublayer:ring];
    }
    CGColorRef dark=CGColorCreateGenericRGB(0,0,0,hdrshot::pinned_window_style::outer_black_alpha);
    CGColorRef light=CGColorCreateGenericRGB(1,1,1,hdrshot::pinned_window_style::inner_white_alpha);
    outer_.fillColor=dark; inner_.fillColor=light;
    CGColorRelease(dark); CGColorRelease(light);
  }
  return self;
}
- (void)layoutSublayers {
  [super layoutSublayers];
  [CATransaction begin]; [CATransaction setDisableActions:YES];
  const CGFloat line=self.lineWidthPoints;
  const CGRect bounds=CGRectMake(0,0,self.bounds.size.width,self.bounds.size.height);
  int index=0;
  for(CAShapeLayer *ring in @[outer_,inner_]) {
    ring.frame=bounds;
    ring.contentsScale=self.contentsScale;
    const CGRect outside=CGRectInset(bounds,index*line,index*line);
    const CGRect inside=CGRectInset(outside,line,line);
    CGMutablePathRef path=CGPathCreateMutable();
    CGPathAddRect(path,nullptr,outside); CGPathAddRect(path,nullptr,inside);
    ring.path=path; CGPathRelease(path); ++index;
  }
  [CATransaction commit];
}
@end

namespace {
class MacPinnedDecoration final : public QObject {
public:
  explicit MacPinnedDecoration(QWidget &widget) : QObject(&widget), widget_(&widget) {
    setObjectName("macPinnedDecoration");
    border_=[HDRShotPinnedBorderLayer layer];
    widget.installEventFilter(this);
    synchronize();
  }
  ~MacPinnedDecoration() override { [border_ removeFromSuperlayer]; }
protected:
  bool eventFilter(QObject *object,QEvent *event) override {
    if(object==widget_ && (event->type()==QEvent::Resize || event->type()==QEvent::Show ||
        event->type()==QEvent::WinIdChange || event->type()==QEvent::DevicePixelRatioChange ||
        event->type()==QEvent::ScreenChangeInternal)) {
      // Coalesce geometry/handle changes after Qt has updated the native view.
      // This is a one-shot event delivery, never an idle/hover polling timer.
      if(!pending_) {
        pending_=true;
        QTimer::singleShot(0,this,[this]{pending_=false;synchronize();});
      }
    }
    return false;
  }
private:
  void synchronize() {
    if(!widget_ || !widget_->internalWinId()) { [border_ removeFromSuperlayer]; return; }
    NSView *view=(__bridge NSView *)(void *)widget_->internalWinId();
    NSWindow *window=view.window;
    if(!window) return;
    view.wantsLayer=YES;
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    if(border_.superlayer!=view.layer) {
      [border_ removeFromSuperlayer]; [view.layer addSublayer:border_];
    }
    const CGFloat backing=std::max(CGFloat(1),window.backingScaleFactor);
    border_.contentsScale=backing;
    border_.lineWidthPoints=hdrshot::pinned_window_style::border_line_physical_pixels(
        widget_->devicePixelRatioF())/backing;
    border_.frame=view.layer.bounds;
    [border_ setNeedsLayout]; [border_ layoutIfNeeded];
    [CATransaction commit];
    // AppKit follows the rectangular NSWindow frame itself. Invalidating here
    // can sample the previous frame before WindowServer consumes Qt's resize,
    // retaining the collapsed shadow bounds after expansion.
    if(!window.hasShadow) window.hasShadow=YES;
  }
  QPointer<QWidget> widget_;
  HDRShotPinnedBorderLayer *border_;
  bool pending_{};
};

class MacPinnedHover final : public QObject {
public:
  explicit MacPinnedHover(QWidget &widget) : QObject(&widget), widget_(&widget) {
    setObjectName("macPinnedHover");
    tracker_=[HDRShotPinnedHover new]; tracker_->widget=&widget;
    attach();
    widget.installEventFilter(this);
    connect(qApp,&QGuiApplication::applicationStateChanged,this,[this] {
      tracker_->entered=nullptr; // Qt owns the active-app transition.
    });
  }
  ~MacPinnedHover() override { [tracker_ detach]; }
protected:
  bool eventFilter(QObject *object,QEvent *event) override {
    if (object==widget_) {
      if (event->type()==QEvent::Hide) [tracker_ leave];
      if (event->type()==QEvent::WinIdChange || event->type()==QEvent::Show) attach();
    }
    return false;
  }
private:
  void attach() {
    if (widget_ && widget_->internalWinId())
      [tracker_ attach:(__bridge NSView *)(void *)widget_->internalWinId()];
  }
  QPointer<QWidget> widget_;
  HDRShotPinnedHover *tracker_;
};
}

namespace hdrshot {
namespace {
class MacPinnedWindow final : public QtPinnedWindowPort {
  static NSWindow *window(QWidget &widget) {
    return ((__bridge NSView *)(void *)widget.winId()).window;
  }
public:
  void set_always_on_top(QWidget &widget, bool topmost) override {
    NSWindow *native=window(widget);
    if (!widget.findChild<QObject *>("macPinnedDecoration")) new MacPinnedDecoration(widget);
    if (!widget.findChild<QObject *>("macPinnedHover")) new MacPinnedHover(widget);
    native.level=topmost ? NSFloatingWindowLevel : NSNormalWindowLevel;
    native.collectionBehavior=NSWindowCollectionBehaviorManaged |
                              NSWindowCollectionBehaviorFullScreenAuxiliary;
  }
  void bring_forward(QWidget &widget) override { [window(widget) orderFront:nil]; }
  void configure_preview(QWidget &widget,QWidget &owner) override {
    NSWindow *native=window(widget);
    native.level=window(owner).level;
    native.hasShadow=YES;
    // Qt assigns Tool windows AppKit's utility fade. Its asynchronous orderOut
    // snapshot can clip the owner's expansion to the collapsed toolbar height.
    // The shared preview already owns its entrance animation; dismissal is immediate.
    native.animationBehavior=NSWindowAnimationBehaviorNone;
    native.ignoresMouseEvents=YES;
    native.hidesOnDeactivate=NO;
    [window(owner) addChildWindow:native ordered:NSWindowAbove];
  }
};
}
std::shared_ptr<QtPinnedWindowPort> make_macos_pinned_window_port() {
  return std::make_shared<MacPinnedWindow>();
}
Result<PreparedPinnedScreenshot,Error> prepare_macos_pinned_screenshot(const ExportSnapshot &snapshot) {
  auto backend=make_macos_analysis_backend();
  if(!backend) return Result<PreparedPinnedScreenshot,Error>::failure(backend.error());
  auto range=MacMetalExportPixelProcessor::create();
  if(!range) return Result<PreparedPinnedScreenshot,Error>::failure(range.error());
  auto image=AnalysisWorkflow::prepare(snapshot,*backend.value().port,range.value().get());
  if(!image) return Result<PreparedPinnedScreenshot,Error>::failure(image.error());
  PreparedPinnedScreenshot result;
  for(auto &aux:result.auxiliary) {
    auto extra=make_macos_analysis_backend();
    if(!extra)return Result<PreparedPinnedScreenshot,Error>::failure(extra.error());
    aux.presenter=std::move(extra.value().presenter);
    if(&aux==&result.auxiliary[1])aux.attach=[](QWidget &,QWidget &surface,
        std::function<void()>,std::function<void(Error)>) {
      NSView *view=(__bridge NSView *)(void *)surface.winId();
      view.wantsLayer=YES;view.layer.cornerRadius=4.;view.layer.masksToBounds=YES;
      return Result<bool,Error>::success(true);
    };
    const auto peer=aux.presenter;
    aux.observe_errors=[peer](std::function<void(Error)> callback) {
      set_macos_analysis_presenter_error_callback(*peer,[callback=std::move(callback)](Error error) {
        dispatch_async(dispatch_get_main_queue(), ^{ callback(error); });
      });
    };
  }
  result.image=std::move(image.value());
  result.presenter=std::move(backend.value().presenter);
  const auto presenter=result.presenter;
  result.observe_errors=[presenter](std::function<void(Error)> callback) {
    set_macos_analysis_presenter_error_callback(*presenter,
        [callback=std::move(callback)](Error error) {
          dispatch_async(dispatch_get_main_queue(), ^{ callback(error); });
        });
  };
  result.observe_recovered=[presenter](std::function<void()> callback) {
    const std::weak_ptr<AnalysisPresenterPort> weak=presenter;
    set_macos_analysis_presenter_recovery_callback(*presenter,
        [weak,callback=std::move(callback)](std::uint64_t generation) {
          dispatch_async(dispatch_get_main_queue(), ^{
            const auto current=weak.lock();
            if(current && acknowledge_macos_analysis_presenter_recovery(*current,generation))
              callback();
          });
        });
  };
  return Result<PreparedPinnedScreenshot,Error>::success(std::move(result));
}
}
