#include "application/capture_interaction_session.hpp"
#include "platform/windows/windows_d3d_presenter.hpp"
#include "platform/windows/windows_qt_ports.hpp"
#include "test_support.hpp"
#include "ui/qt/qt_overlay_host.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QFrame>
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
      if (!result)
        failed->store(true);
      completed->fetch_add(1);
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
    for (int pixel = 0; pixel < segment.size_px.width * segment.size_px.height;
         ++pixel)
      segment.rgba_half.insert(segment.rgba_half.end(),
                               {0x3800, 0x3800, 0x3800, 0x3c00});
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
    auto actual = WindowsD3DPreviewPresenter::create(underlays[index], display);
    HDRSHOT_CHECK(actual.has_value());
    presenters[index] = std::make_shared<Presenter>(actual.value());
    hosts[index] = std::move(host);
  }

  const auto payload = CaptureReadyPayload{frozen, ready, windows};
  const auto sequence = std::make_shared<std::atomic<std::uint64_t>>(2);
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
