#pragma once
#include <QLockFile>
#include <QString>
#include <memory>
namespace hdrshot {
enum class GuiInstanceStatus { owned, already_running, io_error };
class QtGuiInstanceGuard final {
 public:
  explicit QtGuiInstanceGuard(const QString& lock_path);
  GuiInstanceStatus status() const { return status_; }
  QString diagnostic() const { return diagnostic_; }
 private:
  std::unique_ptr<QLockFile> lock_;
  GuiInstanceStatus status_{GuiInstanceStatus::io_error};
  QString diagnostic_;
};
} // namespace hdrshot
