#include "platform/qt/update/native_update_backend.hpp"
#include "platform/qt/update/update_package.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QProcess>
#include <QSet>
#include <QXmlStreamReader>
#include <QtEndian>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <libproc.h>
#include <signal.h>
#include <unistd.h>
namespace hdrshot::update {
QString process_identity(qint64 pid) {
  if (pid <= 0 || pid > INT_MAX)
    return {};
  proc_bsdinfo info{};
  if (proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info))
    return {};
  return QString::number(info.pbi_start_tvsec) + ":" + QString::number(info.pbi_start_tvusec);
}
bool process_alive(qint64 pid, const QString &creation) {
  if (pid <= 0 || pid > INT_MAX)
    return false;
  const auto current = process_identity(pid);
  if (!current.isEmpty())
    return creation.isEmpty() || current == creation;
  return ::kill(static_cast<int>(pid), 0) == 0 || errno == EPERM;
}
bool durable_flush(QFile &f) {
  return f.flush() && ::fcntl(f.handle(), F_FULLFSYNC) == 0;
}
bool durable_directory(const QString &path) {
  const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY);
  if (fd < 0)
    return false;
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
}
static Result<QByteArray, Error> run(const QString &program, const QStringList &args) {
  QProcess p;
  p.setProgram(program);
  p.setArguments(args);
  p.start();
  if (!p.waitForStarted(10000) || !p.waitForFinished(120000) ||
      p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
    return Result<QByteArray, Error>::failure(
        failure("系统无法打开或卸载安装包", ErrorCode::permission_denied));
  return Result<QByteArray, Error>::success(p.readAllStandardOutput());
}
static QString mount_from_plist(const QByteArray &bytes) {
  QXmlStreamReader xml(bytes);
  bool next = false;
  while (!xml.atEnd()) {
    xml.readNext();
    if (xml.isStartElement() && xml.name() == "key")
      next = xml.readElementText() == "mount-point";
    else if (next && xml.isStartElement() && xml.name() == "string")
      return xml.readElementText();
  }
  return {};
}
static QJsonValue plist_value(QXmlStreamReader &xml) {
  if (xml.name() == "dict") {
    QJsonObject object;
    QString key;
    while (xml.readNextStartElement()) {
      if (xml.name() == "key")
        key = xml.readElementText();
      else {
        const auto value = plist_value(xml);
        if (!key.isEmpty())
          object.insert(key, value);
        key.clear();
      }
    }
    return object;
  }
  if (xml.name() == "array") {
    QJsonArray a;
    while (xml.readNextStartElement())
      a.push_back(plist_value(xml));
    return a;
  }
  if (xml.name() == "string" || xml.name() == "integer")
    return xml.readElementText();
  xml.skipCurrentElement();
  return {};
}
static Result<QString, Error> current_mount(const QString &image) {
  const auto info = run("/usr/bin/hdiutil", {"info", "-plist"});
  if (!info)
    return Result<QString, Error>::failure(info.error());
  QXmlStreamReader xml(info.value());
  QJsonObject object;
  while (xml.readNextStartElement()) {
    if (xml.name() == "plist") {
      if (xml.readNextStartElement())
        object = plist_value(xml).toObject();
      break;
    }
    xml.skipCurrentElement();
  }
  if (xml.hasError() || !object["images"].isArray())
    return Result<QString, Error>::failure(failure("mount_info_parse"));
  for (const auto &value : object["images"].toArray()) {
    const auto entry = value.toObject();
    if (QFileInfo(entry["image-path"].toString()).canonicalFilePath() !=
        QFileInfo(image).canonicalFilePath())
      continue;
    for (const auto &entity : entry["system-entities"].toArray()) {
      const auto mount = entity.toObject()["mount-point"].toString();
      if (!mount.isEmpty())
        return Result<QString, Error>::success(mount);
    }
  }
  return Result<QString, Error>::success({});
}
Result<bool, Error> verify_macos_update_bundle(const QString &mount, const Candidate &c) {
  using R = Result<bool, Error>;
  const auto app = mount + "/SeriousShot.app";
  for (const auto &file :
       QDir(mount).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot))
    if (file.fileName().endsWith(".app", Qt::CaseInsensitive) && file.absoluteFilePath() != app)
      return R::failure(failure("mac_unexpected_app"));
  if (c.platform != "macos" || QFileInfo(app).isSymLink() || !QFileInfo(app).isDir())
    return R::failure(failure("mac_app_missing"));
  auto manifest = package_manifest(app, c, mount + "/package_manifest.json");
  if (!manifest)
    return R::failure(manifest.error());
  const auto &package = manifest.value();
  if (package.raw["app_name"] != "SeriousShot.app" ||
      package.raw["bundle_id"] != "dev.hdrshot.desktop" ||
      package.entrypoint != "Contents/MacOS/SeriousShot")
    return R::failure(failure("mac_manifest_identity"));
  auto files = verify_tree(app, package);
  if (!files)
    return files;
  QSet<QString> expected;
  for (const auto &f : package.files)
    expected.insert(f.path);
  for (const auto &value : package.raw["symlinks"].toArray()) {
    const auto link = value.toObject();
    const auto path = link["path"].toString(), to = link["target"].toString();
    if (!safe_relative(path) || QDir::isAbsolutePath(to) || expected.contains(path) ||
        !QFileInfo(app + "/" + path).isSymLink())
      return R::failure(failure("mac_manifest_symlink"));
    std::error_code error;
    const auto actual = std::filesystem::read_symlink((app + "/" + path).toStdString(), error);
    const auto resolved = QFileInfo(app + "/" + path).canonicalFilePath();
    if (error || QString::fromStdString(actual.string()) != to ||
        !resolved.startsWith(QDir(app).canonicalPath() + "/"))
      return R::failure(failure("mac_symlink_target"));
    expected.insert(path);
  }
  QDirIterator all(app, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                   QDirIterator::Subdirectories);
  while (all.hasNext()) {
    const auto path = all.next(), relative = QDir(app).relativeFilePath(path);
    if (all.fileInfo().isDir() && !all.fileInfo().isSymLink())
      continue;
    if (!expected.contains(relative))
      return R::failure(failure("mac_unmanaged_file"));
  }
  const auto info =
      run("/usr/bin/plutil", {"-convert", "json", "-o", "-", app + "/Contents/Info.plist"});
  if (!info)
    return R::failure(failure("mac_bundle_info"));
  const auto plist = QJsonDocument::fromJson(info.value()).object();
  if (plist["CFBundleIdentifier"] != "dev.hdrshot.desktop" ||
      plist["CFBundleShortVersionString"].toString().toStdString() != c.version.text() ||
      plist["CFBundleExecutable"] != "SeriousShot")
    return R::failure(failure("mac_bundle_identity"));
  QFile executable(app + "/" + package.entrypoint);
  if (!executable.open(QIODevice::ReadOnly))
    return R::failure(failure("mac_executable"));
  const auto header = executable.read(4096);
  const quint32 wanted = c.architecture == "arm64" ? 0x0100000c
                         : c.architecture == "x64" ? 0x01000007
                                                   : 0;
  qint64 slice_offset = 0;
  bool architecture = false;
  if (header.size() >= 12) {
    const auto *raw = reinterpret_cast<const uchar *>(header.constData());
    if (qFromLittleEndian<quint32>(raw) == 0xfeedfacf)
      architecture = qFromLittleEndian<quint32>(raw + 4) == wanted;
    else {
      const auto magic = qFromBigEndian<quint32>(raw), count = qFromBigEndian<quint32>(raw + 4);
      const int stride = magic == 0xcafebabe ? 20 : magic == 0xcafebabf ? 32 : 0;
      if (stride && count <= 16 && header.size() >= 8 + static_cast<int>(count) * stride)
        for (quint32 n = 0; n < count; ++n) {
          const auto *entry = raw + 8 + n * static_cast<quint32>(stride);
          if (qFromBigEndian<quint32>(entry) == wanted) {
            const auto offset = stride == 20
                                    ? static_cast<quint64>(qFromBigEndian<quint32>(entry + 8))
                                    : qFromBigEndian<quint64>(entry + 8);
            if (offset < static_cast<quint64>(executable.size())) {
              slice_offset = static_cast<qint64>(offset);
              architecture = true;
            }
            break;
          }
        }
    }
  }
  if (!wanted || !architecture || !executable.seek(slice_offset))
    return R::failure(failure("mac_architecture"));
  const auto thin = executable.read(32);
  if (thin.size() != 32)
    return R::failure(failure("mac_macho_header"));
  const auto *thin_raw = reinterpret_cast<const uchar *>(thin.constData());
  if (qFromLittleEndian<quint32>(thin_raw) != 0xfeedfacf ||
      qFromLittleEndian<quint32>(thin_raw + 4) != wanted)
    return R::failure(failure("mac_macho_header"));
  const auto count = qFromLittleEndian<quint32>(thin_raw + 16),
             size = qFromLittleEndian<quint32>(thin_raw + 20);
  if (count > 4096 || size > 4 * 1024 * 1024)
    return R::failure(failure("mac_macho_commands"));
  const auto commands = executable.read(size);
  if (commands.size() != size)
    return R::failure(failure("mac_macho_commands"));
  qsizetype pos = 0;
  bool macos = false;
  for (quint32 n = 0; n < count; ++n) {
    if (pos > commands.size() - 8)
      return R::failure(failure("mac_macho_commands"));
    const auto *raw = reinterpret_cast<const uchar *>(commands.constData() + pos);
    const auto kind = qFromLittleEndian<quint32>(raw), length = qFromLittleEndian<quint32>(raw + 4);
    if (length < 8 || length > commands.size() - pos)
      return R::failure(failure("mac_macho_commands"));
    if (kind == 0x32) {
      if (length < 24 || qFromLittleEndian<quint32>(raw + 8) != 1)
        return R::failure(failure("mac_macho_platform"));
      macos = true;
    }
    if (kind == 0x24) {
      if (length < 16)
        return R::failure(failure("mac_macho_platform"));
      macos = true;
    }
    pos += length;
  }
  if (!macos || pos != commands.size())
    return R::failure(failure("mac_macho_platform"));
  const auto signed_ok = run("/usr/bin/codesign", {"--verify", "--deep", "--strict", app});
  if (!signed_ok)
    return R::failure(failure("mac_bundle_signature"));
  return R::success(true);
}
class MacInstaller final : public NativeInstaller {
public:
  QString install_identity() const override {
    // executable itself is a file; resolve via its enclosing Contents/MacOS
    // directory.
    return QDir(QCoreApplication::applicationDirPath() + "/../..").canonicalPath();
  }
  bool manual() const override {
    return true;
  } // Ad-hoc/Gatekeeper POC gate is intentionally closed.
  Result<bool, Error> install(Slot &slot, const Candidate &candidate, const QByteArray &,
                              std::function<bool()>) override {
    if (slot.record["phase"] == "manual_pending") {
      auto mount = slot.record["mount"].toString();
      if (mount.isEmpty()) {
        auto recovered = current_mount(slot.payload());
        if (!recovered)
          return Result<bool, Error>::failure(recovered.error());
        mount = recovered.value();
      }
      if (mount.isEmpty()) {
        slot.record["phase"] = "ready";
        auto saved = slot.save();
        if (!saved)
          return saved;
        return install(slot, candidate, QByteArray{}, {});
      }
      const auto verified = verify_macos_update_bundle(mount, candidate);
      if (!verified)
        return verified;
      const auto opened = run("/usr/bin/open", {mount});
      return opened ? Result<bool, Error>::success(true)
                    : Result<bool, Error>::failure(opened.error());
    }
    slot.record["phase"] = "manual_pending";
    slot.record["mount"] = "";
    auto saved = slot.save();
    if (!saved)
      return saved;
    const auto mounted =
        run("/usr/bin/hdiutil", {"attach", "-readonly", "-nobrowse", "-plist", slot.payload()});
    if (!mounted) {
      auto found = current_mount(slot.payload());
      if (found && found.value().isEmpty())
        slot.record["phase"] = "ready";
      else if (found)
        slot.record["mount"] = found.value();
      (void)slot.save();
      return Result<bool, Error>::failure(mounted.error());
    }
    const auto mount = mount_from_plist(mounted.value());
    if (mount.isEmpty())
      return Result<bool, Error>::failure(failure("已挂载但无法确认卷路径，需要恢复"));
    slot.record["mount"] = mount;
    saved = slot.save();
    if (!saved)
      return saved;
    const auto verified = verify_macos_update_bundle(mount, candidate);
    if (!verified) {
      (void)cancel_manual(slot);
      return verified;
    }
    const auto opened = run("/usr/bin/open", {mount});
    if (!opened)
      return Result<bool, Error>::failure(opened.error());
    return Result<bool, Error>::success(true); // ManualInstallPending, never installation success.
  }
  Result<bool, Error> cancel_manual(Slot &slot) override {
    auto found = current_mount(slot.payload());
    if (!found)
      return Result<bool, Error>::failure(found.error());
    const auto mount = found.value();
    if (mount.isEmpty()) {
      slot.record["phase"] = "ready";
      slot.record.remove("mount");
      return slot.save();
    }
    const auto detached = run("/usr/bin/hdiutil", {"detach", mount});
    if (!detached)
      return Result<bool, Error>::failure(
          failure("安装卷仍在使用，暂时无法清理", ErrorCode::path_not_writable));
    slot.record["phase"] = "ready";
    slot.record.remove("mount");
    return slot.save();
  }
};
std::unique_ptr<NativeInstaller> native_installer() {
  return std::make_unique<MacInstaller>();
}
} // namespace hdrshot::update
