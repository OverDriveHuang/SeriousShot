#include "platform/cpu/cpu_analysis_port.hpp"
#include "platform/windows/windows_analysis_backend.hpp"
#include "platform/windows/windows_capture.hpp"
#include "test_support.hpp"
#include "ui/qt/analyzer_source_view.hpp"
#include "ui/qt/analyzer_window.hpp"
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QMouseEvent>
#include <chrono>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <windows.h>

using namespace hdrshot;
namespace {
analysis::Input fixture() {
  analysis::FloatImage pixels;
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 16; ++x)
      pixels.push_back({float(x) / 4, float(y) / 4, 0.25F, 1});
  auto source = make_cpu_analysis_source({16, 8}, std::move(pixels));
  HDRSHOT_CHECK(source.has_value());
  return {source.value(), 1, true};
}
struct MouseFilter final : QObject {
  int presses{};
  QPointF position;
  bool eventFilter(QObject *, QEvent *event) override {
    if (event->type() == QEvent::MouseButtonPress) {
      ++presses;
      position = static_cast<QMouseEvent *>(event)->position();
    }
    return false;
  }
};
void native_source_child_composes_and_receives_input() {
  const auto input = fixture();
  auto window = std::make_unique<AnalyzerWindow>(input, std::string{});
  window->resize(800, 600);
  window->setWindowOpacity(0);
  window->show();
  QCoreApplication::processEvents();
  auto *surface = window->source_widget()->presentation_surface();
  const auto hwnd = reinterpret_cast<HWND>(surface->winId());
  HDRSHOT_CHECK(hwnd && IsWindow(hwnd) && IsWindowVisible(hwnd));
  SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                    GetWindowLongPtrW(hwnd, GWL_EXSTYLE) |
                        WS_EX_NOREDIRECTIONBITMAP);
  SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                   SWP_FRAMECHANGED);
  MouseFilter filter;
  surface->installEventFilter(&filter);
  const auto dpr = surface->devicePixelRatioF();
  const auto local =
      MAKELPARAM(static_cast<int>(5 * dpr), static_cast<int>(7 * dpr));
  SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, local);
  SendMessageW(hwnd, WM_LBUTTONUP, 0, local);
  QCoreApplication::processEvents();
  HDRSHOT_CHECK(filter.presses > 0);
  HDRSHOT_CHECK_NEAR(filter.position.x(), 5, 1);
  HDRSHOT_CHECK_NEAR(filter.position.y(), 7, 1);

  auto made = make_windows_analysis_presenter();
  HDRSHOT_CHECK(made.has_value());
  auto presenter = std::move(made.value());
  struct Mailbox {
    std::mutex mutex;
    std::optional<Error> error;
  };
  auto mailbox = std::make_shared<Mailbox>();
  set_windows_analysis_presenter_error_callback(*presenter, [mailbox](Error e) {
    const std::scoped_lock lock(mailbox->mutex);
    mailbox->error = std::move(e);
  });
  analysis::SourceView view;
  view.target_size = {static_cast<int>(surface->width() * dpr),
                      static_cast<int>(surface->height() * dpr)};
  view.scale = 2;
  HDRSHOT_CHECK(view.target_size.width > 0 && view.target_size.height > 0);
  auto overlay = std::make_shared<analysis::UiImage>();
  overlay->size = view.target_size;
  overlay->rgba.resize(std::size_t(view.target_size.width) *
                       view.target_size.height * 4);
  overlay->rgba[0] = 255;
  overlay->rgba[3] = 128;
  view.operation_overlay = overlay;
  for (int i = 0; i < 3; ++i) {
    view.offset_x = i;
    HDRSHOT_CHECK(
        presenter->present(input, view, reinterpret_cast<std::uintptr_t>(hwnd))
            .has_value());
  }
  const auto await_latest = [&] {
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    WindowsAnalysisPresentationStatus status;
    do {
      QCoreApplication::processEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      status = windows_analysis_presentation_status(*presenter);
    } while (status.completed_generation < status.requested &&
             status.failed == 0 && std::chrono::steady_clock::now() < until);
    return status;
  };
  auto status = await_latest();
  if (status.failed) {
    const std::scoped_lock lock(mailbox->mutex);
    if (mailbox->error)
      std::cerr << "presenter error=" << to_string(mailbox->error->code)
                << '\n';
  }
  HDRSHOT_CHECK(status.completed_generation == status.requested &&
                status.completed > 0);
  window->resize(900, 650);
  QCoreApplication::processEvents();
  view.target_size = {
      static_cast<int>(surface->width() * surface->devicePixelRatioF()),
      static_cast<int>(surface->height() * surface->devicePixelRatioF())};
  view.operation_overlay.reset();
  HDRSHOT_CHECK(
      presenter->present(input, view, reinterpret_cast<std::uintptr_t>(hwnd))
          .has_value());
  status = await_latest();
  HDRSHOT_CHECK(status.completed_generation == status.requested &&
                status.failed == 0);
  // Close with work in flight. The presenter owns its retained input and must
  // never dereference this HWND after the window has gone.
  view.offset_x = 99;
  HDRSHOT_CHECK(
      presenter->present(input, view, reinterpret_cast<std::uintptr_t>(hwnd))
          .has_value());
  set_windows_analysis_presenter_error_callback(*presenter, {});
  surface->removeEventFilter(&filter);
  window.reset();
  presenter.reset();
  made.value().reset();
}
} // namespace
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  const auto displays = windows_enumerate_displays();
  if (!displays || displays.value().empty()) {
    std::cout << "SKIP: no available desktop display\n";
    return 77;
  }
  return hdrshot::test::run(
      {{"hidden analyzer HWND presentation and input",
        native_source_child_composes_and_receives_input}});
}
