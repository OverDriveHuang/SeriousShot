#include "platform/macos/macos_login_startup_paths.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
namespace hdrshot {
namespace {
QByteArray read_bounded(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) return {};
  return file.readAll();
}
bool project_root(const QString& path) {
  const auto cmake = read_bounded(path + "/CMakeLists.txt");
  return QRegularExpression(QStringLiteral(R"(\bproject\s*\(\s*HDRShot(?:\s|\)))"))
      .match(QString::fromUtf8(cmake)).hasMatch() &&
      QFileInfo::exists(path + "/src/ui/qt/settings_window.cpp") &&
      QFileInfo::exists(path + "/tools/macos_qt_overlay_poc.mm");
}
}
bool macos_login_target_is_project_staging(const QString& bundle_path) {
  const QString canonical = QFileInfo(bundle_path).canonicalFilePath();
  if (canonical.isEmpty()) return false;
  QDir ancestor(QFileInfo(canonical).absolutePath());
  for (int depth = 0; depth < 12; ++depth) {
    const auto cache = QString::fromUtf8(read_bounded(ancestor.filePath("CMakeCache.txt")));
    const auto name = QRegularExpression(QStringLiteral(R"((?:^|\n)CMAKE_PROJECT_NAME:[^=\n]*=HDRShot\r?(?:\n|$))")).match(cache);
    const auto home = QRegularExpression(QStringLiteral(R"((?:^|\n)CMAKE_HOME_DIRECTORY:[^=\n]*=([^\r\n]+))")).match(cache);
    if (name.hasMatch() && home.hasMatch()) {
      const auto source = QFileInfo(home.captured(1)).canonicalFilePath();
      if (!source.isEmpty() && project_root(source)) return true;
    }
    if (project_root(ancestor.absolutePath())) {
      const auto relative = ancestor.relativeFilePath(canonical);
      const auto first = relative.section('/', 0, 0);
      if (first == ".agent" || first == "artifacts") return true;
    }
    if (!ancestor.cdUp()) break;
  }
  return false;
}
} // namespace hdrshot
