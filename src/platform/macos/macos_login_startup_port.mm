#include "platform/macos/macos_login_startup_port.hpp"
#import <AppKit/AppKit.h>
#import <ServiceManagement/ServiceManagement.h>
#import <Security/Security.h>
#include <QFileInfo>
namespace hdrshot {
namespace {
Error native_error(NSError* error, const char* operation) {
  return Error{ErrorCode::permission_denied, "macos_login_startup", Retryability::after_user_action,
      {{"operation", operation}, {"domain", error ? error.domain.UTF8String : "ServiceManagement"},
       {"native_code", std::to_string(error ? error.code : 0)},
       {"description", error ? error.localizedDescription.UTF8String : "登录启动操作未完成，请检查系统设置。"}}};
}
std::string target_problem(NSBundle* bundle) {
  if (!bundle.bundleIdentifier.length || ![bundle.bundlePath.pathExtension isEqualToString:@"app"] ||
      !bundle.executableURL || !QFileInfo(QString::fromUtf8(bundle.executablePath.UTF8String)).isExecutable())
    return "请先运行完整的已签名应用，再启用登录启动。";
  SecStaticCodeRef code = nullptr;
  const auto created = SecStaticCodeCreateWithPath((__bridge CFURLRef)bundle.bundleURL, kSecCSDefaultFlags, &code);
  const auto valid = created == errSecSuccess ? SecStaticCodeCheckValidity(code, kSecCSStrictValidate, nullptr) : created;
  if (code) CFRelease(code);
  if (valid != errSecSuccess) return "应用签名验证失败，请重新安装完整应用后重试（" + std::to_string(valid) + "）。";
  return {};
}
}
Result<LoginStartupSnapshot, Error> MacLoginStartupPort::query() {
  if (!NSThread.isMainThread) return Result<LoginStartupSnapshot, Error>::failure(
      Error{ErrorCode::precondition_failed,"macos_login_startup",Retryability::never,{{"reason","必须在主线程读取登录状态。"}}});
  LoginStartupSnapshot snapshot;
  snapshot.current_bundle_path = QFileInfo(QString::fromUtf8(NSBundle.mainBundle.bundlePath.UTF8String)).canonicalFilePath().toStdString();
  snapshot.can_open_system_settings = true;
  snapshot.approval_message = "需要在系统设置中批准登录启动。";
  const auto status = SMAppService.mainAppService.status;
  switch(status) {
    case SMAppServiceStatusNotRegistered: snapshot.state = LoginStartupState::disabled; break;
    case SMAppServiceStatusEnabled: snapshot.state = LoginStartupState::enabled; break;
    case SMAppServiceStatusRequiresApproval: snapshot.state = LoginStartupState::requires_approval; break;
    case SMAppServiceStatusNotFound: snapshot.state = LoginStartupState::service_not_found; break;
    default: snapshot.state = LoginStartupState::unknown; break;
  }
  snapshot.can_disable = status == SMAppServiceStatusEnabled || status == SMAppServiceStatusRequiresApproval;
  const auto problem = target_problem(NSBundle.mainBundle);
  snapshot.can_enable = problem.empty() &&
      (status == SMAppServiceStatusNotRegistered || status == SMAppServiceStatusNotFound);
  if (!problem.empty()) {
    snapshot.diagnostic = problem;
    // Do not prevent turning off a previously registered service from an
    // invalid location. Its real enabled state remains visible and operable.
    if (!snapshot.can_disable) snapshot.state = LoginStartupState::invalid_target;
  }
  return Result<LoginStartupSnapshot, Error>::success(std::move(snapshot));
}
Result<bool, Error> MacLoginStartupPort::set_enabled(bool enabled) {
  const auto current = query();
  if (!current) return Result<bool, Error>::failure(current.error());
  if (!(enabled ? current.value().can_enable : current.value().can_disable))
    return Result<bool, Error>::failure(Error{ErrorCode::precondition_failed,"macos_login_startup",Retryability::after_user_action,
      {{"reason", current.value().diagnostic.empty() ? "请在系统设置中允许登录启动，然后再次勾选。" : current.value().diagnostic}}});
  NSError* error = nil;
  const BOOL ok = enabled ? [SMAppService.mainAppService registerAndReturnError:&error] :
      [SMAppService.mainAppService unregisterAndReturnError:&error];
  if (!ok) return Result<bool, Error>::failure(native_error(error, enabled ? "register" : "unregister"));
  return Result<bool, Error>::success(true);
}
Result<bool, Error> MacLoginStartupPort::open_system_settings() {
  if (!NSThread.isMainThread) return Result<bool, Error>::failure(
      Error{ErrorCode::precondition_failed,"macos_login_startup",Retryability::never,{}});
  [SMAppService openSystemSettingsLoginItems];
  return Result<bool, Error>::success(true);
}
} // namespace hdrshot
