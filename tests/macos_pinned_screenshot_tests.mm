#include "platform/macos/macos_qt_pinned_screenshot.hpp"
#include "platform/macos/macos_analysis_backend.hpp"
#include "platform/macos/macos_metal_export_pixel_processor.hpp"
#include "application/clean_content_cache.hpp"
#include "ui/qt/analyzer_source_view.hpp"
#include "ui/qt/pinned_screenshot_preview.hpp"
#include "test_support.hpp"
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QToolButton>
#include <QProcess>
#include <QImage>
#include <QDir>
#include <QPointer>
#include <QPalette>
#include <algorithm>
#include <memory>
#include <vector>
#include <iostream>
using namespace hdrshot;
namespace {
bool spin(std::function<bool()> predicate,int timeout=6000) {
  QElapsedTimer time;time.start();
  while(!predicate() && time.elapsed()<timeout) {
    QApplication::processEvents();QThread::msleep(3);
  }
  return predicate();
}
ExportSnapshot fixture(bool hdr = true) {
  auto desktop = std::make_shared<FrozenDesktop>();
  desktop->frame_id = {44};
  desktop->display_generation = 1;
  CanonicalFrameSegment s;
  s.display_id = {1};
  s.size_px = {256, 160};
  s.point_pixel_scale = 1;
  s.desktop_frame_points = {0, 0, 256, 160};
  desktop->desktop_bounds_points = s.desktop_frame_points;
  s.encoding = {ColorPrimaries::display_p3, TransferFunction::linear,
                AlphaMode::opaque, 0};
  s.pixel_format = PixelFormat::rgba32_float;
  s.display_dynamic_range = DisplayDynamicRange::hdr;
  s.rgba_float.resize(256U * 160U * 4U);
  for (std::size_t p = 0; p < 256U * 160U; ++p) {
    const float x = float(p % 256) / 255;
    s.rgba_float[p * 4] = .02F + x * (hdr ? 5.98F : .98F);
    s.rgba_float[p * 4 + 1] = .03F + x * (hdr ? 2.97F : .97F);
    s.rgba_float[p * 4 + 2] = .04F + x * (hdr ? 3.96F : .96F);
    s.rgba_float[p * 4 + 3] = 1;
  }
  desktop->canonical_segments.push_back(std::move(s));
  ExportSnapshot snapshot;
  snapshot.session_id = {22};
  snapshot.operation_id = {1};
  snapshot.frozen_desktop = desktop;
  snapshot.target_display_id = {1};
  snapshot.selection = {1, {16, 8, 224, 144}};
  snapshot.annotations.revision = 1;
  snapshot.annotation_render_plan = std::make_shared<AnnotationRenderPlan>(
      AnnotationRenderPlan{1, {224, 144}, {}, snapshot.selection.desktop_rect});
  return snapshot;
}

// Inject at the registered AppKit tracking-area boundary, including Qt's own
// owner. No QWidget event injection, WA_UnderMouse mutation or real pointer warp
// is used: this exercises inactive native callback routing, not OS mouse input.
NSEvent *tracking_event(NSView *root, QPoint local, NSEventType type) {
  const auto bounds=root.bounds;
  const NSPoint point=NSMakePoint(NSMinX(bounds)+local.x(),
      root.flipped ? NSMinY(bounds)+local.y() : NSMaxY(bounds)-local.y());
  const NSPoint location=[root convertPoint:point toView:nil];
  if(type==NSEventTypeMouseMoved)
    return [NSEvent mouseEventWithType:type location:location modifierFlags:0
        timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:root.window.windowNumber
        context:nil eventNumber:0 clickCount:0 pressure:0];
  return [NSEvent enterExitEventWithType:type location:location modifierFlags:0
      timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:root.window.windowNumber
      context:nil eventNumber:0 trackingNumber:0 userData:nullptr];
}
void tracking_callback(id owner, NSEvent *event) {
  switch(event.type) {
  case NSEventTypeMouseMoved:
    if([owner respondsToSelector:@selector(mouseMoved:)]) [owner mouseMoved:event];
    break;
  case NSEventTypeMouseEntered:
    if([owner respondsToSelector:@selector(mouseEntered:)]) [owner mouseEntered:event];
    break;
  case NSEventTypeMouseExited:
    if([owner respondsToSelector:@selector(mouseExited:)]) [owner mouseExited:event];
    break;
  case NSEventTypeCursorUpdate:
    if([owner respondsToSelector:@selector(cursorUpdate:)]) [owner cursorUpdate:event];
    break;
  default: HDRSHOT_CHECK(false);
  }
}
void track(NSView *root, QPoint local, NSEventType type) {
  NSEvent *event=tracking_event(root,local,type);
  HDRSHOT_CHECK(event && root.trackingAreas.count>0);
  // Snapshot the actual registrations because a callback may update tracking.
  for(NSTrackingArea *area in [root.trackingAreas copy]) tracking_callback(area.owner,event);
  QApplication::processEvents();
}
void inactive_hover() {
  auto prepared=prepare_macos_pinned_screenshot(fixture());
  HDRSHOT_CHECK(prepared.has_value());
  auto &data=prepared.value();
  auto native=make_macos_pinned_window_port();
  auto window=std::make_unique<PinnedScreenshotWindow>(data.image.input,PinnedScreenshotDefaults{});
  window->resize(533,400);
  int actions=0,fronts=0,topmost_changes=0;
  window->action_requested=[&](UiCommand){++actions;};
  window->front_requested=[&]{++fronts;};
  window->topmost_changed=[&](bool){++topmost_changes;};
  window->present_requested=[&](auto view,auto surface) {
    HDRSHOT_CHECK(data.presenter->present(data.image.input,view,surface).has_value());
  };
  window->show();
  auto *root=(__bridge NSView *)(void *)window->winId();
  HDRSHOT_CHECK(spin([&]{return macos_analysis_presentation_status(*data.presenter).completed>0;}));
  auto *surface=window->source_view()->presentation_surface();
  HDRSHOT_CHECK(surface->testAttribute(Qt::WA_NativeWindow));
  auto *surface_ns=(__bridge NSView *)(void *)surface->winId();
  HDRSHOT_CHECK(surface_ns!=root && surface_ns.window==root.window);
  NSArray<NSTrackingArea *> *qt_areas=[root.trackingAreas copy];
  native->set_always_on_top(*window,true);
  const auto registered_count=root.trackingAreas.count;
  native->set_always_on_top(*window,true);
  HDRSHOT_CHECK(root.trackingAreas.count==registered_count);

  // Preserve the pan-hover regression under the new conditional window-drag
  // contract: only an image larger than the viewport uses the hand cursor.
  window->source_view()->zoom_at(QPointF(window->width()/2.,window->height()/2.),2.);

  auto *toolbar=window->findChild<QWidget *>("pinnedToolbar");
  auto *grip=window->findChild<QWidget *>("pinWindowGrip");
  auto *copy=window->findChild<QToolButton *>("pinCopy");
  auto *topmost=window->findChild<QToolButton *>("pinTopmost");
  auto *close=window->findChild<QToolButton *>("pinClose");
  HDRSHOT_CHECK(toolbar && grip && copy && topmost && close && grip->width()>0);
  // Deactivate only this test process; never activate or operate another app.
  if(NSApp.active) [NSApp deactivate];
  HDRSHOT_CHECK(spin([]{return !NSApp.active;}));
  NSWindow *key_before=NSApp.keyWindow;
  const auto geometry_before=window->geometry();
  const auto transform_before=window->source_view()->transform();
  HDRSHOT_CHECK(spin([&]{return !toolbar->isVisible();},1500));
  const QPoint image_point(window->width()/2,window->height()/2);
  track(root,image_point,NSEventTypeMouseEntered);
  track(root,image_point,NSEventTypeMouseMoved);
  track(root,image_point,NSEventTypeCursorUpdate);
  // The unpatched Qt 6.11.1 owners ignore inactive callbacks: this first
  // observable requirement must fail against the old adapter.
  HDRSHOT_CHECK(toolbar->isVisible());
  HDRSHOT_CHECK(surface->underMouse());
  HDRSHOT_CHECK([NSCursor.currentCursor isEqual:NSCursor.openHandCursor]);
  auto hover=[&](QWidget *target) {
    track(root,target->mapTo(window.get(),target->rect().center()),NSEventTypeMouseMoved);
    track(root,target->mapTo(window.get(),target->rect().center()),NSEventTypeCursorUpdate);
    HDRSHOT_CHECK(target->underMouse());
    HDRSHOT_CHECK(!NSApp.active && NSApp.keyWindow==key_before);
  };
  const auto background=[](QToolButton *button) {
    const auto *parent=button->parentWidget();
    const auto point=button->mapTo(parent,QPoint(3,button->height()/2));
    const auto image=button->parentWidget()->grab().toImage();
    return image.pixelColor(qRound(point.x()*image.devicePixelRatio()),
                            qRound(point.y()*image.devicePixelRatio()));
  };
  const auto copy_normal=background(copy);
  hover(grip);
  HDRSHOT_CHECK(!surface->underMouse());
  HDRSHOT_CHECK(![NSCursor.currentCursor isEqual:NSCursor.openHandCursor]);
  HDRSHOT_CHECK(![NSCursor.currentCursor isEqual:NSCursor.arrowCursor]);
  NSCursor *move_cursor=NSCursor.currentCursor;
  for(auto *button:{copy,topmost,close}) {
    hover(button);
    if(button==copy) HDRSHOT_CHECK(background(copy)!=copy_normal);
    HDRSHOT_CHECK([NSCursor.currentCursor isEqual:NSCursor.arrowCursor]);
    HDRSHOT_CHECK(!grip->underMouse());
    for(auto *other:{copy,topmost,close}) HDRSHOT_CHECK(other->underMouse()==(other==button));
  }
  track(root,image_point,NSEventTypeMouseMoved);
  track(root,image_point,NSEventTypeCursorUpdate);
  HDRSHOT_CHECK(surface->underMouse() && !close->underMouse());
  HDRSHOT_CHECK([NSCursor.currentCursor isEqual:NSCursor.openHandCursor]);
  HDRSHOT_CHECK(background(copy)==copy_normal);
  HDRSHOT_CHECK(actions==0 && fronts==0 && topmost_changes==0);
  HDRSHOT_CHECK(window->geometry()==geometry_before);
  HDRSHOT_CHECK(window->source_view()->transform().pan==transform_before.pan);

  // A fully visible image uses the same native move cursor as the toolbar
  // grip. These are native callback injections, not proof of inactive OS hover.
  window->source_view()->fit();
  track(root,image_point,NSEventTypeMouseMoved);
  track(root,image_point,NSEventTypeCursorUpdate);
  HDRSHOT_CHECK(surface->cursor().shape()==Qt::SizeAllCursor);
  HDRSHOT_CHECK([NSCursor.currentCursor isEqual:move_cursor]);
  HDRSHOT_CHECK(!NSApp.active && NSApp.keyWindow==key_before);
  const auto before_restore=macos_analysis_presentation_status(*data.presenter).requested;
  window->source_view()->set_transform(transform_before);
  track(root,image_point,NSEventTypeMouseMoved);
  track(root,image_point,NSEventTypeCursorUpdate);
  HDRSHOT_CHECK([NSCursor.currentCursor isEqual:NSCursor.openHandCursor]);

  // Drain the restore request through the shared Qt scheduler as well as the
  // native worker, before measuring hover-only presentation requests.
  HDRSHOT_CHECK(spin([&]{auto s=macos_analysis_presentation_status(*data.presenter);
    return s.requested>before_restore&&!s.worker_active&&!s.pending;}));
  // Bound the actual local bridge + Qt dispatch cost under a burst. This is
  // not a hardware mouse latency or WindowServer frame-time measurement.
  const auto presents_before=macos_analysis_presentation_status(*data.presenter).requested;
  const QPoint grip_point=grip->mapTo(window.get(),grip->rect().center());
  const QPoint copy_point=copy->mapTo(window.get(),copy->rect().center());
  constexpr int sample_count=2400;
  std::vector<double> micros;micros.reserve(sample_count);
  QElapsedTimer total;total.start();
  for(int i=0;i<sample_count;++i) {
    // Long stays within a region plus crossings, without triggering actions.
    const int region=(i/40)%3;
    const QPoint point=region==0 ? image_point+QPoint(i%20,0) :
        region==1 ? grip_point : copy_point;
    QElapsedTimer timer;timer.start();
    track(root,point,NSEventTypeMouseMoved);
    micros.push_back(static_cast<double>(timer.nsecsElapsed())/1000.);
  }
  const double total_ms=static_cast<double>(total.nsecsElapsed())/1000000.;
  std::sort(micros.begin(),micros.end());
  HDRSHOT_CHECK(spin([&]{auto s=macos_analysis_presentation_status(*data.presenter);
    return !s.worker_active&&!s.pending;}));
  const auto presents_after=macos_analysis_presentation_status(*data.presenter).requested;
  std::cout<<"inactive hover burst events="<<sample_count<<" total_ms="<<total_ms
      <<" p50_us="<<micros[sample_count/2]<<" p95_us="<<micros[sample_count*95/100]
      <<" source_present_delta="<<(presents_after-presents_before)<<'\n';
  HDRSHOT_CHECK(presents_after==presents_before);
  HDRSHOT_CHECK(actions==0 && fronts==0 && topmost_changes==0);
  HDRSHOT_CHECK(!NSApp.active && NSApp.keyWindow==key_before);
  track(root,image_point,NSEventTypeMouseMoved);

  track(root,QPoint(-20,-20),NSEventTypeMouseExited);
  HDRSHOT_CHECK(!surface->underMouse() && toolbar->isVisible());
  HDRSHOT_CHECK(spin([&]{return !toolbar->isVisible();},1500));
  window->resize(600,450);QApplication::processEvents();
  const QPoint resized_image(550,350);
  track(root,resized_image,NSEventTypeMouseEntered);
  track(root,resized_image,NSEventTypeMouseMoved);
  HDRSHOT_CHECK(toolbar->isVisible() && surface->underMouse());

  // A second real native surface must keep its own hover/toolbar state.
  PinnedScreenshotWindow other(data.image.input,{});
  other.resize(400,400);other.show();
  (void)other.source_view()->presentation_surface()->winId();
  native->set_always_on_top(other,true);
  auto *other_root=(__bridge NSView *)(void *)other.winId();
  auto *other_toolbar=other.findChild<QWidget *>("pinnedToolbar");
  HDRSHOT_CHECK(spin([&]{return !other_toolbar->isVisible();},1500));
  const auto first_order=[NSApp.orderedWindows indexOfObject:root.window];
  const auto second_order=[NSApp.orderedWindows indexOfObject:other_root.window];
  track(other_root,QPoint(200,200),NSEventTypeMouseEntered);
  track(other_root,QPoint(200,200),NSEventTypeMouseMoved);
  HDRSHOT_CHECK(other_toolbar->isVisible());
  HDRSHOT_CHECK(!surface->underMouse());
  // AppKit does not guarantee cross-area enter/exit ordering. A late exit
  // from the first pin must not clear the second pin's current hover.
  track(root,QPoint(-20,-20),NSEventTypeMouseExited);
  HDRSHOT_CHECK(other.source_view()->presentation_surface()->underMouse());
  HDRSHOT_CHECK([NSApp.orderedWindows indexOfObject:root.window]==first_order);
  HDRSHOT_CHECK([NSApp.orderedWindows indexOfObject:other_root.window]==second_order);
  track(other_root,QPoint(-20,-20),NSEventTypeMouseExited);
  HDRSHOT_CHECK(spin([&]{return !other_toolbar->isVisible();},1500));
  HDRSHOT_CHECK(!toolbar->isVisible() && !NSApp.active && NSApp.keyWindow==key_before);
  other.close();

  // Retain only newly registered bridge owners, never Qt's helper whose raw
  // QNSView lifetime is Qt-owned. A queued bridge callback after destruction
  // must be harmless even when the original widget/native view is gone.
  NSMutableArray *late_owners=[NSMutableArray array];
  for(NSTrackingArea *area in root.trackingAreas) {
    bool qt_owner=false;
    for(NSTrackingArea *original in qt_areas)
      if([original.owner class]==[area.owner class]) qt_owner=true;
    if(!qt_owner && area.owner) [late_owners addObject:area.owner];
  }
  HDRSHOT_CHECK(late_owners.count>0);
  NSEvent *late_event=tracking_event(root,image_point,NSEventTypeMouseMoved);
  window->hide();
  for(id owner in late_owners) tracking_callback(owner,late_event);
  HDRSHOT_CHECK(!toolbar->isVisible());
  QPointer<PinnedScreenshotWindow> guard(window.get());
  window->close();window.reset();
  HDRSHOT_CHECK(!guard);
  for(id owner in late_owners) tracking_callback(owner,late_event);
  QApplication::processEvents();
  HDRSHOT_CHECK(actions==0 && fronts==0 && topmost_changes==0);
  HDRSHOT_CHECK(!NSApp.active && NSApp.keyWindow==key_before);
  HDRSHOT_CHECK(spin([&]{auto s=macos_analysis_presentation_status(*data.presenter);return !s.worker_active&&!s.pending;}));
  std::cout<<"inactive native tracking-owner callback injection passed (not real OS pointer input)\n";
}

CALayer *window_border(NSView *root) {
  for(CALayer *layer in root.layer.sublayers)
    if([layer.name isEqualToString:@"SeriousShotPinnedBorder"]) return layer;
  return nil;
}

void boundary_native_roundtrip() {
  auto snapshot=fixture(false);
  auto desktop=std::make_shared<FrozenDesktop>(*snapshot.frozen_desktop);
  for(auto &segment:desktop->canonical_segments)
    for(std::size_t i=0;i<segment.rgba_float.size();i+=4)
      for(std::size_t c=0;c<3;++c) segment.rgba_float[i+c]=.18F;
  snapshot.frozen_desktop=desktop;
  auto prepared=prepare_macos_pinned_screenshot(snapshot);
  HDRSHOT_CHECK(prepared.has_value());
  auto &data=prepared.value();
  auto native=make_macos_pinned_window_port();
  auto window=std::make_unique<PinnedScreenshotWindow>(data.image.input,PinnedScreenshotDefaults{});
  window->resize(500,322);
  int submissions=0;
  window->present_requested=[&](auto view,auto surface){
    ++submissions;
    HDRSHOT_CHECK(data.presenter->present(data.image.input,view,surface).has_value());
  };
  window->topmost_changed=[&](bool top){native->set_always_on_top(*window,top);};
  window->show();
  HDRSHOT_CHECK(spin([&]{auto s=macos_analysis_presentation_status(*data.presenter);return s.completed>0&&!s.worker_active&&!s.pending;}));
  NSView *root=(__bridge NSView *)(void *)window->winId();
  auto *source=window->source_view();
  auto *surface=source->presentation_surface();
  const auto source_geometry=source->geometry();
  const auto image=source->image_rect();
  const auto pixels=data.image.input.source->read_region({0,0,224,144});
  HDRSHOT_CHECK(pixels.has_value());
  const NSPoint hit=NSMakePoint(NSMidX(root.frame),NSMidY(root.frame));
  NSView *input_target=[root hitTest:hit];
  native->set_always_on_top(*window,true);
  HDRSHOT_CHECK(spin([&]{return window_border(root)!=nil;}));
  CALayer *border=window_border(root);
  HDRSHOT_CHECK(root.window.hasShadow);
  HDRSHOT_CHECK(CGRectEqualToRect(border.frame,root.layer.bounds));
  HDRSHOT_CHECK(border.zPosition>0);
  HDRSHOT_CHECK([root hitTest:hit]==input_target);
  HDRSHOT_CHECK(source->geometry()==source_geometry&&source->image_rect()==image);
  for(int i=0;i<5;++i) native->set_always_on_top(*window,i%2);
  // Window-level changes may legitimately expose Source. Check idle after
  // those native events settle, not during the existing Expose refresh path.
  spin([]{return false;},120);
  HDRSHOT_CHECK(spin([&]{auto s=macos_analysis_presentation_status(*data.presenter);return !s.worker_active&&!s.pending;}));
  const int before=submissions;
  spin([]{return false;},120);
  HDRSHOT_CHECK(window_border(root)==border&&submissions==before);
  HDRSHOT_CHECK(border.animationKeys.count==0);
  const auto folder=qEnvironmentVariable("HDRSHOT_PIN_BOUNDARY_CAPTURE");
  auto capture=[&](const QString &name,bool check_pixels) {
    if(folder.isEmpty()) return;
    QDir().mkpath(folder);
    native->bring_forward(*window);
    spin([]{return false;},120);
    const QString path=folder+"/"+name+".png";
    QProcess process;
    process.start("/usr/sbin/screencapture",{"-x","-o","-l",QString::number(root.window.windowNumber),path});
    const bool finished=process.waitForFinished(5000);
    if(!finished||process.exitCode()!=0) std::cerr<<process.readAllStandardError().toStdString();
    HDRSHOT_CHECK(finished&&process.exitCode()==0);
    QImage captured(path);HDRSHOT_CHECK(!captured.isNull());
    if(check_pixels) {
      const double dpr=double(captured.width())/window->width();
      const int line=std::max(1,int(std::lround(.5*dpr)));
      const int y=captured.height()/2;
      for(bool right:{false,true}) {
        auto brightness=[&](int offset){return captured.pixelColor(right?captured.width()-1-offset:offset,y).lightness();};
        const int outer=brightness(line/2),inner=brightness(line+line/2),body=brightness(line*3+4);
        std::cout<<name.toStdString()<<" edge="<<(right?"right":"left")<<" outer/inner/body="<<outer<<'/'<<inner<<'/'<<body<<'\n';
        HDRSHOT_CHECK(outer<body-8&&inner>body+8);
      }
    }
    std::cout<<"WindowServer boundary capture "<<path.toStdString()<<'\n';
  };
  for(int width:{500,450}) {
    window->resize(width,322);QApplication::processEvents();
    HDRSHOT_CHECK(spin([&]{return CGRectEqualToRect(border.frame,root.layer.bounds);}));
    HDRSHOT_CHECK(source==window->source_view()&&surface==source->presentation_surface());
    for(bool dark:{false,true}) {
      auto palette=window->palette();
      palette.setColor(QPalette::Window,dark?QColor("#26292E"):QColor("#F5F6F8"));
      window->setPalette(palette);
      const QString name=QString::number(width)+(dark?"-dark":"-light");
      QEvent enter(QEvent::Enter);QApplication::sendEvent(window.get(),&enter);
      capture(name+"-toolbar",true);
      QEvent leave(QEvent::Leave);QApplication::sendEvent(window.get(),&leave);
      HDRSHOT_CHECK(spin([&]{return !window->findChild<QWidget *>("pinnedToolbar")->isVisible();},1200));
      HDRSHOT_CHECK(!border.hidden);
      capture(name+"-hidden",true);
    }
  }
  const QSize expanded=window->size();
  auto *collapse=window->findChild<QToolButton *>("pinCollapse");
  collapse->click();QApplication::processEvents();
  HDRSHOT_CHECK(spin([&]{return CGRectEqualToRect(border.frame,root.layer.bounds);}));
  HDRSHOT_CHECK(window->height()==60&&root.window.hasShadow);
  capture("collapsed",false);
  collapse->click();QApplication::processEvents();
  HDRSHOT_CHECK(spin([&]{return CGRectEqualToRect(border.frame,root.layer.bounds);}));
  HDRSHOT_CHECK(window->size()==expanded);
  window->hide();window->show();QApplication::processEvents();
  HDRSHOT_CHECK(window_border(root)==border&&root.window.hasShadow);
  HDRSHOT_CHECK(spin([&]{auto state=macos_analysis_presentation_status(*data.presenter);return state.completed_generation==state.requested&&!state.worker_active&&!state.pending;}));
  if(!folder.isEmpty()) {
    // Include the real WindowServer shadow. This does not call render/grab.
    const QString path=folder+"/native-shadow.png";
    native->bring_forward(*window);spin([]{return false;},120);
    QProcess process;process.start("/usr/sbin/screencapture",{"-x","-l",QString::number(root.window.windowNumber),path});
    const bool finished=process.waitForFinished(5000);
    if(!finished||process.exitCode()!=0) std::cerr<<process.readAllStandardError().toStdString();
    HDRSHOT_CHECK(finished&&process.exitCode()==0);
    QImage captured(path);HDRSHOT_CHECK(!captured.isNull());
    std::cout<<"shadow capture="<<captured.width()<<'x'<<captured.height()
             <<" widget="<<window->width()<<'x'<<window->height()
             <<" dpr="<<window->devicePixelRatioF()
             <<" nativeFrame="<<NSStringFromRect(root.window.frame).UTF8String
             <<" view="<<NSStringFromRect(root.bounds).UTF8String
             <<" opaque="<<root.window.opaque<<" id="<<root.window.windowNumber<<std::endl;
    HDRSHOT_CHECK(captured.width()>window->width()*window->devicePixelRatioF());
    HDRSHOT_CHECK(captured.height()>window->height()*window->devicePixelRatioF());
    HDRSHOT_CHECK(captured.hasAlphaChannel());
    HDRSHOT_CHECK(captured.pixelColor(captured.width()/2,captured.height()/2).alpha()>0);
  }
  window->setWindowFlag(Qt::WindowStaysOnTopHint,true);window->show();
  spin([]{return false;},120);
  root=(__bridge NSView *)(void *)window->winId();
  HDRSHOT_CHECK(spin([&]{return window_border(root)==border&&CGRectEqualToRect(border.frame,root.layer.bounds);}));
  HDRSHOT_CHECK(root.window.hasShadow);
  std::cout<<"recreated widget="<<window->width()<<'x'<<window->height()<<" native="<<NSStringFromRect(root.window.frame).UTF8String<<" view="<<NSStringFromRect(root.bounds).UTF8String<<std::endl;
  HDRSHOT_CHECK(data.image.input.source->read_region({0,0,224,144}).value()==pixels.value());
  window->close();window.reset();QApplication::processEvents();
  HDRSHOT_CHECK(border.superlayer==nil);
  HDRSHOT_CHECK(spin([&]{auto s=macos_analysis_presentation_status(*data.presenter);return !s.worker_active&&!s.pending;}));
  std::cout<<"native boundary: shadow, native composition layer, input passthrough, resize/collapse, export source and disposal passed\n";
}

void collapse_native_roundtrip() {
  auto prepared=prepare_macos_pinned_screenshot(fixture());
  HDRSHOT_CHECK(prepared.has_value());
  auto &data=prepared.value();
  auto native=make_macos_pinned_window_port();
  auto window=std::make_unique<PinnedScreenshotWindow>(data.image.input,PinnedScreenshotDefaults{});
  window->resize(500,360);
  int submissions=0;
  window->present_requested=[&](auto view,auto surface) {
    ++submissions;
    HDRSHOT_CHECK(data.presenter->present(data.image.input,view,surface).has_value());
  };
  window->topmost_changed=[&](bool value){native->set_always_on_top(*window,value);};
  window->show();native->set_always_on_top(*window,true);
  HDRSHOT_CHECK(spin([&]{auto s=macos_analysis_presentation_status(*data.presenter);return s.completed>0&&!s.worker_active&&!s.pending;}));
  auto *collapse=window->findChild<QToolButton *>("pinCollapse");
  HDRSHOT_CHECK(collapse);
  auto *source=window->source_view();
  auto *surface=source->presentation_surface();
  auto *surface_ns=(__bridge NSView *)(void *)surface->winId();
  auto *root=(__bridge NSView *)(void *)window->winId();
  for(bool fit:{true,false}) {
    source->fit();
    if(!fit) source->zoom_at(QPointF(113,151),3.);
    HDRSHOT_CHECK(spin([&]{auto s=macos_analysis_presentation_status(*data.presenter);return s.completed_generation==s.requested&&!s.worker_active&&!s.pending;}));
    const auto size_before=window->size();
    const auto source_size=source->size();
    const auto image_before=source->image_rect();
    const auto transform=source->transform();
    collapse->click();
    QApplication::processEvents();
    HDRSHOT_CHECK(!source->isVisible()&&!surface->isVisible());
    HDRSHOT_CHECK(surface_ns.hiddenOrHasHiddenAncestor);
    HDRSHOT_CHECK(window->width()==size_before.width()&&window->height()<100);
    HDRSHOT_CHECK(source->size()==source_size);
    HDRSHOT_CHECK(root.window.level==NSFloatingWindowLevel);
    HDRSHOT_CHECK(spin([&]{auto s=macos_analysis_presentation_status(*data.presenter);return !s.worker_active&&!s.pending;}));
    const auto settled=macos_analysis_presentation_status(*data.presenter);
    const int submitted_before=submissions;
    for(int i=0;i<8;++i) window->refresh_presentation();
    spin([]{return false;},120);
    HDRSHOT_CHECK(submissions==submitted_before);
    HDRSHOT_CHECK(macos_analysis_presentation_status(*data.presenter).requested==settled.requested);
    window->move(window->pos()+QPoint(18,12));
    const auto moved=window->pos();
    collapse->click();
    HDRSHOT_CHECK(spin([&]{auto s=macos_analysis_presentation_status(*data.presenter);return s.completed>settled.completed&&!s.worker_active&&!s.pending;}));
    HDRSHOT_CHECK(source==window->source_view()&&surface==source->presentation_surface());
    HDRSHOT_CHECK(source->isVisible()&&surface->isVisible()&&!surface_ns.hiddenOrHasHiddenAncestor);
    HDRSHOT_CHECK(window->size()==size_before&&window->pos()==moved);
    HDRSHOT_CHECK(source->transform().fit==transform.fit);
    HDRSHOT_CHECK(source->image_rect()==image_before);
    HDRSHOT_CHECK(root.window.level==NSFloatingWindowLevel);
  }
  collapse->click();
  window->findChild<QToolButton *>("pinTopmost")->click();
  HDRSHOT_CHECK(root.window.level==NSNormalWindowLevel);
  window->refresh_presentation();
  window->close();window.reset();
  HDRSHOT_CHECK(spin([&]{auto s=macos_analysis_presentation_status(*data.presenter);return !s.worker_active&&!s.pending;}));
  std::cout<<"native collapse surface hidden, no submissions, fit/manual restore and closed lifecycle passed\n";
}

void native() {
  for(bool hdr:{false,true}) {
    auto snapshot=fixture(hdr);
    auto processor=MacMetalExportPixelProcessor::create();
    HDRSHOT_CHECK(processor.has_value());
    auto shared_processor=std::shared_ptr<MacMetalExportPixelProcessor>(std::move(processor.value()));
    auto plan=std::make_shared<AnnotationRenderPlan>(*snapshot.annotation_render_plan);
    plan->ordered_layers.push_back({{1},AnnotationKind::rectangle,{20,20,4,1},0xff0000,
                                   {{20,20,{128,255,255,128}}},"native-pin-fixture"});
    snapshot.annotation_render_plan=plan;
    CleanContentCache cache(shared_processor);
    auto clean=cache.get(snapshot.frozen_desktop,snapshot.target_display_id,plan);
    HDRSHOT_CHECK(clean.has_value());
    snapshot.frozen_desktop=clean.value()->source;
    snapshot.clean_content=clean.value();
    snapshot.clean_document_revision=snapshot.annotations.revision;
    auto prepared=prepare_macos_pinned_screenshot(snapshot);
    HDRSHOT_CHECK(prepared.has_value());
    auto &data=prepared.value();
    HDRSHOT_CHECK(data.image.input.is_hdr==hdr);
    HDRSHOT_CHECK(data.image.input.source->size_px()==(PixelSize{224,144}));
    HDRSHOT_CHECK(!data.image.original.clean_content->owned.empty());
    auto second=prepare_macos_pinned_screenshot(data.image.original);
    HDRSHOT_CHECK(second && second.value().image.input.is_hdr==hdr);
    auto first_pixels=data.image.input.source->read_region({0,0,224,144});
    auto second_pixels=second.value().image.input.source->read_region({0,0,224,144});
    HDRSHOT_CHECK(first_pixels && second_pixels && first_pixels.value()==second_pixels.value());
    auto output_snapshot=data.image.original;
    output_snapshot.save_format=SaveFormat::png_display_p3_dual_range;
    auto exported=ExportWorkflow::prepare(output_snapshot,nullptr,shared_processor.get(),
                                         nullptr,nullptr,shared_processor.get());
    HDRSHOT_CHECK(exported.has_value());
    const auto &png=exported.value().artifact.bytes;
    HDRSHOT_CHECK(png.size()>24 && png[18]==0 && png[19]==224 && png[22]==0 && png[23]==144);
    PinnedScreenshotWindow window(data.image.input,{});
    window.resize(400,400);
    auto native=make_macos_pinned_window_port();
    window.topmost_changed=[&](bool top){native->set_always_on_top(window,top);};
    window.front_requested=[&]{native->bring_forward(window);};
    window.present_requested=[&](auto view,auto surface){
      auto result=data.presenter->present(data.image.input,view,surface);
      HDRSHOT_CHECK(result.has_value());
    };
    window.show();native->set_always_on_top(window,true);native->bring_forward(window);
    auto *ns=(__bridge NSView *)(void *)window.winId();
    HDRSHOT_CHECK(ns.window.level==NSFloatingWindowLevel);
    HDRSHOT_CHECK(spin([&]{return macos_analysis_presentation_status(*data.presenter).completed>0;}));
    auto *pin=window.findChild<QToolButton *>("pinTopmost");HDRSHOT_CHECK(pin);
    pin->click();HDRSHOT_CHECK(ns.window.level==NSNormalWindowLevel);
    pin->click();HDRSHOT_CHECK(ns.window.level==NSFloatingWindowLevel);
    const auto before=macos_analysis_presentation_status(*data.presenter).completed;
    window.source_view()->zoom_at({180,180},1.5);
    HDRSHOT_CHECK(spin([&]{return macos_analysis_presentation_status(*data.presenter).completed>before;}));
    auto after_zoom=ExportWorkflow::prepare(output_snapshot,nullptr,shared_processor.get(),
                                           nullptr,nullptr,shared_processor.get());
    HDRSHOT_CHECK(after_zoom && after_zoom.value().artifact.bytes==png);
    window.findChild<QToolButton *>("pinFit")->click();
    HDRSHOT_CHECK(spin([&]{auto s=macos_analysis_presentation_status(*data.presenter);return s.completed_generation==s.requested&&!s.worker_active;}));
    {
      PinnedScreenshotWindow other(data.image.input,{false,400});
      other.show();native->set_always_on_top(other,false);
      auto *other_ns=(__bridge NSView *)(void *)other.winId();
      HDRSHOT_CHECK(other_ns.window.level==NSNormalWindowLevel && !other.always_on_top());
      HDRSHOT_CHECK(window.always_on_top());
      native->set_always_on_top(other,true);
      native->bring_forward(window);
      HDRSHOT_CHECK([NSApp.orderedWindows indexOfObject:ns.window] <
                    [NSApp.orderedWindows indexOfObject:other_ns.window]);
      native->bring_forward(other);
      HDRSHOT_CHECK([NSApp.orderedWindows indexOfObject:other_ns.window] <
                    [NSApp.orderedWindows indexOfObject:ns.window]);
      other.close();HDRSHOT_CHECK(!window.is_closed());
    }
    const auto folder=qEnvironmentVariable("HDRSHOT_PIN_NATIVE_CAPTURE");
    if(!folder.isEmpty()) {
      QDir().mkpath(folder);
      QEvent enter(QEvent::Enter);QApplication::sendEvent(&window,&enter);
      spin([]{return false;},100);
      const QString path=folder+(hdr?"/pin-hdr-native.png":"/pin-sdr-native.png");
      QProcess capture;
      capture.start("/usr/sbin/screencapture",{"-x","-o","-l",QString::number(ns.window.windowNumber),path});
      const bool captured=capture.waitForFinished(5000);
      std::cout<<"capture exit="<<capture.exitCode()<<" visible="<<ns.window.visible
               <<" occlusion="<<ns.window.occlusionState<<" screen="<<NSScreen.screens.count
               <<" error="<<capture.readAllStandardError().toStdString()<<std::endl;
      HDRSHOT_CHECK(captured && capture.exitCode()==0);
      QImage image(path);HDRSHOT_CHECK(!image.isNull());
      const double ratio=double(image.width())/window.width();
      // Final WindowServer pixels: green analysis and blue copy must be above
      // the native Metal view. No QWidget::grab/render drives this check.
      for(const auto &entry:{std::pair{"pinAnalyze",true},std::pair{"pinCopy",false}}) {
        auto *b=window.findChild<QToolButton *>(entry.first);
        const auto origin=b->mapTo(&window,QPoint(4,4));
        const auto c=image.pixelColor(int(origin.x()*ratio),int(origin.y()*ratio));
        HDRSHOT_CHECK(entry.second ? c.green()>c.red()*1.2 : c.blue()>c.red()*1.5);
      }
      std::cout<<"WindowServer capture "<<path.toStdString()<<'\n';
    }
    window.close();
    HDRSHOT_CHECK(spin([&]{auto s=macos_analysis_presentation_status(*data.presenter);return !s.worker_active&&!s.pending;}));
  }
}
void preview_expansion_probe() {
  auto prepared=prepare_macos_pinned_screenshot(fixture(false));HDRSHOT_CHECK(prepared.has_value());
  const auto data=prepared.value();
  PinnedScreenshotWindow window(data.image.input,{});window.setGeometry(100,160,500,340);
  auto native=make_macos_pinned_window_port();
  window.present_requested=[&](auto view,auto surface){HDRSHOT_CHECK(data.presenter->present(data.image.input,view,surface).has_value());};
  window.preview()->present=[&](int index,auto view,auto surface){return bool(data.auxiliary[std::size_t(index)].presenter->present(data.image.input,view,surface));};
  window.preview()->configure_window=[&](QWidget &card){native->configure_preview(card,window);};
  window.show();native->set_always_on_top(window,true);
  HDRSHOT_CHECK(spin([&]{return macos_analysis_presentation_status(*data.presenter).completed>0;}));
  auto *toggle=window.findChild<QToolButton *>("pinCollapse");
  NSView *root=(__bridge NSView *)(void *)window.winId();
  NSView *source=(__bridge NSView *)(void *)window.source_view()->presentation_surface()->winId();
  auto *card=&window.preview()->host(1);
  for(bool preview:{false,true}) {
    toggle->click();spin([]{return false;},200);
    if(preview) {
      QEvent enter(QEvent::Enter);QApplication::sendEvent(toggle,&enter);
      HDRSHOT_CHECK(spin([&]{return card->isVisible() && macos_analysis_presentation_status(*data.auxiliary[1].presenter).completed>0;}));
      spin([]{return false;},160);
    }
    QProcess recording;
    const auto output=qEnvironmentVariable("HDRSHOT_TRANSITION_RECORD");
    if(!output.isEmpty()) {
      recording.start("/usr/sbin/screencapture",{"-x","-v","-V","3","-R100,160,500,340",
          output+QString("-%1.mov").arg(preview)});
      HDRSHOT_CHECK(recording.waitForStarted());
      spin([]{return false;},1000);
    }
    QElapsedTimer timer;timer.start();
    toggle->click();
    for(int step=0;step<45;++step) {
      CAMetalLayer *metal=nil;
      for(CALayer *child in source.layer.sublayers)if([child isKindOfClass:CAMetalLayer.class])metal=(CAMetalLayer *)child;
      auto *visible=(CALayer *)metal.presentationLayer;
      NSView *peek=card->internalWinId() ? (__bridge NSView *)(void *)card->internalWinId() : nil;
      std::cout<<"transition preview="<<preview<<" ms="<<timer.elapsed()
        <<" root="<<NSStringFromRect(root.window.frame).UTF8String
        <<" source="<<NSStringFromRect(source.frame).UTF8String
        <<" model="<<NSStringFromRect(metal.frame).UTF8String
        <<" presented="<<NSStringFromRect(visible.frame).UTF8String
        <<" animations="<<(metal.animationKeys.description ?: @"[]").UTF8String
        <<" card_visible="<<peek.window.visible<<" card_parent="<<(peek.window.parentWindow!=nil)
        <<" card_animation="<<peek.window.animationBehavior<<std::endl;
      spin([]{return false;},8);
    }
    if(!output.isEmpty()) {
      HDRSHOT_CHECK(spin([&]{return recording.state()==QProcess::NotRunning;},6000));
      std::cout<<"record exit="<<recording.exitCode()<<" "<<recording.readAllStandardError().toStdString()<<std::endl;
      HDRSHOT_CHECK(recording.exitCode()==0);
    }
  }
  window.close();
}

void thumbnail_native_clicks() {
  auto prepared=prepare_macos_pinned_screenshot(fixture(false));HDRSHOT_CHECK(prepared.has_value());
  const auto data=prepared.value();
  PinnedScreenshotWindow window(data.image.input,{});window.setGeometry(100,160,500,340);
  auto native=make_macos_pinned_window_port();
  window.present_requested=[&](auto view,auto surface){HDRSHOT_CHECK(data.presenter->present(data.image.input,view,surface).has_value());};
  window.preview()->present=[&](int index,auto view,auto surface){return bool(data.auxiliary[std::size_t(index)].presenter->present(data.image.input,view,surface));};
  window.preview()->configure_window=[&](QWidget &card){native->configure_preview(card,window);};
  window.show();native->set_always_on_top(window,true);
  auto *toggle=window.findChild<QToolButton *>("pinCollapse");
  auto &thumb=window.preview()->source(0);
  auto *surface=thumb.presentation_surface();
  int clicks=0;QObject::connect(toggle,&QToolButton::clicked,[&]{++clicks;});
  auto send=[&](QPoint point,NSEventType type) {
    NSView *root=(__bridge NSView *)(void *)window.winId();
    const QPoint in_root=toggle->mapTo(&window,point);
    NSPoint local=NSMakePoint(in_root.x(),root.flipped?in_root.y():root.bounds.size.height-in_root.y());
    NSView *hit=[root hitTest:local];HDRSHOT_CHECK(hit!=nil);
    const NSPoint location=[root convertPoint:local toView:nil];
    NSEvent *event=[NSEvent mouseEventWithType:type location:location modifierFlags:0
        timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:root.window.windowNumber
        context:nil eventNumber:0 clickCount:1 pressure:type==NSEventTypeLeftMouseDown?1:0];
    if(type==NSEventTypeLeftMouseDown)[hit mouseDown:event];else [hit mouseUp:event];
    QApplication::processEvents();
  };
  for(const QPoint point:{QPoint(19,19),QPoint(4,19),QPoint(2,2),QPoint(31,31)}) {
    toggle->click();
    HDRSHOT_CHECK(spin([&]{return macos_analysis_presentation_status(*data.auxiliary[0].presenter).completed>0;}));
    spin([]{return false;},80);const int before=clicks;
    QEvent enter(QEvent::Enter);QApplication::sendEvent(toggle,&enter);
    HDRSHOT_CHECK(spin([&]{return window.preview()->host(1).isVisible();}));
    NSView *preview=(__bridge NSView *)(void *)window.preview()->host(1).winId();
    // Qt visibility becomes false before AppKit's utility-window fade ends.
    // Native dismissal must be immediate before restoring the owner's height.
    HDRSHOT_CHECK(preview.window.animationBehavior==NSWindowAnimationBehaviorNone);
    send(point,NSEventTypeLeftMouseDown);
    HDRSHOT_CHECK(!preview.window.visible);
    HDRSHOT_CHECK(toggle->isDown() && !thumb.dragging() && !window.preview()->host(1).isVisible());
    send(point,NSEventTypeLeftMouseUp);
    HDRSHOT_CHECK(window.height()==340 && clicks==before+1);
    spin([]{return false;},330);HDRSHOT_CHECK(!window.preview()->host(1).isVisible());
    std::cout<<"native hit-test expand point="<<point.x()<<","<<point.y()<<" passed\n";
  }
  (void)surface;
  window.close();
}

void thumbnail_native(bool portrait=false) {
  auto snapshot=fixture(false);
  auto pixels=std::make_shared<FrozenDesktop>(*snapshot.frozen_desktop);
  for(auto &segment:pixels->canonical_segments) {
    for(std::size_t pixel=0;pixel<segment.rgba_float.size()/4;++pixel) {
      const bool left=(pixel%256)<128,top=(pixel/256)<80;
      segment.rgba_float[pixel*4]=top?(left?.7f:.05f):(left?.05f:.7f);
      segment.rgba_float[pixel*4+1]=top?.05f:.7f;
      segment.rgba_float[pixel*4+2]=top?(left?.05f:.7f):.05f;
    }
  }
  snapshot.frozen_desktop=pixels;
  if(portrait) {
    snapshot.selection={1,{16,8,64,144}};
    snapshot.annotation_render_plan=std::make_shared<AnnotationRenderPlan>(
        AnnotationRenderPlan{1,{64,144},{},snapshot.selection.desktop_rect});
  }
  auto prepared=prepare_macos_pinned_screenshot(snapshot);HDRSHOT_CHECK(prepared.has_value());
  auto data=prepared.value();
  QtPinnedScreenshotController controller(*qApp,[data](const ExportSnapshot &) {
    return Result<PreparedPinnedScreenshot,Error>::success(data);
  },make_macos_pinned_window_port(),{}, {},[]{return SettingsSnapshot{};},{});
  bool done=false;
  controller.open(fixture(false),[&](auto result){HDRSHOT_CHECK(result.has_value());done=true;});
  HDRSHOT_CHECK(spin([&]{return done;}));
  PinnedScreenshotWindow *window=nullptr;
  for(auto *top:QApplication::topLevelWidgets())
    if(auto *pin=dynamic_cast<PinnedScreenshotWindow *>(top);pin && !pin->is_closed())window=pin;
  HDRSHOT_CHECK(window);
  auto *button=window->findChild<QToolButton *>("pinCollapse");
  auto *card=&window->preview()->host(1);
  auto *root=(__bridge NSView *)(void *)window->winId();
  // Keep test fixtures on the visible Space without activating another app.
  root.window.collectionBehavior=NSWindowCollectionBehaviorCanJoinAllSpaces |
      NSWindowCollectionBehaviorFullScreenAuxiliary;
  const auto folder=qEnvironmentVariable("HDRSHOT_PIN_PREVIEW_CAPTURE");
  auto capture=[&](QWidget &widget,QString name) {
    if(folder.isEmpty())return;
    QDir().mkpath(folder);
    auto *native=(__bridge NSView *)(void *)widget.winId();
    QProcess process;process.start("/usr/sbin/screencapture",{"-x","-o","-l",
        QString::number(native.window.windowNumber),folder+"/"+name+".png"});
    const bool finished=process.waitForFinished(5000);
    std::cout<<"preview capture "<<name.toStdString()<<" id="<<native.window.windowNumber
      <<" visible="<<native.window.visible<<" occlusion="<<native.window.occlusionState
      <<" error="<<process.readAllStandardError().toStdString()<<std::endl;
    HDRSHOT_CHECK(finished && process.exitCode()==0);
  };
  for(bool dark:{false,true}) for(int width:{450,500}) {
    auto palette=window->palette();palette.setColor(QPalette::Window,dark?QColor("#26292E"):QColor("#F5F6F8"));
    window->setPalette(palette);window->setGeometry(120,140,width,340);
    button->click();
    HDRSHOT_CHECK(spin([&]{return macos_analysis_presentation_status(*data.auxiliary[0].presenter).completed>0;}));
    spin([]{return false;},150);
    const auto old_key=NSApp.keyWindow;
    const auto size=window->size();
    const auto origin=window->pos();
    QEvent enter(QEvent::Enter);QApplication::sendEvent(button,&enter);
    HDRSHOT_CHECK(spin([&]{return card->isVisible() && macos_analysis_presentation_status(*data.auxiliary[1].presenter).completed>0;}));
    spin([]{return false;},200);
    auto *card_view=(__bridge NSView *)(void *)card->winId();
    HDRSHOT_CHECK(card_view.window.ignoresMouseEvents && !card_view.window.keyWindow);
    HDRSHOT_CHECK(card_view.window.animationBehavior==NSWindowAnimationBehaviorNone);
    HDRSHOT_CHECK(NSApp.keyWindow==old_key && window->size()==size && window->pos()==origin);
    HDRSHOT_CHECK(card_view.window.parentWindow==root.window);
    const auto suffix=QString("%1-%2-%3").arg(width).arg(dark?"dark":"light").arg(portrait?"portrait":"landscape");
    std::cout<<"preview layout "<<suffix.toStdString()<<" dpr="<<window->devicePixelRatioF()
       <<" button="<<button->width()<<" thumb="<<window->preview()->source(0).width()<<"x"<<window->preview()->source(0).height()
       <<" card="<<card->width()<<"x"<<card->height()<<std::endl;
    capture(*window,"thumbnail-"+suffix);capture(*card,"preview-"+suffix);
    button->click();HDRSHOT_CHECK(!card->isVisible() && window->height()==340);
    spin([]{return false;},100);
    const auto thumb_count=macos_analysis_presentation_status(*data.auxiliary[0].presenter).requested;
    const auto card_count=macos_analysis_presentation_status(*data.auxiliary[1].presenter).requested;
    window->preview()->refresh(0);window->preview()->refresh(1);
    spin([]{return false;},350);
    HDRSHOT_CHECK(!card->isVisible());
    HDRSHOT_CHECK(macos_analysis_presentation_status(*data.auxiliary[0].presenter).requested==thumb_count);
    HDRSHOT_CHECK(macos_analysis_presentation_status(*data.auxiliary[1].presenter).requested==card_count);
  }
  controller.shutdown();
  std::cout<<"native thumbnail and hover preview passed\n";
}

}
int main(int argc,char **argv) {
  QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
  if(!MTLCreateSystemDefaultDevice() || !NSScreen.screens.count) return 77;
  try {
    if(app.arguments().contains("--expansion-probe")) {preview_expansion_probe();return 0;}
    if(app.arguments().contains("--thumbnail-click-only")) {thumbnail_native_clicks();return 0;}
    if(app.arguments().contains("--preview-only")) {thumbnail_native();thumbnail_native(true);return 0;}
    if(app.arguments().contains("--boundary-only")) {
      boundary_native_roundtrip();return 0;
    }
    if(app.arguments().contains("--collapse-only")) {
      collapse_native_roundtrip();
      std::cout<<"native collapse tests passed\n";return 0;
    }
    const bool inactive_only=app.arguments().contains("--inactive-hover-only");
    if(!inactive_only) { native(); collapse_native_roundtrip(); }
    inactive_hover();
    // Native flag recreation can affect activation; keep it after the existing
    // inactive-hover scenario so the latter retains its original preconditions.
    if(!inactive_only) { boundary_native_roundtrip();thumbnail_native(); }
    std::cout<<"native pin tests passed\n";return 0;
  }
  catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
