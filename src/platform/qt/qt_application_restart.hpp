#pragma once

#include "core/error.hpp"
#include "core/result.hpp"
#include "ports/export_ports.hpp"
#include "ui/qt/qt_window_activation_port.hpp"

#include <QString>
#include <QStringList>
#include <optional>
#include <string_view>
#include <vector>

namespace hdrshot {
class QtUiLanguageRuntime;

struct QtApplicationRestartRequest {
  QString executable;
  QString working_directory;
  UiLanguage language;
  QString title;
  QString failure;
  QString retry;
  QString close;
};

Result<std::optional<UiLanguage>, Error> parse_application_restart_arguments(
    const std::vector<std::string_view>& arguments);
QtApplicationRestartRequest make_application_restart_request(
    UiLanguage target, const QtUiLanguageRuntime& runtime);
QStringList application_restart_arguments(UiLanguage language);
int relaunch_application(const QtApplicationRestartRequest& request,
                         QtWindowActivationPort& activation);
void show_application_restart_failure(UiLanguage target, bool already_running,
                                      QtWindowActivationPort& activation);
}  // namespace hdrshot
