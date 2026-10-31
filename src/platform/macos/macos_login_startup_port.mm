#include "platform/macos/macos_login_startup_port.hpp"
#include "platform/macos/macos_login_startup_paths.hpp"
#import <AppKit/AppKit.h>
#import <ServiceManagement/ServiceManagement.h>
#import <Security/Security.h>
#import <DiskArbitration/DiskArbitration.h>
#import <sys/mount.h>
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
  const auto path = QString::fromUtf8(bundle.bundlePath.UTF8String);
  if (!bundle.bundleIdentifier.length || ![bundle.bundlePath.pathExtension isEqualToString:@"app"] ||
      !bundle.executableURL || !QFileInfo(QString::fromUtf8(bundle.executablePath.UTF8String)).isExecutable())
    return "请先运行完整的已签名应用，再启用登录启动。";
  const auto canonical = QFileInfo(path).canonicalFilePath();
  if (canonical.isEmpty()) return "无法读取当前应用的实际位置，请重新安装后重试。";
  if (canonical.split('/').contains("AppTranslocation"))
    return "请先将应用复制到稳定的本地位置，再启用登录启动。";
  struct statfs volume{};
  if (statfs(canonical.toUtf8().constData(), &volume) != 0)
    return "无法确认应用所在卷，请检查安装位置后重试。";
  if (volume.f_flags & MNT_RDONLY)
    return "应用位于只读卷，请先复制到稳定的本地位置，再启用登录启动。";
  if (!(volume.f_flags & MNT_LOCAL))
    return "应用位于网络卷，请先复制到稳定的本地位置，再启用登录启动。";
  DASessionRef session = DASessionCreate(kCFAllocatorDefault);
  DADiskRef disk = session ? DADiskCreateFromVolumePath(kCFAllocatorDefault, session,
      (__bridge CFURLRef)[NSURL fileURLWithFileSystemRepresentation:volume.f_mntonname isDirectory:YES relativeToURL:nil]) : nullptr;
  CFDictionaryRef description = disk ? DADiskCopyDescription(disk) : nullptr;
  const auto protocol = description ? static_cast<CFStringRef>(CFDictionaryGetValue(
      description, kDADiskDescriptionDeviceProtocolKey)) : nullptr;
  const bool disk_image = protocol && CFGetTypeID(protocol) == CFStringGetTypeID() &&
      CFStringCompare(protocol, CFSTR("Disk Image"), 0) == kCFCompareEqualTo;
  if (description) CFRelease(description);
  if (disk) CFRelease(disk);
  if (session) CFRelease(session);
  if (disk_image)
    return "应用位于磁盘映像，请先复制到稳定的本地位置，再启用登录启动。";
  if (macos_login_target_is_project_staging(canonical))
    return "当前应用位于项目构建或暂存目录，请先复制到稳定的本地位置，再启用登录启动。";
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
