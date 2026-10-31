#pragma once
#include "ports/login_startup_port.hpp"
#include <QByteArray>
#include <QString>
#include <functional>
namespace hdrshot {
enum class WindowsStartupApproval { missing_no_override, enabled, blocked, unknown };
struct WindowsStartupApprovalRead {
  WindowsStartupApproval state{WindowsStartupApproval::unknown};
  std::string diagnostic;
};
WindowsStartupApproval decode_windows_startup_approval(unsigned long type, const QByteArray& bytes);
WindowsStartupApproval classify_windows_startup_approval_read(unsigned long status, unsigned long type, const QByteArray& bytes);
// Composition/harness dependency injection only. No runtime user path/item switches.
struct WindowsLoginStartupContext {
  QString item_basename{QStringLiteral("SeriousShot.lnk")};
  QString ownership_marker{QStringLiteral("SeriousShot managed login startup v1")};
  std::function<Result<QString,Error>()> startup_directory;
  std::function<QString()> current_executable;
  std::function<WindowsStartupApprovalRead()> approval;
};
class WindowsLoginStartupPort final : public LoginStartupPort {
 public:
  explicit WindowsLoginStartupPort(WindowsLoginStartupContext context = {});
  Result<LoginStartupSnapshot,Error> query() override;
  Result<bool,Error> set_enabled(bool enabled) override;
  Result<bool,Error> open_system_settings() override;
 private:
  WindowsLoginStartupContext context_;
};
}
