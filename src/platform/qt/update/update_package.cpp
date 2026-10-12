#include "platform/qt/update/update_package.hpp"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <archive.h>
#include <archive_entry.h>
#include <cstring>
namespace hdrshot::update {
bool safe_relative(const QString &p) {
  if (p.isEmpty() || p.size() > 240 || p.startsWith('/') || p.contains('\\') || p.contains(':') ||
      p.contains(QChar(0)))
    return false;
  for (const auto &component : p.split('/')) {
    if (component.isEmpty() || component == "." || component == ".." || component.endsWith('.') ||
        component.endsWith(' '))
      return false;
    const auto stem = component.section('.', 0, 0).toUpper();
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || stem == "CLOCK$" ||
        (stem.size() == 4 && (stem.startsWith("COM") || stem.startsWith("LPT")) && stem[3] >= '1' &&
         stem[3] <= '9'))
      return false;
  }
  return true;
}
Result<Package, Error> package_manifest(const QString &root, const Candidate &c,
                                        const QString &manifest_path) {
  using R = Result<Package, Error>;
  auto read = read_json(manifest_path.isEmpty() ? root + "/package_manifest.json" : manifest_path);
  if (!read)
    return R::failure(read.error());
  const auto m = read.value();
  if (m["product"] != "SeriousShot" || m["version"].toString().toStdString() != c.version.text() ||
      m["platform"].toString().toStdString() != c.platform ||
      m["architecture"].toString().toStdString() != c.architecture ||
      m["installer_protocol"] != 1 || !m["files"].isArray())
    return R::failure(failure("package_identity"));
  Package p;
  p.raw = m;
  p.entrypoint = m["entrypoint"].toString();
  if (!safe_relative(p.entrypoint) || (c.platform == "windows" && p.entrypoint.contains('/')))
    return R::failure(failure("package_entrypoint"));
  QSet<QString> seen;
  qint64 total = 0;
  bool has_entry = false;
  for (const auto &value : m["files"].toArray()) {
    const auto f = value.toObject();
    ManagedFile file{f["path"].toString(), f["sha256"].toString().toLatin1(),
                     f["size"].toVariant().toLongLong()};
    if (!safe_relative(file.path) || seen.contains(file.path.toCaseFolded()) ||
        file.sha256.size() != 64 || file.size < 0 || file.size > 1073741824 ||
        file.path == "package_manifest.json")
      return R::failure(failure("package_file"));
    const auto hash = QByteArray::fromHex(file.sha256);
    if (hash.size() != 32 || hash.toHex() != file.sha256)
      return R::failure(failure("package_hash"));
    seen.insert(file.path.toCaseFolded());
    total += file.size;
    has_entry |= file.path == p.entrypoint;
    p.files.push_back(file);
    if (total > 4LL * 1073741824 || p.files.size() > 4096)
      return R::failure(failure("package_limit"));
  }
  if (!has_entry || m["unpacked_size"].toVariant().toLongLong() != total)
    return R::failure(failure("package_total"));
  return R::success(p);
}
Result<bool, Error> verify_tree(const QString &root, const Package &p) {
  for (const auto &f : p.files) {
    const auto full = root + "/" + f.path;
    const QFileInfo info(full);
    if (info.isSymLink() || !info.isFile() || info.size() != f.size)
      return Result<bool, Error>::failure(failure("managed_file_size"));
    auto hash = hash_file(full);
    if (!hash || hash.value() != f.sha256)
      return Result<bool, Error>::failure(failure("managed_file_hash"));
  }
  return Result<bool, Error>::success(true);
}
Result<Package, Error> extract_zip(const QString &archive_path, const QString &staging,
                                   const Candidate &c) {
  using R = Result<Package, Error>;
  if (QFileInfo::exists(staging) || !QDir().mkpath(staging))
    return R::failure(failure("staging_exists"));
  auto *reader = archive_read_new();
  struct Close {
    archive *p;
    ~Close() {
      archive_read_free(p);
    }
  } close{reader};
  archive_read_support_format_zip(reader);
  archive_read_support_filter_none(reader);
#ifdef Q_OS_WIN
  if (archive_read_open_filename_w(reader, reinterpret_cast<const wchar_t *>(archive_path.utf16()),
                                   65536) != ARCHIVE_OK)
#else
  if (archive_read_open_filename(reader, QFile::encodeName(archive_path).constData(), 65536) !=
      ARCHIVE_OK)
#endif
    return R::failure(failure("zip_open"));
  QSet<QString> seen, files;
  quint64 total = 0;
  unsigned count = 0;
  archive_entry *entry = nullptr;
  int status;
  while ((status = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
    const auto *raw = archive_entry_pathname_utf8(entry);
    if (!raw)
      raw = archive_entry_pathname(entry);
    if (!raw || ++count > 4097)
      return R::failure(failure("zip_count_encoding"));
    QString name = QString::fromUtf8(raw);
    const bool directory = archive_entry_filetype(entry) == AE_IFDIR;
    if (directory && name.endsWith('/'))
      name.chop(1);
    if (!safe_relative(name) || seen.contains(name.toCaseFolded()) ||
        archive_entry_symlink(entry) || archive_entry_hardlink(entry) ||
        archive_entry_is_encrypted(entry) ||
        (!directory && archive_entry_filetype(entry) != AE_IFREG))
      return R::failure(failure("zip_path_type"));
    seen.insert(name.toCaseFolded());
    const auto size = archive_entry_size(entry);
    if (size < 0 || size > 1073741824 || (name == "package_manifest.json" && size > 1048576))
      return R::failure(failure("zip_entry_limit"));
    total += static_cast<quint64>(size);
    if (total > 4ULL * 1073741824)
      return R::failure(failure("zip_total_limit"));
    if (directory) {
      if (archive_read_data_skip(reader) != ARCHIVE_OK)
        return R::failure(failure("zip_directory_data"));
      continue;
    }
    files.insert(name);
    const auto destination = staging + "/" + name;
    if (!QDir().mkpath(QFileInfo(destination).absolutePath()))
      return R::failure(failure("zip_mkdir"));
    QFile out(destination);
    if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly))
      return R::failure(failure("zip_create"));
    char buffer[65536];
    qint64 written = 0;
    la_ssize_t n;
    while ((n = archive_read_data(reader, buffer, sizeof(buffer))) > 0) {
      written += n;
      if (written > size || out.write(buffer, n) != n)
        return R::failure(failure("zip_size_write"));
    }
    if (n < 0 || written != size || !durable_flush(out))
      return R::failure(failure("zip_crc_flush"));
  }
  if (status != ARCHIVE_EOF || archive_read_has_encrypted_entries(reader) > 0 ||
      !files.contains("package_manifest.json"))
    return R::failure(failure("zip_truncated_encrypted"));
  auto manifest = package_manifest(staging, c);
  if (!manifest)
    return manifest;
  if (files.size() != manifest.value().files.size() + 1)
    return R::failure(failure("zip_unmanaged_entry"));
  for (const auto &f : manifest.value().files)
    if (!files.contains(f.path))
      return R::failure(failure("zip_manifest_entry"));
  auto verified = verify_tree(staging, manifest.value());
  return verified ? manifest : R::failure(verified.error());
}
} // namespace hdrshot::update
