#include "ui/qt/capture_error_message.hpp"
#include "test_support.hpp"
using namespace hdrshot;
int main() { return test::run({{"dynamic shortcut and timeout", [] {
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
