#pragma once
#include "platform/windows/windows_analysis_backend.hpp"
#include "platform/windows/windows_display_state_service.hpp"
#include <QWidget>
#include <functional>

namespace hdrshot {
// UI-thread attachment shared by analyzer and pin hosts. The actual native
// content surface, not its outer window, identifies the presentation target.
Result<bool, Error> bind_windows_source_display(
    QWidget &owner, QWidget &surface,
    std::shared_ptr<AnalysisPresenterPort> presenter,
    std::shared_ptr<WindowsDisplayStateService> displays,
    std::function<void()> request_present,
    std::function<void(Error)> report_error = {},
    std::function<void()> clear_display_error = {});
} // namespace hdrshot
