#include "platform/macos/macos_qt_overlay_window.hpp"
#include "test_support.hpp"
#include "platform/macos/metal_edr_presenter.hpp"
#include <future>
#include <QElapsedTimer>
#include <QDir>
#include "ui/qt/capture_magnifier_panel.hpp"
#include "domain/analysis/math.hpp"

#include <QApplication>
#include <QWidget>
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

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
void card_is_passive_and_moves_as_one_hidden_surface() {
  QWidget host; prepare_host(host);
  hdrshot::MacQtOverlayWindow adapter((__bridge void*)NSScreen.mainScreen);
  adapter.prepare(host);
  HDRSHOT_CHECK(!adapter.preparation_error());
  auto layer = (__bridge CAMetalLayer*)adapter.magnifier_surface();
  HDRSHOT_CHECK(layer != nil && layer != (__bridge CAMetalLayer*)adapter.native_surface());
  NSWindow* card = nil;
  for(NSWindow* window in NSApp.windows) if(window.contentView.layer == layer) card=window;
  HDRSHOT_CHECK(card != nil && !card.visible && card.ignoresMouseEvents);
  HDRSHOT_CHECK(!card.canBecomeKeyWindow && !card.canBecomeMainWindow);
  HDRSHOT_CHECK(!card.movable && !card.hasShadow);
  HDRSHOT_CHECK((card.styleMask & NSWindowStyleMaskFullScreen)==0);
  const auto parent = native_window(host).frame;
  for(auto rect : {QRect(24,32,220,320),QRect(120,160,216,400)}) {
    adapter.move_magnifier(host,rect);
    HDRSHOT_CHECK_NEAR(NSMinX(card.frame),NSMinX(parent)+rect.x(),.01);
    HDRSHOT_CHECK_NEAR(NSMaxY(card.frame),NSMaxY(parent)-rect.y(),.01);
    HDRSHOT_CHECK_NEAR(layer.drawableSize.width,qRound(rect.width()*host.devicePixelRatioF()),.01);
    HDRSHOT_CHECK_NEAR(layer.drawableSize.height,qRound(rect.height()*host.devicePixelRatioF()),.01);
    HDRSHOT_CHECK(adapter.magnifier_surface()==(__bridge void*)layer && !card.visible);
  }
  auto presenter=hdrshot::MacMetalEdrPresenter::create();
  HDRSHOT_CHECK(presenter.has_value());
  HDRSHOT_CHECK(presenter.value()->configure_target_layer(adapter.magnifier_surface(),
      hdrshot::MacMetalSurfaceRange::edr).has_value());
  adapter.move_magnifier(host,QRect(24,32,12,24));
  hdrshot::MacMetalOverlayRequest source;
  source.width_px=source.height_px=2;source.source_is_linear=true;
  source.rgba_half_extended_p3=std::make_shared<const std::vector<std::uint16_t>>(16,0x4000);
  const int backing=qRound(host.devicePixelRatioF());
  source.magnifier={true,{1,1},{0,4,12,12},4,1};
  hdrshot::MacMetalCardRequest request{source,{12*backing,24*backing},
      std::vector<std::uint8_t>(std::size_t(12*24*backing*backing*4))};
  auto rendered=std::async(std::launch::async,[&]{return presenter.value()->present_card_to_layer(
      adapter.magnifier_surface(),request,[](auto&){return true;});});
  QElapsedTimer deadline;deadline.start();
  while(rendered.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready && deadline.elapsed()<3000)
    QApplication::processEvents(QEventLoop::AllEvents,5);
  HDRSHOT_CHECK(rendered.wait_for(std::chrono::milliseconds(0))==std::future_status::ready);
  auto receipt=rendered.get();
  if(!receipt)for(const auto& [key,value]:receipt.error().safe_context)std::cerr<<key<<'='<<value<<'\n';
  HDRSHOT_CHECK(receipt.has_value());
  HDRSHOT_CHECK(!card.visible);
  adapter.show_magnifier(); // hidden host must not expose a late completion
  HDRSHOT_CHECK(!card.visible);
  adapter.hide_magnifier();HDRSHOT_CHECK(card.parentWindow==nil);
}
void complete_card_renders_real_panel_text() {
  using namespace hdrshot;
  const auto original=qApp->palette();
  auto presenter=MacMetalEdrPresenter::create();HDRSHOT_CHECK(presenter.has_value());
  for(bool dark:{false,true})for(int dpr:{1,2})for(bool all:{false,true}) {
    auto palette=original;palette.setColor(QPalette::Window,dark?QColor("#20242a"):QColor("#f4f5f7"));
    palette.setColor(QPalette::WindowText,dark?Qt::white:Qt::black);qApp->setPalette(palette);
    QWidget parent;CaptureMagnifierPanel panel(&parent);MagnifierReadings fields;
    if(all)fields.selected.fill(true);
    const int width=dpr==1?216:220; // Same odd-cell geometry as QtOverlayHost.
    const int height=width+panel.header_height()+panel.text_height(fields,width);
    panel.resize(width,height);
    panel.show_sample({31,31},magnifier_readout({.25F,.5F,.75F},100,false),fields,width,8,false);
    QImage ui(width*dpr,height*dpr,QImage::Format_ARGB32_Premultiplied);ui.setDevicePixelRatio(dpr);ui.fill(Qt::transparent);
    panel.render(&ui);
    ui=ui.convertToFormat(QImage::Format_RGBA8888);
    MacMetalOverlayRequest source;source.width_px=source.height_px=64;source.source_is_linear=true;
    auto pixels=std::make_shared<LinearFloatPixels>(64*64*4);
    for(int y=0;y<64;++y)for(int x=0;x<64;++x){auto i=std::size_t(y*64+x)*4;
      (*pixels)[i]=.25F;(*pixels)[i+1]=.5F;(*pixels)[i+2]=.75F;(*pixels)[i+3]=1.F;}
    source.rgba_float_linear_p3=pixels;
    const int image=magnifier_odd_pixels(width*dpr,8)*8;
    source.magnifier={true,{31,31},{0,28*dpr,image,image},8,1};
    MacMetalCardRequest card{source,{ui.width(),ui.height()},
      std::vector<std::uint8_t>(ui.constBits(),ui.constBits()+ui.sizeInBytes())};
    auto result=presenter.value()->render_card_offscreen(card);HDRSHOT_CHECK(result.has_value());
    const auto center=std::size_t((28*dpr+image/2)*ui.width()+image/2)*4;
    HDRSHOT_CHECK_NEAR(result.value().rgba_linear_display_p3[center],.25,.0005);
    if(const char* directory=std::getenv("HDRSHOT_CARD_VISUAL_DIR")) {
      QDir().mkpath(QString::fromUtf8(directory));QImage output(ui.size(),QImage::Format_RGB888);
      for(int y=0;y<ui.height();++y)for(int x=0;x<ui.width();++x){auto i=std::size_t(y*ui.width()+x)*4;
        const auto& p=result.value().rgba_linear_display_p3;
        auto rgb=analysis_math::display_srgb_from_work(analysis_math::a3(p[i],p[i+1],p[i+2]),1u);
        output.setPixelColor(x,y,QColor::fromRgbF(std::clamp(rgb.x,0.F,1.F),std::clamp(rgb.y,0.F,1.F),std::clamp(rgb.z,0.F,1.F)));}
      auto path=QString("%1/card_%2_%3_dpr%4.png").arg(QString::fromUtf8(directory))
        .arg(dark?"dark":"light").arg(all?"all":"default").arg(dpr);
      HDRSHOT_CHECK(output.save(path));
    }
  }
  qApp->setPalette(original);
}
} // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  if (QApplication::platformName() != QStringLiteral("cocoa") || !NSScreen.mainScreen) {
    std::cout << "SKIP: native Cocoa window/screen unavailable\n";
    return 77;
  }
  return hdrshot::test::run({
      {"complete card renders real Qt text in both themes and scales",complete_card_renders_real_panel_text},
      {"card is passive and moves its complete surface",card_is_passive_and_moves_as_one_hidden_surface},
      {"hidden native overlay is fixed", hidden_overlay_is_fixed_without_native_fullscreen},
      {"reconfiguration preserves fixed geometry and surface", reconfiguration_reasserts_native_flags_without_rebuilding_surface},
      {"hidden and destroyed focus observers remain inert", hidden_and_destroyed_observers_do_not_report_interruption},
      {"save dialog lowering keeps geometry locked", lowering_for_save_dialog_does_not_unlock_overlay}});
}
