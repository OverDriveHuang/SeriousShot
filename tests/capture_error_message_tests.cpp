#include "ui/qt/ui_language_runtime.hpp"
#include "ui/qt/capture_error_message.hpp"
#include "test_support.hpp"
#include <QCoreApplication>
using namespace hdrshot;
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const bool english = app.arguments().contains("--english");
  hdrshot::QtUiLanguageRuntime language;
  const auto locale = english ? hdrshot::UiLanguage::english
                              : hdrshot::UiLanguage::simplified_chinese;
  const auto initialized = language.initialize(app, locale, {});
  if (!initialized || initialized.value().active_language != locale)
    qFatal("Unable to initialize the requested test translations");
  if (english)
    return test::run({{"English capture error retains dynamic shortcut and error code", [] {
      Error timeout{ErrorCode::capture_failed, "test", Retryability::after_user_action,
                    {{"reason", "capture_progress_timeout"}}};
      HDRSHOT_CHECK(capture_error_message(timeout, "permission", "Alt+F12") ==
                    "Could not get all screen images in time. Press Alt+F12 or choose “Capture” to retry. "
                    "If it is still processing, wait before retrying. Restart SeriousShot if it stays busy.");
      timeout.safe_context.clear();
      HDRSHOT_CHECK(capture_error_message(timeout, "permission") ==
                    "Could not capture the screen. Press the capture shortcut or choose “Capture” to retry. "
                    "Error: capture_failed");
    }}});
  return test::run({{"dynamic shortcut and timeout", [] {
  Error timeout{ErrorCode::capture_failed,"test",Retryability::after_user_action,{{"reason","capture_progress_timeout"}}};
  auto text=capture_error_message(timeout,"permission","Alt+F12");
  HDRSHOT_CHECK(text.contains("Alt+F12") && text.contains(QStringLiteral("全部屏幕")));
  HDRSHOT_CHECK(text.contains(QStringLiteral("菜单“截图”")) && text.contains(QStringLiteral("重启")));
}}, {"permission and busy phases", [] {
  Error error{ErrorCode::permission_denied,"native",Retryability::after_user_action,{}};
  HDRSHOT_CHECK(capture_error_message(error,"permission guidance","X")=="permission guidance");
  HDRSHOT_CHECK(!capture_busy_message(UiPhase::capturing,false).contains(QStringLiteral("已经打开")));
  HDRSHOT_CHECK(capture_busy_message(UiPhase::selecting,false).contains(QStringLiteral("已经打开")));
  HDRSHOT_CHECK(capture_busy_message(UiPhase::idle,true).contains(QStringLiteral("稍后")));
  error.code=ErrorCode::capture_failed;
  HDRSHOT_CHECK(capture_error_message(error,"permission").contains(QStringLiteral("重试")));
}}}); }
