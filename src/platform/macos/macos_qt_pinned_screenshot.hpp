#pragma once
#include "platform/qt/qt_pinned_screenshot_controller.hpp"
namespace hdrshot {
Result<PreparedPinnedScreenshot,Error> prepare_macos_pinned_screenshot(const ExportSnapshot &);
std::shared_ptr<QtPinnedWindowPort> make_macos_pinned_window_port();
}
