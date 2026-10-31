#include "platform/qt/qt_gui_instance_guard.hpp"
#include <QDir>
#include <QFileInfo>
namespace hdrshot {
QtGuiInstanceGuard::QtGuiInstanceGuard(const QString& path) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
    diagnostic_ = QStringLiteral("无法创建应用实例锁目录：") + QFileInfo(path).absolutePath();
    return;
  }
  lock_ = std::make_unique<QLockFile>(path);
  // No arbitrary age expiry: QLockFile still removes locks belonging to dead
  // processes on this machine, while a long-running resident remains owned.
  lock_->setStaleLockTime(0);
  if (lock_->tryLock(0)) { status_ = GuiInstanceStatus::owned; return; }
  if (lock_->error() == QLockFile::LockFailedError) {
    qint64 pid{}; QString host, application;
    if (lock_->getLockInfo(&pid, &host, &application)) {
      status_ = GuiInstanceStatus::already_running;
      return;
    }
  }
  diagnostic_ = QStringLiteral("无法取得应用实例锁：") + path +
      QStringLiteral("（错误 %1）").arg(static_cast<int>(lock_->error()));
}
} // namespace hdrshot
