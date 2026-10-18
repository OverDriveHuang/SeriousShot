#include "application/capture_interaction_session.hpp"
#include "platform/windows/windows_d3d_presenter.hpp"
#include "platform/windows/windows_gpu_source.hpp"
#include "platform/windows/windows_qt_ports.hpp"
#include "test_support.hpp"
#include "ui/qt/qt_overlay_host.hpp"

#include <QApplication>
#include <QBackingStore>
#include <QDir>
#include <QElapsedTimer>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QScreen>
#include <QToolButton>
#include <QWindow>
#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <windows.h>

using namespace hdrshot;
namespace {

struct EventTrace final : QObject {
  std::vector<std::string> events;
  bool eventFilter(QObject *object, QEvent *event) override {
    const auto kind = event->type();
    if (kind != QEvent::WindowActivate && kind != QEvent::WindowDeactivate &&
        kind != QEvent::ApplicationDeactivate && kind != QEvent::UngrabMouse)
      return false;
    const char *name = kind == QEvent::WindowActivate     ? "WindowActivate"
                       : kind == QEvent::WindowDeactivate ? "WindowDeactivate"
                       : kind == QEvent::ApplicationDeactivate
                           ? "ApplicationDeactivate"
                           : "UngrabMouse";
    events.emplace_back(std::string(object->objectName().toStdString()) + ':' +
                        name);
    return false;
  }
  void print() const {
    for (const auto &event : events)
      std::cerr << "  Qt " << event << '\n';
  }
};

struct NativeMessageTrace {
  struct Entry {
    HWND hwnd{};
    UINT message{};
    WPARAM wparam{};
    LPARAM lparam{};
  };
  static thread_local NativeMessageTrace *current;
  HHOOK hook{};
  std::vector<Entry> events;
  std::size_t printed{};

  NativeMessageTrace() {
    current = this;
    hook = SetWindowsHookExW(WH_CALLWNDPROC, procedure, nullptr,
                             GetCurrentThreadId());
    HDRSHOT_CHECK(hook != nullptr);
  }
  ~NativeMessageTrace() {
    UnhookWindowsHookEx(hook);
    current = nullptr;
  }
  static LRESULT CALLBACK procedure(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && current) {
      const auto *message = reinterpret_cast<const CWPSTRUCT *>(lparam);
      switch (message->message) {
      case WM_ACTIVATE:
      case WM_SETFOCUS:
      case WM_KILLFOCUS:
      case WM_CAPTURECHANGED:
      case WM_ACTIVATEAPP:
        if (current->events.size() < 512)
          current->events.push_back({message->hwnd, message->message,
                                     message->wParam, message->lParam});
        break;
      default:
        break;
      }
    }
    return CallNextHookEx(current ? current->hook : nullptr, code, wparam,
                          lparam);
  }
  void print_new() {
    while (printed < events.size()) {
      const auto &event = events[printed++];
      const char *name = event.message == WM_ACTIVATE    ? "WM_ACTIVATE"
                         : event.message == WM_SETFOCUS  ? "WM_SETFOCUS"
                         : event.message == WM_KILLFOCUS ? "WM_KILLFOCUS"
                         : event.message == WM_CAPTURECHANGED
                             ? "WM_CAPTURECHANGED"
                             : "WM_ACTIVATEAPP";
      wchar_t class_name[128]{};
      GetClassNameW(event.hwnd, class_name, 128);
      std::wcerr << L"  Win32 " << name << L" hwnd=" << event.hwnd << L" class="
                 << class_name << L" wparam=0x" << std::hex << event.wparam
                 << L" lparam=0x" << event.lparam << std::dec << L'\n';
    }
  }
};
thread_local NativeMessageTrace *NativeMessageTrace::current = nullptr;

struct Presenter final : PreviewPresenterPort {
  explicit Presenter(std::shared_ptr<WindowsD3DPreviewPresenter> actual)
      : actual(std::move(actual)) {}
  std::shared_ptr<WindowsD3DPreviewPresenter> actual;
  std::shared_ptr<std::atomic<int>> completed =
      std::make_shared<std::atomic<int>>(0);
  std::shared_ptr<std::atomic<bool>> failed =
      std::make_shared<std::atomic<bool>>(false);
  std::optional<PresentPreviewRequest> last;
  void cancel(SessionId session, OperationId operation) override {
    actual->cancel(session, operation);
  }
  void present(const PresentPreviewRequest &request, Completion done) override {
    last = request;
    actual->present(request, [completed = completed, failed = failed,
                              done = std::move(done)](auto result) mutable {
      // The real presenter intentionally coalesces pending pointer previews.
      // Superseded requests are cancelled; readiness requires an actual frame.
      if (!result && result.error().code != ErrorCode::operation_cancelled) {
        std::cerr << "native fixture presentation failure "
                  << to_string(result.error().code) << '\n';
        failed->store(true);
      }
      if (result) completed->fetch_add(1);
      done(std::move(result));
    });
  }
};

void trace_hit(const char *step, POINT point, HWND underlay, HWND host) {
  const HWND hit = WindowFromPoint(point);
  wchar_t class_name[128]{};
  RECT bounds{};
  if (hit) {
    GetClassNameW(hit, class_name, 128);
    GetWindowRect(hit, &bounds);
  }
  std::wcerr << step << L" point=" << point.x << L',' << point.y << L" hit="
             << hit << L" class=" << class_name << L" rect=" << bounds.left
             << L',' << bounds.top << L',' << bounds.right << L','
             << bounds.bottom << L" visible=" << IsWindowVisible(hit)
             << L" underlay=" << underlay << L" host=" << host << L'\n';
}

struct CursorRestore {
  POINT saved{};
  HWND foreground{};
  bool left_down{};
  bool real_input{};
  explicit CursorRestore(bool real)
      : foreground(GetForegroundWindow()), real_input(real) {
    GetCursorPos(&saved);
  }
  ~CursorRestore() {
    if (left_down) {
      if (real_input) {
        INPUT release{};
        release.type = INPUT_MOUSE;
        release.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &release, sizeof(release));
      } else {
        ReleaseCapture();
      }
    }
    SetCursorPos(saved.x, saved.y);
    if (foreground && IsWindow(foreground))
      SetForegroundWindow(foreground);
  }
};

bool wait_until(const std::function<bool()> &condition, int timeout_ms = 1500) {
  QElapsedTimer clock;
  clock.start();
  while (clock.elapsed() < timeout_ms) {
    QApplication::processEvents(QEventLoop::AllEvents, 20);
    if (condition())
      return true;
    Sleep(5);
  }
  return condition();
}

void settle_visual_events() {
  QElapsedTimer clock;
  clock.start();
  while (clock.elapsed() < 300) {
    QApplication::processEvents(QEventLoop::AllEvents, 20);
    Sleep(5);
  }
}

std::vector<bool> text_ink(const QImage& image) {
  HDRSHOT_CHECK(!image.isNull());
  // Each ROI contains one right-aligned scalar, on an opaque uniform panel.
  // Compare glyph coverage, allowing SDR/HDR composition to change color.
  const int background = qGray(image.pixel(0, 0));
  std::vector<bool> mask;
  mask.reserve(std::size_t(image.width()) * image.height());
  for (int y = 0; y < image.height(); ++y)
    for (int x = 0; x < image.width(); ++x)
      mask.push_back(std::abs(qGray(image.pixel(x, y)) - background) > 55);
  return mask;
}

// Capture the composed desktop only. Do not ask any QWidget to paint or flush
// its backing store: that would conceal the historical native-child defect.
QImage desktop_pixels(const RECT& monitor) {
  const int width = monitor.right - monitor.left;
  const int height = monitor.bottom - monitor.top;
  HDC screen = GetDC(nullptr);
  HDRSHOT_CHECK(screen);
  HDC memory = CreateCompatibleDC(screen);
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits{};
  HBITMAP bitmap = memory ? CreateDIBSection(screen, &info, DIB_RGB_COLORS,
                                            &bits, nullptr, 0) : nullptr;
  HGDIOBJ previous = bitmap ? SelectObject(memory, bitmap) : nullptr;
  const bool captured = previous && previous != HGDI_ERROR &&
      BitBlt(memory, 0, 0, width, height, screen, monitor.left, monitor.top,
             SRCCOPY | CAPTUREBLT) && GdiFlush();
  QImage pixels;
  if (captured)
    pixels = QImage(static_cast<const uchar*>(bits), width, height, width * 4,
                    QImage::Format_RGB32).copy();
  if (previous && previous != HGDI_ERROR) SelectObject(memory, previous);
  if (bitmap) DeleteObject(bitmap);
  if (memory) DeleteDC(memory);
  ReleaseDC(nullptr, screen);
  HDRSHOT_CHECK(captured && !pixels.isNull());
  return pixels;
}

void actual_screen_magnifier_readings(bool native_control = false,
                                     bool gamut = false, bool gpu_card = false) {
  CursorRestore restore_cursor(false);
  auto* screen = QGuiApplication::primaryScreen();
  HDRSHOT_CHECK(screen);
  const auto available = screen->availableGeometry();
  const double scale = screen->devicePixelRatio();
  MONITORINFO monitor{sizeof(monitor)};
  HDRSHOT_CHECK(GetMonitorInfoW(
      MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor));
  constexpr int width = 420, height = 600;
  const int x = available.left() + 20, y = available.top() + 20;
  WindowsDisplayInfo display;
  display.snapshot.id = {17};
  display.snapshot.desktop_frame_points = {double(x), double(y), width, height};
  display.snapshot.point_pixel_scale = scale;
  display.snapshot.capture_size_px = {qRound(width * scale), qRound(height * scale)};
  display.physical_x = monitor.rcMonitor.left +
      qRound((x - screen->geometry().left()) * scale);
  display.physical_y = monitor.rcMonitor.top +
      qRound((y - screen->geometry().top()) * scale);
  WindowsQtInputPlatformAdapter input;
  auto native = std::make_unique<WindowsQtOverlayWindow>(display);
  auto* window = native.get();
  QtOverlayHost host(std::move(native), {17}, SelectionSnapshot{1, {}}, input);
  host.setGeometry(x, y, width, height);
  host.prepare_hidden_native_surface();
  host.windowHandle()->setScreen(screen);
  OverlayEditorWidget editor(display.snapshot.capture_size_px, scale, nullptr,
                             "", input, SelectionSnapshot{1, {}}, &host);
  auto* toolbar = editor.findChild<QFrame*>("editorToolbar");
  auto* property = editor.findChild<QFrame*>("propertyBar");
  HDRSHOT_CHECK(toolbar && property);
  if (native_control) {
    // Diagnostic A/B control: reproduce the former child-HWND topology without
    // changing production code. A failure must come from desktop glyphs.
    editor.setAttribute(Qt::WA_NativeWindow);
    toolbar->setAttribute(Qt::WA_NativeWindow);
    property->setAttribute(Qt::WA_NativeWindow);
  }
  editor.setGeometry(host.rect());
  CaptureMagnifierPanel panel(&editor);
  MagnifierReadings fields;
  // A shorter image area keeps all four new groups visible together, including
  // wrapped Windows headings; production geometry is not changed by this test.
  const int image_height = gamut ? (gpu_card ? 40 : 32) : 216;
  if (gamut) {
    fields.selected.fill(false);
    for (int id : {9, 10, 11, 12}) fields.selected[id] = true;
  }
  panel.setGeometry(30, 30, 216,
                    panel.header_height() + image_height + panel.text_height(fields, 216));
  editor.set_magnifier_exclusion(panel.geometry());
  editor.show();
  window->configure(host, true);
  // Test only: actual desktop capture must include our owned transparent host.
  HDRSHOT_CHECK(SetWindowDisplayAffinity(reinterpret_cast<HWND>(host.winId()), WDA_NONE));
  HDRSHOT_CHECK(SetWindowDisplayAffinity(static_cast<HWND>(host.native_surface()), WDA_NONE));

  std::shared_ptr<WindowsD3DPreviewPresenter> card_presenter;
  FrozenDesktopRef card_source;
  QImage last_ui;
  std::uint64_t card_revision=0;
  if(gpu_card) {
    display.source_white={false,80};display.advanced_color_mode=1;
    auto created=WindowsD3DPreviewPresenter::create(host.native_surface(),display,{},host.native_magnifier_surface());
    HDRSHOT_CHECK(created.has_value());card_presenter=std::move(created.value());card_presenter->invalidate_card(1);
    auto frozen=std::make_shared<FrozenDesktop>();frozen->frame_id={91};frozen->display_generation=1;
    CanonicalFrameSegment segment;segment.display_id=display.snapshot.id;
    segment.size_px=display.snapshot.capture_size_px;segment.encoding=WindowsColor::linear_p3_encoding();
    segment.rgba_half.assign(std::size_t(segment.size_px.width)*segment.size_px.height*4,0x3800);
    for(std::size_t i=3;i<segment.rgba_half.size();i+=4)segment.rgba_half[i]=0x3c00;
    frozen->canonical_segments.push_back(std::move(segment));card_source=std::move(frozen);
    HDRSHOT_CHECK(SetWindowDisplayAffinity(static_cast<HWND>(host.native_magnifier_surface()),WDA_NONE));
  }
  const auto display_sample=[&](PixelPoint point,const MagnifierReadout& reading,const MagnifierReadings& selected,int image,int zoom) {
    panel.show_sample(point,reading,selected,image,zoom,!gpu_card);
    if(!gpu_card)return;
    window->move_magnifier(host,panel.geometry());
    last_ui=QImage(qRound(panel.width()*scale),qRound(panel.height()*scale),QImage::Format_RGBA8888);
    last_ui.setDevicePixelRatio(scale);last_ui.fill(Qt::transparent);
    panel.render(&last_ui); // Production UI-texture stimulus; never in observer.
    last_ui=last_ui.convertToFormat(QImage::Format_RGBA8888);
    MagnifierCardFrame frame{{1},card_source,display.snapshot.id,1,1,++card_revision,
        {last_ui.width(),last_ui.height()},
        {true,point,{0,qRound(panel.header_height()*scale),
         magnifier_odd_pixels(last_ui.width(),zoom)*zoom,
         magnifier_odd_pixels(qRound(image*scale),zoom)*zoom},zoom,card_revision},{}};
    frame.ui_rgba.resize(std::size_t(last_ui.width())*last_ui.height()*4);
    for(int row=0;row<last_ui.height();++row)std::copy_n(last_ui.constScanLine(row),std::size_t(last_ui.width())*4,
        frame.ui_rgba.data()+std::size_t(row)*last_ui.width()*4);
    auto complete=std::make_shared<std::atomic<bool>>(false),failed=std::make_shared<std::atomic<bool>>(false);
    card_presenter->present_card(std::move(frame),[complete,failed](auto result){*failed=!result;*complete=true;});
    HDRSHOT_CHECK(wait_until([&]{return complete->load();}));HDRSHOT_CHECK(!*failed);
    window->show_magnifier();
  };
  const QString output = qEnvironmentVariable("HDRSHOT_MAGNIFIER_SCREEN_OUTPUT");
  if (!output.isEmpty()) HDRSHOT_CHECK(QDir().mkpath(output));
  std::cerr << "screen fixture gamut=" << gamut << " nativeControl=" << native_control
            << " dpr=" << scale << " available=" << available.width() << 'x' << available.height()
            << " physical=" << monitor.rcMonitor.right - monitor.rcMonitor.left << 'x'
            << monitor.rcMonitor.bottom - monitor.rcMonitor.top << " panel="
            << panel.width() << 'x' << panel.height() << " output=" << output.toStdString() << '\n';
  auto verify = [&](const QString& stage, const std::vector<QString>& expected) {
    settle_visual_events();
    auto labels = panel.findChildren<QLabel*>("magnifierScalarValue");
    HDRSHOT_CHECK(labels.size() == qsizetype(expected.size()));
    for (qsizetype i = 0; i < labels.size(); ++i)
      HDRSHOT_CHECK(labels[i]->text() == expected[std::size_t(i)]);
    if (gamut) {
      QStringList selected_headings;
      for (std::size_t position : {2u, 4u, 6u, 7u})
        if (fields.selected[magnifier_reading_order[position]])
          selected_headings << magnifier_reading_names()[int(position)];
      const auto headings = panel.findChildren<QLabel*>("magnifierGroupHeading");
      HDRSHOT_CHECK(headings.size() == selected_headings.size());
      int previous_bottom = -1;
      for (qsizetype i = 0; i < headings.size(); ++i) {
        HDRSHOT_CHECK(headings[i]->text() == selected_headings[i]);
        HDRSHOT_CHECK(headings[i]->y() > previous_bottom);
        HDRSHOT_CHECK(headings[i]->height() >= headings[i]->heightForWidth(headings[i]->width()));
        previous_bottom = headings[i]->geometry().bottom();
        labels.push_back(headings[i]);
      }
    }
    const QImage* image=nullptr;
    if(gpu_card) image=&last_ui;
    else {
      auto* store=panel.backingStore();HDRSHOT_CHECK(store);
      image=dynamic_cast<const QImage*>(store->paintDevice());
    }
    HDRSHOT_CHECK(image && !image->isNull());
    // Both references are read-only. The GPU case uses the texture submitted
    // by the stimulus, never an observation-time widget render/repaint.
    std::vector<QImage> backing;
    for(qsizetype i=0;i<labels.size();++i) {
      const QPoint offset=labels[i]->mapTo(gpu_card?static_cast<QWidget*>(&panel):static_cast<QWidget*>(&host),QPoint{});
      const QRect roi(qRound(offset.x()*scale),qRound(offset.y()*scale),qRound(labels[i]->width()*scale),qRound(labels[i]->height()*scale));
      HDRSHOT_CHECK(image->rect().contains(roi));backing.push_back(image->copy(roi));
    }
    const QImage desktop = (gamut || gpu_card) ? desktop_pixels(monitor.rcMonitor)
                                : screen->grabWindow(0).toImage();
    HDRSHOT_CHECK(!desktop.isNull());
    HDRSHOT_CHECK(desktop.width() == monitor.rcMonitor.right - monitor.rcMonitor.left);
    HDRSHOT_CHECK(desktop.height() == monitor.rcMonitor.bottom - monitor.rcMonitor.top);
    POINT host_origin{};
    HDRSHOT_CHECK(ClientToScreen(gpu_card?static_cast<HWND>(host.native_magnifier_surface()):reinterpret_cast<HWND>(host.winId()), &host_origin));
    if (gamut) {
      const QPoint origin = gpu_card?QPoint{}:panel.mapTo(&host, QPoint{});
      const QRect panel_roi(host_origin.x - monitor.rcMonitor.left + qRound(origin.x() * scale),
                            host_origin.y - monitor.rcMonitor.top + qRound(origin.y() * scale),
                            qRound(panel.width() * scale), qRound(panel.height() * scale));
      HDRSHOT_CHECK(desktop.rect().contains(panel_roi));
      HDRSHOT_CHECK(panel.geometry().bottom() < host.height());
      if (!output.isEmpty())
        HDRSHOT_CHECK(desktop.copy(panel_roi).save(QDir(output).filePath(stage + "-panel-screen.png")));
    }
    for (qsizetype i = 0; i < labels.size(); ++i) {
      const QPoint offset = labels[i]->mapTo(gpu_card?static_cast<QWidget*>(&panel):static_cast<QWidget*>(&host), QPoint{});
      const QRect roi(host_origin.x - monitor.rcMonitor.left + qRound(offset.x() * scale),
                      host_origin.y - monitor.rcMonitor.top + qRound(offset.y() * scale),
                      backing[std::size_t(i)].width(), backing[std::size_t(i)].height());
      HDRSHOT_CHECK(desktop.rect().contains(roi));
      const QImage actual = desktop.copy(roi);
      if (!output.isEmpty()) {
        const QString prefix = QDir(output).filePath(QString("%1-row%2").arg(stage).arg(i));
        HDRSHOT_CHECK(backing[std::size_t(i)].save(prefix + "-backing.png"));
        HDRSHOT_CHECK(actual.save(prefix + "-screen.png"));
      }
      const auto expected_ink = text_ink(backing[std::size_t(i)]);
      const auto actual_ink = text_ink(actual);
      HDRSHOT_CHECK(expected_ink.size() == actual_ink.size());
      int expected_count = 0, actual_count = 0, different = 0;
      for (std::size_t pixel = 0; pixel < expected_ink.size(); ++pixel) {
        expected_count += expected_ink[pixel];
        actual_count += actual_ink[pixel];
        different += expected_ink[pixel] != actual_ink[pixel];
      }
      std::cerr << "actual screen " << stage.toStdString() << " row=" << i
                << " value=" << labels[i]->text().toStdString()
                << " backingInk=" << expected_count << " screenInk=" << actual_count
                << " different=" << different << '\n';
      HDRSHOT_CHECK(expected_count >= 8);
      HDRSHOT_CHECK(different <= std::max(3, expected_count * 12 / 100));
    }
  };
  if (gamut) {
    // Lock the full menu/float order to the confirmed contract independently
    // of the production order array, then inspect every new group on screen.
    constexpr std::array<std::size_t, 13> order{0,1,9,2,10,3,11,12,4,5,6,7,8};
    HDRSHOT_CHECK(magnifier_reading_order == order);
    const auto exercise = [&](const QString& selection) {
      panel.setFixedHeight(panel.header_height() + image_height + panel.text_height(fields, 216));
      editor.set_magnifier_exclusion(panel.geometry());
      const std::array<MagnifierReadout, 4> samples{
          MagnifierReadout{}, magnifier_readout({.5,.5,.5},203,false),
          magnifier_readout({.75,.75,.75},203,false),
          magnifier_readout({2,.5,.25},203,false)};
      const std::array<QString, 4> stages{"placeholder", "half", "three-quarters", "hdr"};
      for (std::size_t stage = 0; stage < samples.size(); ++stage) {
        std::vector<QString> expected;
        for (const auto& row : magnifier_scalar_rows(samples[stage], fields))
          if (!row.heading) expected.push_back(row.value);
        display_sample({100,100},samples[stage],fields,image_height,8);
        verify(selection + '-' + stages[stage], expected);
      }
    };
    exercise("all-new");
    for (int id : {9,10,11,12}) {
      fields.selected.fill(false);
      fields.selected[id] = true;
      exercise("id" + QString::number(id));
    }
    if (!native_control) {
      HDRSHOT_CHECK(!editor.testAttribute(Qt::WA_NativeWindow));
      HDRSHOT_CHECK(!toolbar->testAttribute(Qt::WA_NativeWindow));
      HDRSHOT_CHECK(!property->testAttribute(Qt::WA_NativeWindow));
    }
    return;
  }
  const auto verify_default = [&](const char* stage, const QString& source,
                                  const QString& nit) {
    verify(stage, {source, source, source, nit, nit, nit, nit});
  };
  // The GPU/sampler path is covered separately by the two-host tests. This
  // case isolates delivery to the real Windows desktop, without mouse input.
  display_sample({100, 100}, {}, fields, image_height, 8);
  verify_default("placeholder", "--", "--");
  display_sample({100, 100}, magnifier_readout({.5, .5, .5}, 203, false),
                    fields, image_height, 8);
  verify_default("half", "0.5000", "101.50");
  display_sample({100, 100}, magnifier_readout({.75, .75, .75}, 203, false),
                    fields, image_height, 8);
  verify_default("three-quarters", "0.7500", "152.25");
  if (!native_control) {
    HDRSHOT_CHECK(!editor.testAttribute(Qt::WA_NativeWindow));
    HDRSHOT_CHECK(!toolbar->testAttribute(Qt::WA_NativeWindow));
    HDRSHOT_CHECK(!property->testAttribute(Qt::WA_NativeWindow));
  }
}

void check_magnifier_values(QtOverlayHost& host, const char* step,
                            const QString& source_value, const QString& nit_value) {
  // Independent exact gray fixtures: 0.25/0.5/0.75 EDR * default 203 nit.
  // Assert the actual scalar widgets, not only delivery or native visibility.
  const std::array<QString,7> expected{source_value,source_value,source_value,
      nit_value,nit_value,nit_value,nit_value};
  const auto correct=[&] {
    if(!host.magnifier_preview().visible)return false;
    const auto actual=host.findChildren<QLabel*>("magnifierScalarValue");
    if(actual.size()!=qsizetype(expected.size()))return false;
    for(qsizetype i=0;i<actual.size();++i)
      if(actual[i]->text()!=expected[std::size_t(i)])return false;
    return true;
  };
  const bool complete=wait_until(correct);
  const auto counts=host.magnifier_counters();
  std::cerr<<step<<" point="<<host.magnifier_preview().center.x<<','
      <<host.magnifier_preview().center.y<<" revision="<<host.magnifier_preview().revision
      <<" requested="<<counts.requested<<" readbacks="<<counts.readbacks
      <<" delivered="<<counts.delivered<<" inFlight="<<counts.in_flight
      <<" pending="<<counts.pending<<" actual=";
  for(const auto* value:host.findChildren<QLabel*>("magnifierScalarValue"))
    std::cerr<<value->text().toStdString()<<' ';
  std::cerr<<'\n';
  HDRSHOT_CHECK(complete);
}

POINT physical_point(const QWidget &widget, QPoint local) {
  const QPoint global = widget.mapToGlobal(local);
  const double scale = widget.devicePixelRatioF();
  return {qRound(global.x() * scale), qRound(global.y() * scale)};
}

bool send_mouse(POINT point, DWORD flags) {
  const auto virtual_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const auto virtual_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const auto virtual_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const auto virtual_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dx =
      static_cast<LONG>((static_cast<long long>(point.x - virtual_x) * 65535) /
                        std::max(1, virtual_width - 1));
  input.mi.dy =
      static_cast<LONG>((static_cast<long long>(point.y - virtual_y) * 65535) /
                        std::max(1, virtual_height - 1));
  input.mi.dwFlags =
      MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE | flags;
  return SendInput(1, &input, sizeof(input)) == 1;
}

bool send_key(WORD key, bool release) {
  INPUT input{};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = key;
  input.ki.dwFlags = release ? KEYEVENTF_KEYUP : 0;
  return SendInput(1, &input, sizeof(input)) == 1;
}

void trace_state(const char *step, const EventTrace &trace,
                 NativeMessageTrace &native_trace,
                 const OverlayInputRecovery &recovery) {
  const auto *active = QApplication::activeWindow();
  const auto *focus_widget = QApplication::focusWidget();
  std::cerr << step << " capture=" << GetCapture()
            << " foreground=" << GetForegroundWindow()
            << " focus=" << GetFocus()
            << " recoveryPending=" << recovery.pending()
            << " recoverySwallowing=" << recovery.swallowing() << " qtActive="
            << (active ? active->objectName().toStdString() : "<none>")
            << " qtFocus="
            << (focus_widget ? focus_widget->objectName().toStdString()
                             : "<none>")
            << '\n';
  trace.print();
  native_trace.print_new();
}

// All input coordinates are inside our two windows. A desktop without a usable
// interactive input queue is skipped in main, never reported as a passing test.
void two_top_level_native_overlay_session(bool drag_selection,
                                          bool message_replay,
                                          bool sibling_active = false,
                                          bool forced_interruption = false) {
  CursorRestore restore_cursor(!message_replay);
  EventTrace trace;
  NativeMessageTrace native_trace;
  qApp->installEventFilter(&trace);
  const auto remove_trace = [&] { qApp->removeEventFilter(&trace); };
  struct TraceCleanup {
    std::function<void()> run;
    ~TraceCleanup() { run(); }
  } trace_cleanup{remove_trace};

  auto *screen = QGuiApplication::primaryScreen();
  HDRSHOT_CHECK(screen != nullptr);
  const QRect available = screen->availableGeometry();
  const int width = std::min(420, (available.width() - 80) / 2);
  const int height = std::min(340, available.height() - 80);
  HDRSHOT_CHECK(width >= 280 && height >= 220);
  const int x = available.left() + 20;
  const int y = available.top() + 20;
  const double scale = screen->devicePixelRatio();
  const QRect physical_screen = [&] {
    MONITORINFO monitor{sizeof(monitor)};
    HDRSHOT_CHECK(GetMonitorInfoW(
        MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor));
    return QRect(monitor.rcMonitor.left, monitor.rcMonitor.top,
                 monitor.rcMonitor.right - monitor.rcMonitor.left,
                 monitor.rcMonitor.bottom - monitor.rcMonitor.top);
  }();

  auto frozen = std::make_shared<FrozenDesktop>();
  frozen->frame_id = {3};
  frozen->display_generation = 9;
  auto windows = std::make_shared<WindowSnapshot>();
  windows->session_id = {1};
  windows->operation_id = {1};
  windows->display_generation = 9;
  CaptureInteractionSession lifecycle;
  HDRSHOT_CHECK(lifecycle.begin({1}));
  const PresentReceipt ready{{1}, {1}, {3}, 9, 1, 0};
  HDRSHOT_CHECK(lifecycle.ready(ready));
  auto recovery = std::make_shared<OverlayInputRecovery>();
  WindowsQtInputPlatformAdapter input_platform;
  std::array<std::unique_ptr<QtOverlayHost>, 2> hosts;
  std::array<std::shared_ptr<Presenter>, 2> presenters;
  std::array<HWND, 2> underlays{};
  std::array<PixelRect, 2> candidates{};
  int locks = 0;
  int exports = 0;
  bool invalid_export = false;
  DisplayId selected_display{};

  for (int index = 0; index < 2; ++index) {
    const DisplayId id{static_cast<std::uint64_t>(index + 7)};
    const int local_x = x + index * width;
    WindowsDisplayInfo display;
    display.snapshot.id = id;
    display.snapshot.desktop_frame_points = {
        static_cast<double>(local_x), static_cast<double>(y),
        static_cast<double>(width), static_cast<double>(height)};
    display.snapshot.point_pixel_scale = scale;
    display.snapshot.capture_size_px = {qRound(width * scale),
                                        qRound(height * scale)};
    display.physical_x = physical_screen.left() +
                         qRound((local_x - screen->geometry().left()) * scale);
    display.physical_y =
        physical_screen.top() + qRound((y - screen->geometry().top()) * scale);
    CanonicalFrameSegment segment;
    segment.display_id = id;
    segment.size_px = display.snapshot.capture_size_px;
    segment.point_pixel_scale = scale;
    segment.encoding = WindowsColor::linear_p3_encoding();
    std::vector<std::uint16_t> raw;
    raw.reserve(std::size_t(segment.size_px.width)*segment.size_px.height*4);
    for(int py=0;py<segment.size_px.height;++py)
      for(int px=0;px<segment.size_px.width;++px) {
        const std::uint16_t gray=px<segment.size_px.width/3 ? 0x3400 :
            px<2*segment.size_px.width/3 ? 0x3800 : 0x3a00;
        raw.insert(raw.end(),{gray,gray,gray,0x3c00});
      }
    // Match the production frozen-frame representation: normalize once to a
    // real native GPU source; no CPU half/float fallback may mask read failure.
    auto normalized=windows_normalize_scrgb_source(segment.size_px,std::move(raw),1.F,1.);
    HDRSHOT_CHECK(normalized.has_value());
    segment.pixel_format=PixelFormat::rgba32_float;
    segment.linear_source=std::move(normalized.value());
    frozen->canonical_segments.push_back(segment);
    display.source_white = {false, 80};
    display.advanced_color_mode = 1; // This fixture models an ACM target, without a Legacy ICC.
    // The click case mirrors the user's full-screen window candidate. The
    // drag case leaves a blank strip to begin a manual selection.
    candidates[index] =
        drag_selection ? PixelRect{qRound(40 * scale), qRound(35 * scale),
                                   qRound((width - 80) * scale),
                                   qRound((height - 130) * scale)}
                       : PixelRect{0, 0, display.snapshot.capture_size_px.width,
                                   display.snapshot.capture_size_px.height};
    windows->candidates.push_back(
        {static_cast<std::uint64_t>(index + 10), id, candidates[index], 0});
    auto host = std::make_unique<QtOverlayHost>(
        std::make_unique<WindowsQtOverlayWindow>(display), id,
        SelectionSnapshot{1, {}}, input_platform, recovery);
    host->setObjectName(QStringLiteral("overlay%1").arg(index));
    host->setGeometry(local_x, y, width, height);
    host->prepare_hidden_native_surface();
    host->windowHandle()->setScreen(screen);
    underlays[index] = static_cast<HWND>(host->native_surface());
    HDRSHOT_CHECK(underlays[index] && IsWindow(underlays[index]));
    host->set_interaction_allowed([&](DisplayId display_id) {
      return lifecycle.allows({1}, display_id);
    });
    host->set_initial_gesture_changed([&](DisplayId display_id, bool begin) {
      return lifecycle.initial_gesture({1}, display_id, begin);
    });
    host->set_selection_established([&](DisplayId display_id) {
      if (!lifecycle.lock({1}, display_id))
        return;
      ++locks;
      selected_display = display_id;
      for (auto &other : hosts)
        if (other &&
            other->property("testDisplayId").toULongLong() != display_id.value)
          other->set_interaction_locked(true);
    });
    host->setProperty("testDisplayId", static_cast<qulonglong>(id.value));
    host->set_export_requested(
        [&](UiCommand command, ExportSnapshot snapshot,
            QtOverlayHost::ExportCompleted) {
          if (command != UiCommand::copy_and_close ||
              snapshot.target_display_id != selected_display)
            invalid_export = true;
          ++exports;
          return false; // No clipboard or filesystem writes.
        },
        SettingsSnapshot{});
    auto actual = WindowsD3DPreviewPresenter::create(underlays[index], display, {}, host->native_magnifier_surface());
    HDRSHOT_CHECK(actual.has_value());
    presenters[index] = std::make_shared<Presenter>(actual.value());
    host->set_magnifier_card(actual.value());
    hosts[index] = std::move(host);
  }

  const auto payload = CaptureReadyPayload{frozen, ready, windows};
  const auto sequence = std::make_shared<std::atomic<std::uint64_t>>(2);
  // Initial display must use the frozen source before any pointer move.
  const auto initial_pointer=physical_point(*hosts[1],{width/2,90});
  HDRSHOT_CHECK(SetCursorPos(initial_pointer.x,initial_pointer.y));
  for (int index = 0; index < 2; ++index)
    hosts[index]->activate_capture(payload, presenters[index], sequence);
  std::cerr << "fixture raw0=" << underlays[0]
            << " qt0=" << reinterpret_cast<HWND>(hosts[0]->winId())
            << " raw1=" << underlays[1]
            << " qt1=" << reinterpret_cast<HWND>(hosts[1]->winId()) << '\n';
  HDRSHOT_CHECK(wait_until([&] {
    return hosts[0]->isVisible() && hosts[1]->isVisible() &&
           presenters[0]->completed->load() > 0 &&
           presenters[1]->completed->load() > 0;
  }));
  HDRSHOT_CHECK(!presenters[0]->failed->load() &&
                !presenters[1]->failed->load());
  HDRSHOT_CHECK(wait_until([&]{return hosts[1]->magnifier_preview().visible;}));
  HDRSHOT_CHECK(!hosts[0]->magnifier_preview().visible);
  check_magnifier_values(*hosts[1],"initial GPU sample before pointer move","0.5000","101.50");
  if (message_replay) {
    // Protocol replay only: establish this thread's Qt active window without
    // sending system input through the external lock-screen desktop.
    SetActiveWindow(
        reinterpret_cast<HWND>(hosts[sibling_active ? 0 : 1]->winId()));
    QApplication::processEvents();
    trace_state("replay queue activation", trace, native_trace, *recovery);
  } else if (sibling_active) {
    // The left overlay owns foreground focus while the right overlay receives
    // the first real system click through its translucent native underlay.
    const HWND left = reinterpret_cast<HWND>(hosts[0]->winId());
    hosts[0]->activateWindow();
    SetForegroundWindow(left);
    HDRSHOT_CHECK(wait_until([&] {
      return GetActiveWindow() == left && GetForegroundWindow() == left;
    }));
    trace_state("real sibling activation", trace, native_trace, *recovery);
  }

  auto center = [&](int index) {
    return physical_point(*hosts[index], {width / 2, 90});
  };
  auto underlay_at = [&](POINT point) -> HWND {
    for (const HWND hwnd : underlays) {
      RECT rect{};
      if (GetWindowRect(hwnd, &rect) && point.x >= rect.left &&
          point.x < rect.right && point.y >= rect.top && point.y < rect.bottom)
        return hwnd;
    }
    return nullptr;
  };
  auto message_at = [&](HWND hwnd, UINT message, WPARAM wparam, POINT point) {
    HDRSHOT_CHECK(SetCursorPos(point.x, point.y));
    // Drain OS moves caused by relocating the cursor before replaying our
    // final protocol event; otherwise an older queued move can overwrite it.
    QApplication::processEvents();
    POINT client = point;
    HDRSHOT_CHECK(ScreenToClient(hwnd, &client));
    SendMessageW(hwnd, message, wparam, MAKELPARAM(client.x, client.y));
    QApplication::processEvents();
  };
  HWND previous_underlay{};
  auto move_to = [&](POINT point) {
    if (message_replay) {
      const HWND current = underlay_at(point);
      HDRSHOT_CHECK(current != nullptr);
      HDRSHOT_CHECK(SetCursorPos(point.x, point.y));
      if (previous_underlay && previous_underlay != current)
        SendMessageW(previous_underlay, WM_MOUSELEAVE, 0, 0);
      message_at(current, WM_MOUSEMOVE,
                 restore_cursor.left_down ? MK_LBUTTON : 0, point);
      previous_underlay = current;
      return;
    }
    HDRSHOT_CHECK(send_mouse(point, 0));
    HDRSHOT_CHECK(wait_until([&] {
      POINT current{};
      return GetCursorPos(&current) && std::abs(current.x - point.x) <= 1 &&
             std::abs(current.y - point.y) <= 1;
    }));
    QApplication::processEvents();
  };
  move_to(center(0));
  if (!message_replay) {
    trace_hit("first native canvas", center(0), underlays[0],
              reinterpret_cast<HWND>(hosts[0]->winId()));
    HDRSHOT_CHECK(WindowFromPoint(center(0)) == underlays[0]);
  }
  HDRSHOT_CHECK(wait_until([&] {
    return presenters[0]->last &&
           preview_selection_rect(presenters[0]->last->model) == candidates[0];
  }));
  HDRSHOT_CHECK(hosts[0]->magnifier_preview().visible);
  HDRSHOT_CHECK(valid_magnifier_preview(hosts[0]->magnifier_preview(),
                                      frozen->canonical_segments[0].size_px));
  check_magnifier_values(*hosts[0],"first host GPU sample","0.5000","101.50");
  HDRSHOT_CHECK(locks == 0);

  move_to(center(1));
  if (!message_replay) {
    trace_hit("second native canvas", center(1), underlays[1],
              reinterpret_cast<HWND>(hosts[1]->winId()));
    HDRSHOT_CHECK(WindowFromPoint(center(1)) == underlays[1]);
  }
  HDRSHOT_CHECK(wait_until([&] {
    return presenters[1]->last &&
           preview_selection_rect(presenters[1]->last->model) == candidates[1];
  }));
  HDRSHOT_CHECK(hosts[1]->magnifier_preview().visible);
  HDRSHOT_CHECK(!hosts[0]->magnifier_preview().visible);
  check_magnifier_values(*hosts[1],"second host GPU sample","0.5000","101.50");
  // Move across distinct source values without waiting for any readout. Once
  // stationary, the final 0.75 gray must replace both pending and stale values.
  for(int offset=0;offset<18;++offset)
    move_to(physical_point(*hosts[1],{50+(width-110)*offset/17,90}));
  HDRSHOT_CHECK(hosts[1]->magnifier_preview().center.x>=
      2*frozen->canonical_segments[1].size_px.width/3);
  check_magnifier_values(*hosts[1],"continuous motion then stationary GPU sample","0.7500","152.25");
  const auto stopped=hosts[1]->magnifier_counters();
  move_to(physical_point(*hosts[1],{width-60,90}));
  QApplication::processEvents();
  HDRSHOT_CHECK(hosts[1]->magnifier_counters().requested==stopped.requested);
  check_magnifier_values(*hosts[1],"same stationary GPU pixel stays populated","0.7500","152.25");
  // Old native input has no TrackMouseEvent/WM_MOUSELEAVE bridge: A remains
  // highlighted while B is highlighted after crossing the real HWND seam.
  trace_state("after cross-overlay move", trace, native_trace, *recovery);
  const bool hover_cleared = wait_until([&] {
    return presenters[0]->last &&
           presenters[0]->last->model.initial_highlight.rect.empty();
  });
  std::cerr << "cross-overlay old candidate cleared=" << hover_cleared << '\n';

  POINT down = center(1);
  POINT up = down;
  if (drag_selection) {
    down = physical_point(*hosts[1], {15, 45});
    up = physical_point(*hosts[1], {width - 18, height - 55});
    move_to(down);
  }
  if (forced_interruption) {
    HDRSHOT_CHECK(message_replay);
    message_at(underlays[1], WM_LBUTTONDOWN, MK_LBUTTON, down);
    restore_cursor.left_down = true;
    HDRSHOT_CHECK(GetCapture() == underlays[1]);
    SendMessageW(underlays[1], WM_CANCELMODE, 0, 0);
    restore_cursor.left_down = false;
    trace_state("after forced WM_CANCELMODE", trace, native_trace, *recovery);
    HDRSHOT_CHECK(recovery->pending());
    HDRSHOT_CHECK(GetCapture() != underlays[1]);
    message_at(underlays[1], WM_LBUTTONUP, 0, down);
    // The next complete click only restores focus; it must not establish the
    // candidate. A second independent click must then work normally.
    message_at(underlays[1], WM_LBUTTONDOWN, MK_LBUTTON, down);
    restore_cursor.left_down = true;
    message_at(underlays[1], WM_LBUTTONUP, 0, down);
    restore_cursor.left_down = false;
    trace_state("after recovery click", trace, native_trace, *recovery);
    HDRSHOT_CHECK(locks == 0);
    HDRSHOT_CHECK(!recovery->pending() && !recovery->swallowing());
  }
  const auto mouse_activation =
      message_replay ? SendMessageW(underlays[1], WM_MOUSEACTIVATE,
                                    reinterpret_cast<WPARAM>(underlays[1]),
                                    MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN))
                     : MA_NOACTIVATE;
  if (message_replay) {
    std::cerr << "native WM_MOUSEACTIVATE=" << mouse_activation
              << " (MA_NOACTIVATE=" << MA_NOACTIVATE << ")\n";
    if (mouse_activation == MA_ACTIVATE ||
        mouse_activation == MA_ACTIVATEANDEAT)
      SetActiveWindow(underlays[1]);
    message_at(underlays[1], WM_LBUTTONDOWN, MK_LBUTTON, down);
  } else {
    HDRSHOT_CHECK(send_mouse(down, MOUSEEVENTF_LEFTDOWN));
  }
  restore_cursor.left_down = true;
  const bool captured =
      wait_until([&] { return GetCapture() == underlays[1]; });
  trace_state("after candidate press", trace, native_trace, *recovery);
  HDRSHOT_CHECK(captured);
  HDRSHOT_CHECK(locks == 0);
  if (drag_selection) {
    const POINT midpoint{(down.x + up.x) / 2, (down.y + up.y) / 2};
    move_to(midpoint);
  }
  if (message_replay)
    message_at(underlays[1], WM_LBUTTONUP, 0, up);
  else
    HDRSHOT_CHECK(send_mouse(up, MOUSEEVENTF_LEFTUP));
  restore_cursor.left_down = false;
  HDRSHOT_CHECK(wait_until([&] { return GetCapture() != underlays[1]; }));
  trace_state("after candidate release", trace, native_trace, *recovery);
  HDRSHOT_CHECK(wait_until([&] { return locks == 1; }));
  HDRSHOT_CHECK(selected_display == DisplayId{8});
  if (drag_selection)
    HDRSHOT_CHECK(!presenters[1]->last->model.selection.desktop_rect.empty());
  else
    HDRSHOT_CHECK(presenters[1]->last->model.selection.desktop_rect ==
                  candidates[1]);
  auto *toolbar = hosts[1]->findChild<QFrame *>("editorToolbar");
  HDRSHOT_CHECK(toolbar != nullptr);
  HDRSHOT_CHECK(wait_until([&] { return toolbar->isVisible(); }));
  HDRSHOT_CHECK(!hosts[0]->findChild<QWidget *>("overlayEditor")->isVisible());

  // Move from the alpha-zero native canvas onto the opaque Qt toolbar and
  // back. Both surfaces must belong to this overlay, with stable focus.
  const QPoint toolbar_center =
      toolbar->mapTo(hosts[1].get(), toolbar->rect().center());
  const POINT toolbar_point = physical_point(*hosts[1], toolbar_center);
  move_to(toolbar_point);
  if (!message_replay) {
    const HWND toolbar_hit = WindowFromPoint(toolbar_point);
    HDRSHOT_CHECK(toolbar_hit != underlays[1]);
    HDRSHOT_CHECK(GetWindowThreadProcessId(toolbar_hit, nullptr) ==
                  GetCurrentThreadId());
  }
  move_to(center(1));
  if (!message_replay)
    HDRSHOT_CHECK(WindowFromPoint(center(1)) == underlays[1]);
  move_to(toolbar_point);
  move_to(center(1));

  if (message_replay) {
    const HWND keyboard_target = reinterpret_cast<HWND>(hosts[1]->winId());
    HDRSHOT_CHECK(PostMessageW(keyboard_target, WM_KEYDOWN, VK_RETURN, 0));
    HDRSHOT_CHECK(PostMessageW(keyboard_target, WM_KEYUP, VK_RETURN, 0));
  } else {
    HDRSHOT_CHECK(send_key(VK_RETURN, false));
    HDRSHOT_CHECK(send_key(VK_RETURN, true));
  }
  const bool entered = wait_until([&] { return exports == 1; });
  trace_state("after native Enter", trace, native_trace, *recovery);
  HDRSHOT_CHECK(entered);
  HDRSHOT_CHECK(!invalid_export);
  HDRSHOT_CHECK(locks == 1 && hosts[1]->isVisible());
  HDRSHOT_CHECK(hover_cleared);
  if (message_replay)
    HDRSHOT_CHECK(mouse_activation == MA_NOACTIVATE);
}

} // namespace

bool interactive_input_desktop() {
  USEROBJECTFLAGS flags{};
  DWORD bytes{};
  if (!GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS, &flags,
                                 sizeof(flags), &bytes) ||
      !(flags.dwFlags & WSF_VISIBLE))
    return false;
  HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  if (!input)
    return false;
  std::array<wchar_t, 256> input_name{};
  std::array<wchar_t, 256> thread_name{};
  const bool same =
      GetUserObjectInformationW(input, UOI_NAME, input_name.data(),
                                sizeof(input_name), &bytes) &&
      GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),
                                UOI_NAME, thread_name.data(),
                                sizeof(thread_name), &bytes) &&
      input_name == thread_name;
  CloseDesktop(input);
  return same;
}

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  const bool message_replay =
      argc > 1 && std::string(argv[1]) == "--message-replay";
  const bool native_control =
      argc > 1 && (std::string(argv[1]) == "--visual-readings-native-control" ||
                   std::string(argv[1]) == "--visual-gamut-readings-native-control");
  const bool gamut_only = argc > 1 &&
      (std::string(argv[1]) == "--visual-gamut-readings" ||
       std::string(argv[1]) == "--visual-gamut-readings-native-control");
  const bool gpu_card=argc>1 && std::string(argv[1])=="--visual-card-readings";
  const bool visual_readings = gpu_card || native_control || gamut_only ||
      (argc > 1 && std::string(argv[1]) == "--visual-readings");
  if (!QGuiApplication::primaryScreen() ||
      (!message_replay && !interactive_input_desktop()) ||
      !GetSystemMetrics(SM_CXSCREEN) || !GetSystemMetrics(SM_CYSCREEN)) {
    std::cerr << "SKIP: no interactive Windows desktop/input queue\n";
    return 77;
  }
  if (!message_replay) {
    const POINT center{GetSystemMetrics(SM_CXSCREEN) / 2,
                       GetSystemMetrics(SM_CYSCREEN) / 2};
    wchar_t class_name[128]{};
    GetClassNameW(WindowFromPoint(center), class_name, 128);
    if (std::wstring(class_name) == L"LockScreenBackstopFrame") {
      std::cerr << "SKIP: Windows lock screen covers system input\n";
      return 77;
    }
  }
  if (visual_readings) {
    const auto available = QGuiApplication::primaryScreen()->availableGeometry();
    if (available.height() < 640 || available.width() < 460) {
      std::cerr << "SKIP: actual-screen readings require a visible 600-point host\n";
      return 77;
    }
    std::vector<hdrshot::test::TestCase> cases;
    if(gpu_card) {
      for(bool dark:{false,true}) {
        auto palette=app.palette();palette.setColor(QPalette::Window,dark?QColor("#25282d"):QColor("#fafafa"));
        palette.setColor(QPalette::WindowText,dark?QColor("#e8edf2"):QColor("#20252c"));
        app.setPalette(palette);
        const int result=hdrshot::test::run({
          {dark?"GPU card dark: natural scalar refresh":"GPU card light: natural scalar refresh",[]{actual_screen_magnifier_readings(false,false,true);}},
          {dark?"GPU card dark: gamut fields and SDR/HDR refresh":"GPU card light: gamut fields and SDR/HDR refresh",[]{actual_screen_magnifier_readings(false,true,true);}}
        });
        if(result)return result;
      }
      return 0;
    }
    if (!gamut_only)
      cases.emplace_back("actual Windows screen: placeholder then stationary scalar refresh",
                         [=] { actual_screen_magnifier_readings(native_control); });
    cases.emplace_back("actual Windows BitBlt screen: four new gamut groups order and SDR/HDR refresh",
                       [=] { actual_screen_magnifier_readings(native_control, true); });
    return hdrshot::test::run(cases);
  }
  std::vector<hdrshot::test::TestCase> cases{
      {"two native overlays: candidate click, toolbar, Enter",
       [=] { two_top_level_native_overlay_session(false, message_replay); }},
      {"two native overlays: cross-window drag selection",
       [=] { two_top_level_native_overlay_session(true, message_replay); }},
  };
  if (message_replay) {
    cases.emplace_back("message replay: click inactive sibling overlay", [] {
      two_top_level_native_overlay_session(false, true, true);
    });
    cases.emplace_back(
        "message replay: capture cancellation and recovery click",
        [] { two_top_level_native_overlay_session(false, true, false, true); });
  }
  if (!message_replay) {
    cases.emplace_back("real SendInput: click inactive sibling overlay", [] {
      two_top_level_native_overlay_session(false, false, true);
    });
  }
  return hdrshot::test::run(cases);
}
