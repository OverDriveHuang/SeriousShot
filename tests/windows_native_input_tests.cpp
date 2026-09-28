#include "platform/windows/windows_qt_ports.hpp"
#include "test_support.hpp"
#include <QApplication>
#include <QMouseEvent>
#include <QPixmap>
#include <memory>
#include <windows.h>

using namespace hdrshot;
namespace {
struct Receiver : QWidget {
  using QWidget::QWidget;
  int presses{}, releases{};
  void mousePressEvent(QMouseEvent *) override { ++presses; }
  void mouseReleaseEvent(QMouseEvent *) override { ++releases; }
};
void hidden_native_surface_input_and_cursor() {
  WindowsDisplayInfo display;
  display.snapshot.capture_size_px = {200, 100};
  QWidget host;
  host.resize(200, 100);
  Receiver child(&host);
  child.setGeometry(20, 10, 100, 60);
  child.show();
  auto window = std::make_unique<WindowsQtOverlayWindow>(display);
  int interrupted = 0;
  window->set_input_interrupted([&] { ++interrupted; });
  window->prepare(host);
  window->configure(host, true);
  const auto hwnd = static_cast<HWND>(window->native_surface());
  HDRSHOT_CHECK(hwnd && IsWindow(hwnd));
  host.setWindowOpacity(0);
  ShowWindow(hwnd, SW_HIDE);
  const auto scale = host.devicePixelRatioF();
  const auto point =
      MAKELPARAM(static_cast<int>(40 * scale), static_cast<int>(30 * scale));
  SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
  SendMessageW(hwnd, WM_LBUTTONUP, 0, point);
  HDRSHOT_CHECK(child.presses == 1 && child.releases == 1 && interrupted == 0);
  SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
  SendMessageW(hwnd, WM_CANCELMODE, 0, 0);
  HDRSHOT_CHECK(child.presses == 2 && child.releases == 1 && interrupted == 1);
  SendMessageW(hwnd, WM_CAPTURECHANGED, 0, 0);
  HDRSHOT_CHECK(interrupted == 1);

  QPixmap bitmap(32, 32);
  bitmap.fill(Qt::transparent);
  bitmap.fill(Qt::red);
  child.setCursor(QCursor(bitmap, 9, 11));
  SendMessageW(hwnd, WM_MOUSEMOVE, 0, point);
  ICONINFO icon{};
  HDRSHOT_CHECK(GetIconInfo(GetCursor(), &icon));
  HDRSHOT_CHECK(icon.xHotspot == 9 && icon.yHotspot == 11);
  DeleteObject(icon.hbmColor);
  DeleteObject(icon.hbmMask);
  window->set_input_interrupted({});
  window.reset();
  HDRSHOT_CHECK(!IsWindow(hwnd) && interrupted == 1);
}
} // namespace
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  return hdrshot::test::run(
      {{"hidden native input capture loss and cursor ownership",
        hidden_native_surface_input_and_cursor}});
}
