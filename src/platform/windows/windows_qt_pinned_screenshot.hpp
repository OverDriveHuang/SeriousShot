#pragma once
#include "platform/qt/qt_pinned_screenshot_controller.hpp"

namespace hdrshot {
class WindowsDisplayStateService;
std::shared_ptr<QtPinnedWindowPort> make_windows_pinned_window_port(
    std::shared_ptr<DiagnosticsPort> diagnostics = {});
Result<PreparedPinnedScreenshot, Error> prepare_windows_pinned_screenshot(
    const ExportSnapshot &, std::shared_ptr<DiagnosticsPort> diagnostics = {},
    std::shared_ptr<WindowsDisplayStateService> displays = {});
} // namespace hdrshot
