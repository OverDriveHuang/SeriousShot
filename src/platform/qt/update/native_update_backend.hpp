#pragma once
#include "platform/qt/update/update_storage.hpp"
namespace hdrshot::update {
class NativeInstaller {
public:
  virtual ~NativeInstaller() = default;
  virtual QString install_identity() const = 0;
  virtual Result<bool, Error> install(Slot &, const Candidate &, const QByteArray &envelope,
                                      std::function<bool()> quit) = 0;
  virtual Result<bool, Error> cancel_manual(Slot &) = 0;
  virtual bool manual() const = 0;
  virtual Result<bool, Error> open_manual(Slot &) {
    return Result<bool,Error>::failure(failure("无法打开安装包，请从发布页面下载安装。"));
  }
  virtual Result<bool, Error> recover(Slot &) {
    return Result<bool, Error>::success(true);
  }
};
std::unique_ptr<NativeInstaller> native_installer();
} // namespace hdrshot::update

#ifdef Q_OS_MACOS
namespace hdrshot::update {
Result<bool, Error> verify_macos_update_bundle(const QString &, const Candidate &);
}
#endif
