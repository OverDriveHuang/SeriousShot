#include "platform/qt/qt_application_restart.hpp"
#include "ui/qt/ui_language_runtime.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTimer>

#include <algorithm>

namespace hdrshot {
namespace {
constexpr auto restart_option = "--language-restart";
//% "Could not restart SeriousShot. Your language choice is saved. Retry, or close and reopen the app manually."
constexpr auto failure_id = QT_TRID_NOOP("language.restart.failed");
//% "SeriousShot could not finish starting. Close this message and reopen the app. Your language choice is saved."
constexpr auto startup_failure_id = QT_TRID_NOOP("language.restart.startup_failed");
//% "SeriousShot is already running. Quit the running instance, then open this app again."
constexpr auto already_running_id = QT_TRID_NOOP("language.restart.already_running");
//% "Retry"
constexpr auto retry_id = QT_TRID_NOOP("language.restart.retry");
//% "Close"
constexpr auto close_id = QT_TRID_NOOP("language.restart.close");

QString localized_or(const QtUiLanguageRuntime& runtime, UiLanguage target,
                     const char* id, const char* fallback) {
  const auto text = runtime.text(target, id);
  return text.isEmpty() ? QString::fromUtf8(fallback) : text;
}
}  // namespace

Result<std::optional<UiLanguage>, Error> parse_application_restart_arguments(
    const std::vector<std::string_view>& arguments) {
  using Parsed = Result<std::optional<UiLanguage>, Error>;
  const bool present = std::any_of(arguments.begin(), arguments.end(), [](auto value) {
    return value.starts_with(restart_option);
  });
  if (!present) return Parsed::success(std::nullopt);
  if (arguments.size() == 2 && arguments.front() == restart_option) {
    if (arguments.back() == "zh-CN") return Parsed::success(UiLanguage::simplified_chinese);
    if (arguments.back() == "en") return Parsed::success(UiLanguage::english);
  }
  return Parsed::failure({ErrorCode::invalid_input, "ApplicationRestart",
      Retryability::after_user_action, {{"reason", "invalid_restart_arguments"}}});
}

QtApplicationRestartRequest make_application_restart_request(
    UiLanguage target, const QtUiLanguageRuntime& runtime) {
  return {QCoreApplication::applicationFilePath(), QDir::currentPath(), target,
      localized_or(runtime, target, "language.restart.title", "Restart SeriousShot"),
      localized_or(runtime, target, failure_id,
          "Could not restart SeriousShot. Your language choice is saved. Retry, or close and reopen the app manually."),
      localized_or(runtime, target, retry_id, "Retry"),
      localized_or(runtime, target, close_id, "Close")};
}

QStringList application_restart_arguments(UiLanguage language) {
  return {QString::fromLatin1(restart_option),
      language == UiLanguage::simplified_chinese ? QStringLiteral("zh-CN") : QStringLiteral("en")};
}

int relaunch_application(const QtApplicationRestartRequest& request,
                         QtWindowActivationPort& activation) {
  while (!QProcess::startDetached(request.executable,
          application_restart_arguments(request.language), request.working_directory)) {
    QMessageBox error(QMessageBox::Critical, request.title, request.failure,
                      QMessageBox::NoButton);
    error.setOption(QMessageBox::Option::DontUseNativeDialog);
    error.setObjectName(QStringLiteral("applicationRestartFailure"));
    error.setTextFormat(Qt::PlainText);
    auto* retry = error.addButton(request.retry, QMessageBox::AcceptRole);
    retry->setObjectName(QStringLiteral("applicationRestartRetryButton"));
    auto* close = error.addButton(request.close, QMessageBox::RejectRole);
    error.setDefaultButton(close);
    error.setEscapeButton(close);
    QTimer::singleShot(0, &error, [&error, &activation] { activation.activate(error); });
    error.exec();
    if (error.clickedButton() != retry) return 1;
  }
  return 0;
}

void show_application_restart_failure(UiLanguage target, bool already_running,
                                      QtWindowActivationPort& activation) {
  QtUiLanguageRuntime runtime;
  (void)runtime.initialize(*QCoreApplication::instance(), target, [target] { return target; });
  const auto request = make_application_restart_request(target, runtime);
  const auto message = already_running
      ? localized_or(runtime, target, already_running_id,
          "SeriousShot is already running. Quit the running instance, then open this app again.")
      : localized_or(runtime, target, startup_failure_id,
          "SeriousShot could not finish starting. Close this message and reopen the app. Your language choice is saved.");
  QMessageBox error(QMessageBox::Critical, request.title, message, QMessageBox::NoButton);
  error.setOption(QMessageBox::Option::DontUseNativeDialog);
  error.setObjectName(QStringLiteral("applicationRestartStartupFailure"));
  error.setTextFormat(Qt::PlainText);
  auto* close = error.addButton(request.close, QMessageBox::RejectRole);
  error.setDefaultButton(close);
  error.setEscapeButton(close);
  QTimer::singleShot(0, &error, [&error, &activation] { activation.activate(error); });
  error.exec();
}
}  // namespace hdrshot
