#include "platform/windows/windows_qt_pinned_screenshot.hpp"
#include "platform/windows/windows_analysis_backend.hpp"
#include "platform/windows/windows_d3d_export.hpp"
#include "platform/windows/windows_color.hpp"
#include "platform/cpu/cpu_analysis_port.hpp"
#include "application/clean_content_cache.hpp"
#include "ui/qt/analyzer_source_view.hpp"
#include "ui/qt/pinned_screenshot_preview.hpp"
#include "test_support.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QToolButton>
#include <QScreen>
#include <QDir>
#include <QPointer>
#include <QProcess>
#include <QStyleHints>
#include <QWindow>
#include <windows.h>
#include <dwmapi.h>
#include <iostream>

using namespace hdrshot;
namespace {
bool verify_desktop_pixels{};
QImage desktop_region(QRect bounds) {
  // Read only the requested composed desktop region. No QWidget repaint or
  // DwmFlush: probes must not repair presentation before observing it.
  const auto desktop=GetDC(nullptr),memory=CreateCompatibleDC(desktop);
  BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth=bounds.width();info.bmiHeader.biHeight=-bounds.height();
  info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
  void *pixels{};
  const auto bitmap=CreateDIBSection(desktop,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
  HDRSHOT_CHECK(bitmap && memory && pixels);
  const auto previous=SelectObject(memory,bitmap);
  const bool copied=BitBlt(memory,0,0,bounds.width(),bounds.height(),desktop,
      bounds.x(),bounds.y(),SRCCOPY|CAPTUREBLT);
  // DIB bits may not be read until this thread's batched GDI copy completes.
  // This flushes the capture copy, not Qt painting or DWM presentation.
  const bool flushed=GdiFlush();
  const auto image=QImage(static_cast<uchar *>(pixels),bounds.width(),bounds.height(),QImage::Format_RGB32).copy();
  SelectObject(memory,previous);DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(nullptr,desktop);
  HDRSHOT_CHECK(copied && flushed);return image;
}
void wait(int ms) {
  QElapsedTimer t;t.start();
  while(t.elapsed()<ms) {QApplication::processEvents();QThread::msleep(2);}
}
bool spin(std::function<bool()> predicate,int timeout=6000) {
  QElapsedTimer t;t.start();
  while(!predicate() && t.elapsed()<timeout) wait(3);
  return predicate();
}
ExportSnapshot fixture(bool hdr) {
  auto desktop=std::make_shared<FrozenDesktop>();
  desktop->frame_id={44};desktop->display_generation=1;
  CanonicalFrameSegment s;
  s.display_id={1};s.size_px={256,160};s.point_pixel_scale=1;
  s.desktop_frame_points={0,0,256,160};
  desktop->desktop_bounds_points=s.desktop_frame_points;
  s.encoding=WindowsColor::linear_p3_encoding();s.pixel_format=PixelFormat::rgba32_float;
  s.display_dynamic_range=hdr?DisplayDynamicRange::hdr:DisplayDynamicRange::sdr;
  s.capture_sdr_tolerance=hdr;
  s.rgba_float.resize(256U*160U*4U);
  for(std::size_t p=0;p<256U*160U;++p) {
    const float x=float(p%256)/255;
    s.rgba_float[p*4]=.02F+x*(hdr?5.98F:1.001F);
    s.rgba_float[p*4+1]=.03F+x*(hdr?2.97F:.97F);
    s.rgba_float[p*4+2]=.04F+x*(hdr?3.96F:.96F);
    s.rgba_float[p*4+3]=1;
  }
  desktop->canonical_segments.push_back(std::move(s));
  ExportSnapshot snapshot;
  snapshot.session_id={22};snapshot.operation_id={1};
  snapshot.frozen_desktop=desktop;snapshot.target_display_id={1};
  snapshot.selection={1,{16,8,240,144}};snapshot.annotations.revision=1;
  snapshot.annotation_render_plan = std::make_shared<AnnotationRenderPlan>(
      AnnotationRenderPlan{1,
                           {240, 144},
                           {},
                           snapshot.selection.desktop_rect,
                           AnnotationScale::from(1).value()});
  return snapshot;
}
void native_content() {
  for(bool hdr:{false,true}) {
    auto snapshot=fixture(hdr);
    auto processor=WindowsD3DExportPixelProcessor::create();HDRSHOT_CHECK(processor.has_value());
    auto shared=std::shared_ptr<WindowsD3DExportPixelProcessor>(std::move(processor.value()));
    auto plan=std::make_shared<AnnotationRenderPlan>(*snapshot.annotation_render_plan);
    plan->ordered_layers.push_back({{1},AnnotationKind::rectangle,{20,20,4,1},0xff0000,
                                   {{20,20,{128,255,255,128}}},"windows-pin-fixture"});
    snapshot.annotation_render_plan=plan;
    CleanContentCache cache;
    auto clean=cache.get(snapshot.frozen_desktop,snapshot.target_display_id,plan);HDRSHOT_CHECK(clean.has_value());
    snapshot.frozen_desktop=clean.value()->source;snapshot.clean_content=clean.value();
    snapshot.clean_document_revision=1;
    auto prepared=prepare_windows_pinned_screenshot(snapshot);HDRSHOT_CHECK(prepared.has_value());
    auto &data=prepared.value();
    HDRSHOT_CHECK(data.image.input.is_hdr==hdr);
    HDRSHOT_CHECK(data.image.input.source->size_px()==(PixelSize{240,144}));
    const auto &segment=data.image.original.frozen_desktop->canonical_segments.front();
    HDRSHOT_CHECK(segment.display_dynamic_range==(hdr?DisplayDynamicRange::hdr:DisplayDynamicRange::sdr));
    HDRSHOT_CHECK(segment.capture_sdr_tolerance==hdr);
    HDRSHOT_CHECK(!data.image.original.clean_content->owned.empty());
    auto second=prepare_windows_pinned_screenshot(data.image.original);HDRSHOT_CHECK(second.has_value());
    HDRSHOT_CHECK(second.value().image.input.is_hdr==hdr);
    auto pixels=data.image.input.source->read_region({0,0,240,144});
    auto again=second.value().image.input.source->read_region({0,0,240,144});
    HDRSHOT_CHECK(pixels && again && pixels.value()==again.value());
    auto cpu=make_cpu_analysis_port();WindowsLinearP3RangeProbe range;
    auto reference=AnalysisWorkflow::prepare(snapshot,*cpu,&range);HDRSHOT_CHECK(reference.has_value());
    auto expected=reference.value().input.source->read_region({0,0,240,144});HDRSHOT_CHECK(expected.has_value());
    HDRSHOT_CHECK(pixels.value().size()==expected.value().size());
    for(std::size_t p=0;p<pixels.value().size();++p)
      HDRSHOT_CHECK_NEAR(pixels.value()[p],expected.value()[p],2e-6);
    auto output=data.image.original;output.save_format=SaveFormat::png_display_p3_dual_range;
    auto exported=ExportWorkflow::prepare(output,nullptr,shared.get(),nullptr,nullptr,&range);
    HDRSHOT_CHECK(exported.has_value());
    const auto png=exported.value().artifact.bytes;
    HDRSHOT_CHECK(png.size()>24 && png[19]==240 && png[23]==144);
    PinnedScreenshotWindow window(data.image.input,{});window.resize(400,400);
    auto attached=data.attach_window(window); if(!attached) for(const auto &[key,value]:attached.error().safe_context) std::cerr<<key<<"="<<value<<" "; HDRSHOT_CHECK(attached.has_value());
    auto native=make_windows_pinned_window_port();
    window.topmost_changed=[&](bool top){native->set_always_on_top(window,top);};
    window.front_requested=[&]{native->bring_forward(window);};
    window.present_requested=[&](auto view,auto surface) {
      HDRSHOT_CHECK(data.presenter->present(data.image.input,view,surface).has_value());
    };
    window.show();native->set_always_on_top(window,true);native->bring_forward(window);
    const auto hwnd=reinterpret_cast<HWND>(window.winId());
    const auto surface=reinterpret_cast<HWND>(window.source_view()->presentation_surface()->winId());
    HDRSHOT_CHECK(IsWindow(surface));
    HDRSHOT_CHECK(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&WS_EX_TOPMOST);
    HDRSHOT_CHECK(spin([&]{return windows_analysis_presentation_status(*data.presenter).completed>0;}));
    auto *pin=window.findChild<QToolButton *>("pinTopmost");HDRSHOT_CHECK(pin);
    pin->click();HDRSHOT_CHECK(!(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&WS_EX_TOPMOST));
    pin->click();HDRSHOT_CHECK(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&WS_EX_TOPMOST);
    const auto before=windows_analysis_presentation_status(*data.presenter).completed;
    window.source_view()->zoom_at({180,180},1.5);
    HDRSHOT_CHECK(spin([&]{return windows_analysis_presentation_status(*data.presenter).completed>before;}));
    auto after=ExportWorkflow::prepare(output,nullptr,shared.get(),nullptr,nullptr,&range);
    HDRSHOT_CHECK(after && after.value().artifact.bytes==png);
    const auto generation=windows_analysis_presentation_status(*data.presenter).display_generation;
    PostMessageW(hwnd,WM_DISPLAYCHANGE,32,0);
    HDRSHOT_CHECK(spin([&]{auto s=windows_analysis_presentation_status(*data.presenter);
      return s.completed_display_generation>generation && s.completed_generation==s.requested;}));
    PinnedScreenshotWindow other(data.image.input,{false,400});other.show();
    const auto other_hwnd=reinterpret_cast<HWND>(other.winId());
    native->set_always_on_top(other,false);native->bring_forward(other);
    HDRSHOT_CHECK(!(GetWindowLongPtrW(other_hwnd,GWL_EXSTYLE)&WS_EX_TOPMOST));
    native->set_always_on_top(other,true);native->bring_forward(window);
    const auto above=[](HWND a,HWND b){for(auto h=GetTopWindow(nullptr);h;h=GetWindow(h,GW_HWNDNEXT)) {
      if(h==a)return true;if(h==b)return false;}return false;};
    HDRSHOT_CHECK(above(hwnd,other_hwnd));native->bring_forward(other);HDRSHOT_CHECK(above(other_hwnd,hwnd));
    other.close();HDRSHOT_CHECK(!window.is_closed());
    window.close();wait(30);
  }
}

void hover_and_idle() {
  auto prepared=prepare_windows_pinned_screenshot(fixture(true));HDRSHOT_CHECK(prepared.has_value());
  auto &data=prepared.value();
  PinnedScreenshotWindow window(data.image.input,{});window.resize(533,400);
  auto attached=data.attach_window(window); if(!attached) for(const auto &[key,value]:attached.error().safe_context) std::cerr<<key<<"="<<value<<" "; HDRSHOT_CHECK(attached.has_value());
  window.present_requested=[&](auto view,auto surface){HDRSHOT_CHECK(data.presenter->present(data.image.input,view,surface).has_value());};
  int fronts=0,actions=0;window.front_requested=[&]{++fronts;};window.action_requested=[&](auto){++actions;};
  window.show();
  auto native=make_windows_pinned_window_port();native->set_always_on_top(window,true);
  HDRSHOT_CHECK(spin([&]{return windows_analysis_presentation_status(*data.presenter).completed>0;}));
  QWidget peer;peer.setWindowTitle("Pinned test focus peer");peer.show();peer.activateWindow();
  wait(80);
  const auto focus=GetForegroundWindow();
  auto *surface=window.source_view()->presentation_surface();
  const auto hwnd=reinterpret_cast<HWND>(surface->winId());
  const auto move=[&](int x,int y){
    const auto dpr=surface->devicePixelRatioF();
    SendMessageW(hwnd,WM_MOUSEMOVE,0,MAKELPARAM(int(x*dpr),int(y*dpr)));
  };
  move(200,200);wait(80);
  auto *toolbar=window.findChild<QWidget *>("pinnedToolbar");HDRSHOT_CHECK(toolbar && toolbar->isVisible());
  HDRSHOT_CHECK(surface->cursor().shape()==Qt::SizeAllCursor);
  const auto initial=windows_analysis_presentation_status(*data.presenter).requested;
  QElapsedTimer time;time.start();
  for(int i=0;i<2400;++i)move(100+i%200,150+i%40);
  const auto elapsed=time.nsecsElapsed();wait(80);
  HDRSHOT_CHECK(windows_analysis_presentation_status(*data.presenter).requested==initial);
  HDRSHOT_CHECK(GetForegroundWindow()==focus && fronts==0 && actions==0);
  // The synthetic mouse messages do not move the OS cursor. Finish the
  // toolbar visibility transition before checking the idle rendering budget.
  toolbar->hide();
  wait(120);
  const auto idle_initial=windows_analysis_presentation_status(*data.presenter).requested;
  FILETIME created{},exited{},kernel0{},user0{},kernel1{},user1{};
  GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel0,&user0);
  wait(500);
  GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel1,&user1);
  const auto ticks=[](FILETIME t){return (std::uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
  std::cout<<"hover2400_ms="<<elapsed/1e6<<" idle500ms_cpu_ms="
    <<(ticks(kernel1)+ticks(user1)-ticks(kernel0)-ticks(user0))/10000.
    <<" source_requests_delta="<<windows_analysis_presentation_status(*data.presenter).requested-idle_initial<<'\n';
  HDRSHOT_CHECK(windows_analysis_presentation_status(*data.presenter).requested==idle_initial);
  auto additional=prepare_windows_pinned_screenshot(fixture(false));HDRSHOT_CHECK(additional.has_value());
  auto &other_data=additional.value();
  PinnedScreenshotWindow other(other_data.image.input,{false,400});
  HDRSHOT_CHECK(other_data.attach_window(other).has_value());
  other.present_requested=[&](auto view,auto target) {
    HDRSHOT_CHECK(other_data.presenter->present(other_data.image.input,view,target).has_value());
  };
  other.show();
  native->set_always_on_top(other,false);
  HDRSHOT_CHECK(spin([&]{auto s=windows_analysis_presentation_status(*other_data.presenter);return s.completed>0 && !s.worker_active && !s.pending;}));
  // Removing the toolbar exposes the source and now correctly requests a
  // frame. Settle that UI transition before measuring genuinely idle work.
  auto *other_toolbar=other.findChild<QWidget *>("pinnedToolbar");
  HDRSHOT_CHECK(other_toolbar);
  other_toolbar->hide();
  wait(120);
  const auto a=windows_analysis_presentation_status(*data.presenter).requested;
  const auto b=windows_analysis_presentation_status(*other_data.presenter).requested;
  GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel0,&user0);
  wait(500);
  GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel1,&user1);
  HDRSHOT_CHECK(windows_analysis_presentation_status(*data.presenter).requested==a);
  HDRSHOT_CHECK(windows_analysis_presentation_status(*other_data.presenter).requested==b);
  std::cout<<"two_windows_idle500ms_cpu_ms="
    <<(ticks(kernel1)+ticks(user1)-ticks(kernel0)-ticks(user0))/10000.<<" source_requests_delta=0\n";
  std::cout<<"{\"hover2400_ms\":"<<elapsed/1e6<<",\"two_windows_idle500ms_cpu_ms\":"
      <<(ticks(kernel1)+ticks(user1)-ticks(kernel0)-ticks(user0))/10000.
      <<",\"source_requests_delta\":0,\"active_dpr\":"<<window.devicePixelRatioF()<<"}\n";
  other.close();
  window.hide();move(200,200);wait(20);HDRSHOT_CHECK(!toolbar->isVisible());
  window.close();
}

void desktop_hover() {
  POINT original{};GetCursorPos(&original);
  struct Restore {POINT point;~Restore(){SetCursorPos(point.x,point.y);}} restore{original};
  auto prepared=prepare_windows_pinned_screenshot(fixture(false));HDRSHOT_CHECK(prepared.has_value());
  auto &data=prepared.value();
  PinnedScreenshotWindow window(data.image.input,{});
  const auto area=QGuiApplication::primaryScreen()->availableGeometry();
  window.setGeometry(area.x()+80,area.y()+80,533,400);
  HDRSHOT_CHECK(data.attach_window(window).has_value());
  window.present_requested=[&](auto view,auto surface){HDRSHOT_CHECK(data.presenter->present(data.image.input,view,surface).has_value());};
  auto native=make_windows_pinned_window_port();
  window.show();native->set_always_on_top(window,true);
  HDRSHOT_CHECK(spin([&]{return windows_analysis_presentation_status(*data.presenter).completed>0;}));
  const auto hwnd=reinterpret_cast<HWND>(window.winId());
  QProcess peer;
  peer.start(QCoreApplication::applicationFilePath(),{"--focus-peer"});
  HDRSHOT_CHECK(peer.waitForStarted(5000));
  HWND peer_hwnd{};
  HDRSHOT_CHECK(spin([&]{peer_hwnd=FindWindowW(L"STATIC",L"SeriousShot pin test focus peer");return peer_hwnd!=nullptr;}));
  SetForegroundWindow(peer_hwnd);wait(100);
  HDRSHOT_CHECK(GetForegroundWindow()==peer_hwnd);
  RECT r{};HDRSHOT_CHECK(GetWindowRect(hwnd,&r));
  SetCursorPos(r.right+20,r.bottom+20);wait(800);
  auto *toolbar=window.findChild<QWidget *>("pinnedToolbar");HDRSHOT_CHECK(toolbar && !toolbar->isVisible());
  const auto move=[&](QWidget *widget,QPoint logical) {
    const auto global=widget->mapToGlobal(logical);
    // Convert through the actual native child, retaining mixed-DPI origins.
    auto *root=widget;
    while(!root->windowHandle())root=root->parentWidget();
    POINT point{qRound(root->mapFromGlobal(global).x()*root->devicePixelRatioF()),
                qRound(root->mapFromGlobal(global).y()*root->devicePixelRatioF())};
    ClientToScreen(reinterpret_cast<HWND>(root->winId()),&point);
    SetCursorPos(point.x,point.y);wait(65);
  };
  auto *surface=window.source_view()->presentation_surface();
  move(surface,{200,200});
  HDRSHOT_CHECK(toolbar->isVisible() && surface->cursor().shape()==Qt::SizeAllCursor);
  auto *grip=window.findChild<QWidget *>("pinWindowGrip");
  auto *copy=window.findChild<QToolButton *>("pinCopy");
  HDRSHOT_CHECK(grip && copy);
  move(grip,grip->rect().center());HDRSHOT_CHECK(grip->underMouse() && GetCursor()==LoadCursorW(nullptr,IDC_SIZEALL));
  move(copy,copy->rect().center());HDRSHOT_CHECK(copy->underMouse() && GetCursor()==LoadCursorW(nullptr,IDC_ARROW));
  HDRSHOT_CHECK(GetForegroundWindow()==peer_hwnd);
  wait(80);
  const auto requested=windows_analysis_presentation_status(*data.presenter).requested;
  for(int i=0;i<8;++i) {move(surface,{180+i*3,170});move(copy,copy->rect().center());}
  HDRSHOT_CHECK(windows_analysis_presentation_status(*data.presenter).requested==requested);
  HDRSHOT_CHECK(GetForegroundWindow()==peer_hwnd);
  const auto folder=qEnvironmentVariable("HDRSHOT_PIN_NATIVE_CAPTURE");
  if(!folder.isEmpty()) {
    QDir().mkpath(folder);
    // Capture the composed desktop, never QWidget::grab/render (which repaints).
    const auto screen=window.screen();const auto origin=screen->geometry().topLeft();
    const auto dpr=screen->devicePixelRatio();
    HDRSHOT_CHECK(GetWindowRect(hwnd,&r));
    auto screenshot=screen->grabWindow(0).toImage().copy(r.left-qRound(origin.x()*dpr),
        r.top-qRound(origin.y()*dpr),r.right-r.left,r.bottom-r.top);
    HDRSHOT_CHECK(!screenshot.isNull());
    HDRSHOT_CHECK(screenshot.save(folder+"/windows-pin-desktop.png"));
    int blue=0;
    const auto at=copy->mapTo(&window,QPoint());
    for(int y=qRound(at.y()*dpr);y<qRound((at.y()+copy->height())*dpr);++y)
      for(int x=qRound(at.x()*dpr);x<qRound((at.x()+copy->width())*dpr);++x) {
        auto c=screenshot.pixelColor(x,y);if(c.blue()>c.red()+40 && c.blue()>c.green()+10)++blue;
      }
    HDRSHOT_CHECK(blue>100);
    std::cout<<"desktop toolbar blue_pixels="<<blue<<'\n';
  }
  SetCursorPos(r.right+20,r.bottom+20);wait(250);HDRSHOT_CHECK(toolbar->isVisible());
  wait(500);HDRSHOT_CHECK(!toolbar->isVisible() && !copy->underMouse());
  HDRSHOT_CHECK(GetForegroundWindow()==peer_hwnd);
  PostMessageW(peer_hwnd,WM_CLOSE,0,0);peer.waitForFinished(3000);
  window.close();
  std::cout<<"OS pointer routing with external process focus passed; not physical mouse/trackpad input\n";
}
bool interactive_desktop() {
  const auto input=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
  if(!input)return false;
  wchar_t input_name[256]{},thread_name[256]{};DWORD bytes{};
  const bool same=GetUserObjectInformationW(input,UOI_NAME,input_name,sizeof(input_name),&bytes) &&
      GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,thread_name,sizeof(thread_name),&bytes) &&
      std::wstring_view(input_name)==thread_name;
  CloseDesktop(input);
  POINT center{GetSystemMetrics(SM_CXSCREEN)/2,GetSystemMetrics(SM_CYSCREEN)/2};
  wchar_t name[128]{};GetClassNameW(WindowFromPoint(center),name,128);
  return same && std::wstring_view(name)!=L"LockScreenBackstopFrame";
}
void decoration_and_preview() {
  auto prepared=prepare_windows_pinned_screenshot(fixture(false));HDRSHOT_CHECK(prepared.has_value());
  auto &data=prepared.value();
  PinnedScreenshotWindow window(data.image.input,{});
  const int width=qEnvironmentVariableIntValue("HDRSHOT_PIN_TEST_WIDTH");
  window.resize(width?width:500,400);
  window.move(QGuiApplication::primaryScreen()->availableGeometry().topLeft()+QPoint(40,40));
  HDRSHOT_CHECK(data.attach_window(window).has_value());
  auto native=make_windows_pinned_window_port();
  window.present_requested=[&](auto view,auto surface){HDRSHOT_CHECK(data.presenter->present(data.image.input,view,surface).has_value());};
  auto *preview=window.preview();
  preview->configure_window=[&](QWidget &card){native->configure_preview(card,window);};
  preview->present=[&](int index,auto view,auto surface){
    return data.auxiliary[std::size_t(index)].presenter->present(data.image.input,view,surface).has_value();
  };
  for(int i=0;i<2;++i) HDRSHOT_CHECK(data.auxiliary[std::size_t(i)].attach(
      preview->host(i),*preview->source(i).presentation_surface(),[&,i]{preview->refresh(i);},
      [](Error){HDRSHOT_CHECK(false);}).has_value());
  window.show();native->set_always_on_top(window,true);native->bring_forward(window);wait(400);
  auto capture=[&](const QString &name) {
    const auto folder=qEnvironmentVariable("HDRSHOT_PIN_NATIVE_CAPTURE");
    if(folder.isEmpty())return;
    QDir().mkpath(folder);
    RECT r{};HDRSHOT_CHECK(GetWindowRect(reinterpret_cast<HWND>(window.winId()),&r));
    QRect bounds(r.left,r.top,r.right-r.left,r.bottom-r.top);
    if(preview->host(1).isVisible()) {
      HDRSHOT_CHECK(GetWindowRect(reinterpret_cast<HWND>(preview->host(1).winId()),&r));
      bounds=bounds.united(QRect(r.left,r.top,r.right-r.left,r.bottom-r.top));
    }
    bounds.adjust(-20,-20,20,20);
    HDRSHOT_CHECK(desktop_region(bounds).save(folder+"/"+name+".png"));
  };
  const auto visible_gradient=[&](QWidget &surface) {
    if(!verify_desktop_pixels)return;
    RECT r{};HDRSHOT_CHECK(GetWindowRect(reinterpret_cast<HWND>(surface.winId()),&r));
    const auto pixels=desktop_region(QRect(r.left,r.top,r.right-r.left,r.bottom-r.top));
    const int left=pixels.pixelColor(pixels.width()/4,pixels.height()/2).red();
    const int right=pixels.pixelColor(3*pixels.width()/4,pixels.height()/2).red();
    std::cout<<"visible_gradient width="<<pixels.width()<<" left="<<left<<" right="<<right<<'\n';
    HDRSHOT_CHECK(right-left>70);
  };
  const auto hwnd=reinterpret_cast<HWND>(window.winId());
  BOOL enabled=FALSE;
  HDRSHOT_CHECK(SUCCEEDED(DwmGetWindowAttribute(hwnd,DWMWA_NCRENDERING_ENABLED,&enabled,sizeof(enabled))));
  HDRSHOT_CHECK(enabled);
  HDRSHOT_CHECK(spin([&]{return windows_analysis_presentation_status(*data.presenter).completed>0;}));
  if(verify_desktop_pixels) {
    // Establish visibility before the initial pixel measurement; another
    // topmost application must not be mistaken for the pin's output.
    native->bring_forward(window);wait(100);
    RECT r{};HDRSHOT_CHECK(GetWindowRect(hwnd,&r));
    HDRSHOT_CHECK(GetAncestor(WindowFromPoint({r.left+3,(r.top+r.bottom)/2}),GA_ROOT)==hwnd);
  }
  RECT expanded_rect{};HDRSHOT_CHECK(GetWindowRect(hwnd,&expanded_rect));
  capture("expanded");
  if(verify_desktop_pixels) {
    RECT r{};HDRSHOT_CHECK(GetWindowRect(hwnd,&r));
    std::cout<<"native_rect="<<r.left<<','<<r.top<<','<<r.right<<','<<r.bottom
             <<" dpr="<<window.devicePixelRatioF()<<" screens="<<QGuiApplication::screens().size()<<'\n';
    const auto desktop=desktop_region(QRect(r.left,(r.top+r.bottom)/2,4,1));
    const auto outer=desktop.pixelColor(0,0),inner=desktop.pixelColor(1,0),content=desktop.pixelColor(3,0);
    std::cout<<"desktop border outer="<<outer.red()<<" inner="<<inner.red()<<" content="<<content.red()<<'\n';
    // With the system-colored Win11 edge suppressed, both alpha rings must
    // composite over the actual image. Adjacent samples differ by <2 codes in
    // this fixed gradient; the bound includes 8-bit display rounding.
    HDRSHOT_CHECK_NEAR(outer.red(),.66*content.red(),6);
    HDRSHOT_CHECK_NEAR(inner.red(),.65*255+.35*content.red(),6);
  }
  HDRSHOT_CHECK(window.property("windowsBorderReady").toBool());
  auto *collapse=window.findChild<QToolButton *>("pinCollapse");HDRSHOT_CHECK(collapse);
  collapse->click();wait(150);
  HDRSHOT_CHECK(window.height()==60);
  HDRSHOT_CHECK(!IsWindowVisible(reinterpret_cast<HWND>(window.source_view()->presentation_surface()->winId())));
  HDRSHOT_CHECK(spin([&]{return windows_analysis_presentation_status(*data.auxiliary[0].presenter).completed>0;}));
  const auto hidden=windows_analysis_presentation_status(*data.presenter).requested;
  wait(150);HDRSHOT_CHECK(windows_analysis_presentation_status(*data.presenter).requested==hidden);
  visible_gradient(*preview->source(0).presentation_surface());
  capture("collapsed");
  const auto focus=GetForegroundWindow();
  QEvent enter(QEvent::Enter);QApplication::sendEvent(collapse,&enter);wait(470);
  auto &card=preview->host(1);HDRSHOT_CHECK(card.isVisible());
  HDRSHOT_CHECK(GetForegroundWindow()==focus);
  HDRSHOT_CHECK(spin([&]{return windows_analysis_presentation_status(*data.auxiliary[1].presenter).completed>0;}));
  visible_gradient(*preview->source(1).presentation_surface());
  capture("preview");
  const auto card_hwnd=reinterpret_cast<HWND>(card.winId());
  HDRSHOT_CHECK(GetWindowLongPtrW(card_hwnd,GWL_EXSTYLE)&WS_EX_NOACTIVATE);
  HDRSHOT_CHECK(GetWindowLongPtrW(card_hwnd,GWL_EXSTYLE)&WS_EX_TRANSPARENT);
  // Actual native child messages must reach the logical button, including the
  // image and badge. Do not invoke clicked() for the expansion regression.
  auto *surface=preview->source(0).presentation_surface();
  const auto thumb=reinterpret_cast<HWND>(surface->winId());
  const auto point=surface->rect().center()*surface->devicePixelRatioF();
  std::vector<QImage> transition;
  std::vector<qint64> frame_times;
  QElapsedTimer recording;recording.start();
  const auto record_frame=[&] {
    if(qEnvironmentVariableIsEmpty("HDRSHOT_PIN_NATIVE_CAPTURE"))return;
    const QRect bounds(expanded_rect.left-12,expanded_rect.top-12,
        expanded_rect.right-expanded_rect.left+24,expanded_rect.bottom-expanded_rect.top+24);
    transition.push_back(desktop_region(bounds));
    frame_times.push_back(recording.elapsed());
  };
  record_frame();
  SendMessageW(thumb,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(point.x(),point.y()));
  wait(20);
  record_frame();
  HDRSHOT_CHECK(!IsWindowVisible(card_hwnd));
  SendMessageW(thumb,WM_LBUTTONUP,0,MAKELPARAM(point.x(),point.y()));
  for(int i=0;i<20;++i) {wait(16);record_frame();}
  for(std::size_t i=0;i<transition.size();++i) {
    HDRSHOT_CHECK(transition[i].save(qEnvironmentVariable("HDRSHOT_PIN_NATIVE_CAPTURE")+
        QString("/transition-%1-%2ms.png").arg(i,2,10,QChar('0')).arg(frame_times[i])));
  }
  HDRSHOT_CHECK(window.height()>60 && IsWindowVisible(reinterpret_cast<HWND>(window.source_view()->presentation_surface()->winId())));
  HDRSHOT_CHECK(!card.isVisible());
  const auto stopped=windows_analysis_presentation_status(*data.auxiliary[1].presenter).requested;
  wait(400);HDRSHOT_CHECK(windows_analysis_presentation_status(*data.auxiliary[1].presenter).requested==stopped);
  // Check image center/edge, button whitespace and native badge at the actual
  // HWND boundary. Waiting and already-visible preview paths both participate.
  const std::array<QPoint,4> points{{QPoint(15,15),QPoint(4,18),QPoint(1,1),QPoint(31,31)}};
  for(std::size_t i=0;i<points.size();++i) {
    collapse->click();wait(80);HDRSHOT_CHECK(window.height()==60);
    QApplication::sendEvent(collapse,&enter);wait(i%2?400:50);
    QWidget *receiver=i<2?surface:(i==3?window.findChild<QWidget *>("pinExpandBadge"):collapse);
    HDRSHOT_CHECK(receiver);
    while(!receiver->windowHandle())receiver=receiver->parentWidget();
    const auto local=receiver->mapFromGlobal(collapse->mapToGlobal(points[i]))*receiver->devicePixelRatioF();
    const auto target=reinterpret_cast<HWND>(receiver->winId());
    SendMessageW(target,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(local.x(),local.y()));wait(15);
    HDRSHOT_CHECK(!IsWindowVisible(card_hwnd));
    SendMessageW(target,WM_LBUTTONUP,0,MAKELPARAM(local.x(),local.y()));wait(100);
    HDRSHOT_CHECK(window.height()>60);
  }
  collapse->click();wait(80);
  SendMessageW(thumb,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(point.x(),point.y()));wait(15);
  SendMessageW(thumb,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(-100,-100));wait(15);
  SendMessageW(thumb,WM_LBUTTONUP,0,MAKELPARAM(-100,-100));wait(400);
  HDRSHOT_CHECK(window.height()==60 && !card.isVisible());
  collapse->click();wait(100);
  // Rebinding actual attached monitors must preserve native presentation and
  // the collapsed window extent. This does not simulate changing system DPI.
  for(auto *screen:QGuiApplication::screens()) {
    collapse->click();wait(40);
    window.move(screen->availableGeometry().topLeft()+QPoint(40,40));wait(120);
    collapse->click();
    HDRSHOT_CHECK(spin([&]{auto status=windows_analysis_presentation_status(*data.presenter);
      return status.completed_generation==status.requested && !status.worker_active && !status.pending;}));
    HDRSHOT_CHECK(window.property("windowsBorderReady").toBool());
  }
  window.close();wait(50);
}
void cross_dpi() {
  auto screens=QGuiApplication::screens();
  std::sort(screens.begin(),screens.end(),[](auto *a,auto *b){return a->devicePixelRatio()<b->devicePixelRatio();});
  HDRSHOT_CHECK(screens.size()>1 && screens.front()->devicePixelRatio()!=screens.back()->devicePixelRatio());
  for(auto *s:screens)std::cout<<"screen="<<s->name().toStdString()<<" dpr="<<s->devicePixelRatio()
      <<" geometry="<<s->geometry().x()<<','<<s->geometry().y()<<','<<s->geometry().width()<<','<<s->geometry().height()<<'\n';
  auto prepared=prepare_windows_pinned_screenshot(fixture(false));HDRSHOT_CHECK(prepared.has_value());
  auto &data=prepared.value();PinnedScreenshotWindow window(data.image.input,{});
  window.resize(500,400);window.move(screens.front()->availableGeometry().topLeft()+QPoint(60,80));
  auto native=make_windows_pinned_window_port();
  std::array<analysis::SourceView,3> frames{};
  window.present_requested=[&](auto view,auto surface){frames[0]=view;HDRSHOT_CHECK(data.presenter->present(data.image.input,view,surface).has_value());};
  HDRSHOT_CHECK(data.attach_window(window).has_value());
  auto *preview=window.preview();
  preview->configure_window=[&](QWidget &card){native->configure_preview(card,window);};
  preview->present=[&](int i,auto view,auto surface){frames[std::size_t(i+1)]=view;return data.auxiliary[std::size_t(i)].presenter->present(data.image.input,view,surface).has_value();};
  for(int i=0;i<2;++i)HDRSHOT_CHECK(data.auxiliary[std::size_t(i)].attach(preview->host(i),
      *preview->source(i).presentation_surface(),[&,i]{preview->refresh(i);},[](Error){HDRSHOT_CHECK(false);}).has_value());
  window.show();native->set_always_on_top(window,true);native->bring_forward(window);wait(500);
  const auto inspect=[&](int index,AnalyzerSourceView &source,const char *stage) {
    wait(250);
    auto *surface=source.presentation_surface();const auto hwnd=reinterpret_cast<HWND>(surface->winId());
    RECT r{};HDRSHOT_CHECK(GetClientRect(hwnd,&r));POINT origin{};HDRSHOT_CHECK(ClientToScreen(hwnd,&origin));
    const auto &frame=frames[std::size_t(index)];
    std::cout<<stage<<" index="<<index<<" widget_dpr="<<source.devicePixelRatioF()
        <<" surface_dpr="<<surface->devicePixelRatioF()<<" qwindow_dpr="<<surface->windowHandle()->devicePixelRatio()
        <<" native_dpi="<<GetDpiForWindow(hwnd)<<" logical="<<source.width()<<'x'<<source.height()
        <<" native="<<r.right<<'x'<<r.bottom<<" frame="<<frame.target_size.width<<'x'<<frame.target_size.height
        <<" scale="<<frame.scale<<" offset="<<frame.offset_x<<','<<frame.offset_y<<'\n';
    for(auto node=hwnd;node;node=GetAncestor(node,GA_PARENT)) {
      const auto region=CreateRectRgn(0,0,0,0);RECT bounds{};const int kind=GetWindowRgn(node,region);
      if(kind!=ERROR) {GetRgnBox(region,&bounds);std::cout<<"native_clip="<<bounds.left<<','<<bounds.top<<','<<bounds.right<<','<<bounds.bottom<<" hwnd="<<node<<'\n';}
      DeleteObject(region);if(node==reinterpret_cast<HWND>(source.window()->winId()))break;
    }
    HDRSHOT_CHECK(frame.target_size.width==r.right && frame.target_size.height==r.bottom);
    const auto pixels=desktop_region(QRect(origin.x,origin.y,r.right,r.bottom));
    const auto folder=qEnvironmentVariable("HDRSHOT_PIN_NATIVE_CAPTURE");
    if(!folder.isEmpty()) {QDir().mkpath(folder);HDRSHOT_CHECK(pixels.save(folder+QString("/%1-%2-%3.png").arg(stage).arg(index).arg(source.devicePixelRatioF())));}
    const auto image=source.image_rect();
    const double sx=double(r.right)/source.width(),sy=double(r.bottom)/source.height();
    for(double y:{.25,.5,.75}) {
      if(index==1 && y!=.25)continue; // The intentional expand badge overlaps the lower-right thumbnail.
      const auto left=pixels.pixelColor(qRound((image.x()+image.width()*.25)*sx),qRound((image.y()+image.height()*y)*sy));
      const auto right=pixels.pixelColor(qRound((image.x()+image.width()*.75)*sx),qRound((image.y()+image.height()*y)*sy));
      std::cout<<"pixels y="<<y<<" left="<<left.red()<<" right="<<right.red()<<'\n';
      HDRSHOT_CHECK(right.red()-left.red()>65);
    }
  };
  auto *collapse=window.findChild<QToolButton *>("pinCollapse");HDRSHOT_CHECK(collapse);
  inspect(0,*window.source_view(),"initial");
  for(int cycle=0;cycle<2;++cycle)for(auto *screen:{screens.back(),screens.front()}) {
    if(cycle==0) {
      window.move(screen->availableGeometry().topLeft()+QPoint(60,80));wait(250);
      inspect(0,*window.source_view(),"expanded move");collapse->click();
    } else {
      collapse->click();window.move(screen->availableGeometry().topLeft()+QPoint(60,80));wait(250);
    }
    inspect(1,preview->source(0),"collapsed move");
    QEvent enter(QEvent::Enter);QApplication::sendEvent(collapse,&enter);wait(600);
    HDRSHOT_CHECK(preview->host(1).isVisible());inspect(2,preview->source(1),"preview after move");
    collapse->click();inspect(0,*window.source_view(),"expanded again");
  }
  window.close();wait(30);
}
}
int main(int argc,char **argv) {
  std::cout<<std::unitbuf;std::cerr<<std::unitbuf;
  if(argc>1 && std::string_view(argv[1])=="--focus-peer") {
    const auto hwnd=CreateWindowExW(0,L"STATIC",L"SeriousShot pin test focus peer",
        WS_OVERLAPPEDWINDOW|WS_VISIBLE,900,200,240,160,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    const auto until=GetTickCount64()+20000;
    MSG msg{};
    while(IsWindow(hwnd)&&GetTickCount64()<until) {
      while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
      Sleep(5);
    }
    if(IsWindow(hwnd))DestroyWindow(hwnd);
    return 0;
  }
  QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
  if(argc>1 && std::string_view(argv[1])=="--cross-dpi") {
    auto screens=QGuiApplication::screens();
    if(!interactive_desktop() || screens.size()<2 || std::all_of(screens.begin(),screens.end(),[&](auto *s){return s->devicePixelRatio()==screens.front()->devicePixelRatio();})) {
      std::cout<<"SKIP: cross-DPI requires an unlocked desktop with differently scaled monitors\n";return 77;
    }
    return hdrshot::test::run({{"Windows actual mixed-DPI pin surfaces and FOV",cross_dpi}});
  }
  if(qEnvironmentVariable("HDRSHOT_PIN_TEST_THEME")=="light")
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
  if(argc>1 && std::string_view(argv[1])=="--preview")
    return hdrshot::test::run({{"Windows decoration collapse and native preview",decoration_and_preview}});
  if(argc>1 && std::string_view(argv[1])=="--visual") {
    if(!interactive_desktop()) {std::cout<<"SKIP: no interactive Windows desktop/input queue\n";return 77;}
    verify_desktop_pixels=true;
    std::vector<hdrshot::test::TestCase> cases{{"Windows composed decoration and native preview",decoration_and_preview}};
    const auto screens=QGuiApplication::screens();
    if(screens.size()>1 && std::any_of(screens.begin(),screens.end(),[&](auto *s){return s->devicePixelRatio()!=screens.front()->devicePixelRatio();}))
      cases.emplace_back("Windows actual mixed-DPI pin surfaces and FOV",cross_dpi);
    else
      std::cout<<"[NOT VERIFIED] Actual mixed-DPI migration requires differently scaled monitors; fixed-DPR cases do not substitute.\n";
    return hdrshot::test::run(cases);
  }
  if(argc>1 && std::string_view(argv[1])=="--idle")
    return hdrshot::test::run({{"Windows native passive hover and idle",hover_and_idle}});
  if(argc>1 && std::string_view(argv[1])=="--desktop") {
    if(!interactive_desktop()) {std::cout<<"SKIP: no interactive Windows desktop/input queue\n";return 77;}
    return hdrshot::test::run({{"Windows composed desktop passive hover",desktop_hover}});
  }
  return hdrshot::test::run({{"Windows pinned content and HWND",native_content},
                            {"Windows decoration collapse and native preview",decoration_and_preview}});
}
