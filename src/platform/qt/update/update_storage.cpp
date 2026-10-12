#include "platform/qt/update/update_storage.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <limits>
namespace hdrshot::update {
Error failure(const char *reason, ErrorCode code, Retryability retry) {
  return {code, "update", retry, {{"reason", reason}}};
}
Result<QJsonObject, Error> read_json(const QString &path) {
  QFile f(path);
  if (!f.exists())
    return Result<QJsonObject, Error>::success({});
  if (!f.open(QIODevice::ReadOnly) || f.size() > 1048576)
    return Result<QJsonObject, Error>::failure(failure("state_read"));
  QJsonParseError error;
  auto doc = QJsonDocument::fromJson(f.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !doc.isObject())
    return Result<QJsonObject, Error>::failure(failure("state_corrupt"));
  return Result<QJsonObject, Error>::success(doc.object());
}
Result<bool, Error> save_json(const QString &path, const QJsonObject &object) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    return Result<bool, Error>::failure(failure("state_directory"));
  QSaveFile f(path);
  f.setDirectWriteFallback(false);
  const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.flush())
    return Result<bool, Error>::failure(failure("state_write"));
  // QSaveFile's commit closes and atomically renames; flush the committed inode
  // and parent directory.
  if (!f.commit())
    return Result<bool, Error>::failure(failure("state_commit"));
  QFile committed(path);
  if (!committed.open(QIODevice::ReadWrite) || !durable_flush(committed) ||
      !durable_directory(QFileInfo(path).absolutePath()))
    return Result<bool, Error>::failure(failure("state_durability"));
  return Result<bool, Error>::success(true);
}
Result<QByteArray, Error> hash_file(const QString &path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return Result<QByteArray, Error>::failure(
        failure("payload_missing", ErrorCode::object_not_found));
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&f))
    return Result<QByteArray, Error>::failure(failure("payload_read"));
  return Result<QByteArray, Error>::success(hash.result().toHex());
}
Slot::Slot(QString r, QString o, QString c)
    : root(std::move(r)), owner(std::move(o)), channel(std::move(c)) {}
Result<bool, Error> Slot::acquire() {
  if (lock)
    return Result<bool, Error>::success(true);
  if (!QDir().mkpath(root))
    return Result<bool, Error>::failure(failure("slot_directory", ErrorCode::path_not_writable));
  lock = std::make_unique<QLockFile>(root + "/operation.lock");
  lock->setStaleLockTime(0);
  if (!lock->tryLock(0)) {
    lock.reset();
    return Result<bool, Error>::failure(
        failure("另一实例正在更新", ErrorCode::precondition_failed));
  }
  auto loaded = read_json(root + "/slot.json");
  if (!loaded) {
    release();
    return Result<bool, Error>::failure(loaded.error());
  }
  record = loaded.value();
  if (QFileInfo::exists(root + "/slot.json")) {
    bool valid = false;
    const auto generation = record["generation"].toString().toULongLong(&valid);
    const auto suffix = record["suffix"].toString(), phase = record["phase"].toString();
    if (!valid || generation == 0 || record["owner"].toString().isEmpty() ||
        record["channel"].toString().isEmpty() || record["key"].toString().isEmpty() ||
        !(suffix == ".part" || suffix == ".dmg" || suffix == ".zip") ||
        !(phase == "empty" || phase == "downloading" || phase == "ready" || phase == "reserved" ||
          phase == "installing" || phase == "manual_pending" || phase == "cleanup_blocked")) {
      release();
      return Result<bool, Error>::failure(failure("slot_record_corrupt"));
    }
  }
  return recover();
}
Result<bool, Error> Slot::ensure_single_payload(bool repair) {
  QStringList files;
  for (const auto &suffix : {QString(".part"), QString(".zip"), QString(".dmg")}) {
    const auto path = root + "/payload" + suffix;
    if (QFileInfo(path).isSymLink())
      return Result<bool, Error>::failure(failure("payload_symlink"));
    if (QFileInfo::exists(path))
      files.push_back(path);
  }
  if (files.size() <= 1)
    return Result<bool, Error>::success(true);
  const auto phase = record["phase"].toString();
  const bool complete = record["suffix"] == ".zip" || record["suffix"] == ".dmg";
  if (repair && lock && complete && (phase == "ready" || phase == "manual_pending") &&
      files.size() == 2 && files.contains(payload()) && files.contains(root + "/payload.part")) {
    if (QFile::remove(root + "/payload.part") && durable_directory(root))
      return Result<bool, Error>::success(true);
    return Result<bool, Error>::failure(
        failure("orphan_cleanup_blocked", ErrorCode::path_not_writable));
  }
  return Result<bool, Error>::failure(failure("multiple_payloads", ErrorCode::path_not_writable));
}
Result<bool, Error> Slot::recover() {
  if (!lock)
    return Result<bool, Error>::failure(failure("slot_unlocked"));
  const auto phase = record["phase"].toString();
  if (phase.isEmpty() || phase == "empty" || phase == "cleanup_blocked") {
    auto removed = remove_payload();
    if (!removed)
      return removed;
  } else if (phase != "downloading") {
    auto unique = ensure_single_payload(true);
    if (!unique)
      return unique;
  }
  if (phase == "reserved" || phase == "installing") {
    if (process_alive(record["parent_pid"].toVariant().toLongLong(),
                      record["parent_creation"].toString()) ||
        process_alive(record["helper_pid"].toVariant().toLongLong(),
                      record["helper_creation"].toString()))
      return Result<bool, Error>::failure(
          failure("安装事务仍在运行", ErrorCode::precondition_failed));
    // A journal must be recovered by the independent native recovery tool
    // before the slot is reusable.
    const auto journal = record["journal"].toString();
    if (!journal.isEmpty() && QFileInfo::exists(journal)) {
      const auto recovered = read_json(journal);
      if (!recovered ||
          !(recovered.value()["phase"] == "rolled_back" ||
            recovered.value()["phase"] == "complete" || recovered.value()["phase"] == "cancelled"))
        return Result<bool, Error>::failure(
            failure("需要运行独立恢复工具", ErrorCode::state_inconsistent));
    }
    record["phase"] = "ready";
    record.remove("transaction");
    record.remove("nonce");
    return save();
  }
  if (phase == "downloading") {
    // Holding the operation lock proves the previous writer is dead; do not
    // reclaim on age alone.
    auto removed = remove_payload();
    if (!removed)
      return removed;
    record["phase"] = "empty";
    return save();
  }
  return Result<bool, Error>::success(true);
}
QString Slot::payload() const {
  return root + "/payload" + record["suffix"].toString(".part");
}
Result<bool, Error> Slot::remove_payload() {
  if (!lock)
    return Result<bool, Error>::failure(failure("slot_unlocked"));
  if (record["phase"] == "manual_pending")
    return Result<bool, Error>::failure(
        failure("安装包仍已挂载，请完成安装或取消", ErrorCode::path_not_writable));
  // Exact owned filenames only; all complete/partial variants count as the one
  // global slot.
  for (const auto &suffix : {QString(".part"), QString(".zip"), QString(".dmg")}) {
    const auto path = root + "/payload" + suffix;
    if (QFileInfo(path).isSymLink())
      return Result<bool, Error>::failure(failure("payload_symlink"));
    if (QFileInfo::exists(path) && !QFile::remove(path))
      return Result<bool, Error>::failure(
          failure("旧安装包清理失败", ErrorCode::path_not_writable));
  }
  if (!durable_directory(root))
    return Result<bool, Error>::failure(failure("cleanup_durability"));
  return Result<bool, Error>::success(true);
}
Result<bool, Error> Slot::reserve(const Candidate &c) {
  auto acquired = acquire();
  if (!acquired)
    return acquired;
  auto removed = remove_payload();
  if (!removed)
    return removed;
  const auto previous = record["generation"].toString().toULongLong();
  if (previous == std::numeric_limits<quint64>::max())
    return Result<bool, Error>::failure(failure("slot_generation_exhausted"));
  const auto generation = previous + 1;
  record = {{"generation", QString::number(generation)},
            {"owner", owner},
            {"channel", channel},
            {"key", QString::fromStdString(c.key())},
            {"version", QString::fromStdString(c.version.text())},
            {"phase", "downloading"},
            {"suffix", ".part"},
            {"parent_pid", QString::number(QCoreApplication::applicationPid())},
            {"parent_creation", process_identity(QCoreApplication::applicationPid())}};
  return save();
}
Result<bool, Error> Slot::save() {
  return save_json(root + "/slot.json", record);
}
bool Slot::matches(const Receipt &r) const {
  return record["owner"] == owner && record["channel"] == channel &&
         record["key"].toString().toStdString() == r.key &&
         record["generation"].toString().toULongLong() == r.generation;
}
Result<Receipt, Error> Slot::verified(const Candidate &c) {
  auto unique = ensure_single_payload();
  if (!unique)
    return Result<Receipt, Error>::failure(unique.error());
  if (record["owner"] != owner || record["channel"] != channel ||
      record["key"].toString().toStdString() != c.key() ||
      !(record["phase"] == "ready" || record["phase"] == "manual_pending"))
    return Result<Receipt, Error>::failure(failure("receipt_stale"));
  const auto path = payload();
  if (QFileInfo(path).isSymLink() ||
      static_cast<std::uint64_t>(QFileInfo(path).size()) != c.asset_size)
    return Result<Receipt, Error>::failure(failure("payload_missing", ErrorCode::object_not_found));
  auto hash = hash_file(path);
  if (!hash || hash.value().toStdString() != c.sha256)
    return Result<Receipt, Error>::failure(failure("payload_corrupt"));
  return Result<Receipt, Error>::success({c.key(), record["generation"].toString().toULongLong()});
}
void Slot::release() {
  lock.reset();
}
} // namespace hdrshot::update
namespace hdrshot::update {
Result<bool, Error> register_cleanup(const QString &root, const QJsonObject &j, qint64 pid) {
  const auto transaction = j["transaction"].toString();
  if (transaction.isEmpty() || transaction.contains('/') || transaction.contains('\\'))
    return Result<bool, Error>::failure(failure("cleanup_transaction"));
  QJsonObject record{
      {"protocol", 1},
      {"transaction", transaction},
      {"target", j["target"]},
      {"backup", j["backup"]},
      {"staging", j["staging"]},
      {"runtime", root + "/helpers/" + transaction},
      {"helper_pid", QString::number(pid)},
      {"helper_creation", process_identity(pid)},
      {"preparing", pid == 0 && (j["phase"] == "preparing" || j["phase"] == "prepared")},
      {"parent_pid", j["parent_pid"]},
      {"parent_creation", j["parent_creation"]}};
  return save_json(root + "/cleanup/" + transaction + ".json", record);
}
Result<bool, Error> cleanup_managed(const QString &root) {
  const auto records =
      QDir(root + "/cleanup").entryList({"*.json"}, QDir::Files | QDir::NoSymLinks);
  for (const auto &name : records) {
    const auto path = root + "/cleanup/" + name;
    auto loaded = read_json(path);
    if (!loaded)
      return Result<bool, Error>::failure(loaded.error());
    auto record = loaded.value();
    if (record["protocol"] != 1)
      return Result<bool, Error>::failure(failure("cleanup_protocol"));
    const auto transaction = record["transaction"].toString(), target = record["target"].toString(),
               runtime = record["runtime"].toString();
    if (name != transaction + ".json" || transaction.isEmpty() || transaction.contains('/') ||
        transaction.contains('\\') ||
        (!runtime.isEmpty() && runtime != root + "/helpers/" + transaction))
      return Result<bool, Error>::failure(failure("cleanup_identity"));
    if (process_alive(record["helper_pid"].toString().toLongLong(),
                      record["helper_creation"].toString()))
      continue;
    if (record["preparing"].toBool()) {
      if (process_alive(record["parent_pid"].toString().toLongLong(),
                        record["parent_creation"].toString()))
        continue;
      // A dead preparer cannot have armed its helper: spawning replaces this
      // record.
      record["terminal"] = true;
      record["preparing"] = false;
      auto saved = save_json(path, record);
      if (!saved)
        return saved;
    }
    if (!record["terminal"].toBool()) {
      auto journal = read_json(runtime + "/transaction.json");
      if (!journal ||
          !(journal.value()["phase"] == "complete" || journal.value()["phase"] == "rolled_back" ||
            journal.value()["phase"] == "cancelled"))
        continue;
      record["terminal"] = true;
      auto saved = save_json(path, record);
      if (!saved)
        return saved;
    }
    for (const auto &field : {QString("backup"), QString("staging"), QString("runtime")}) {
      const auto directory = record[field].toString();
      if (directory.isEmpty())
        continue;
      const auto expected =
          field == "runtime"
              ? root + "/helpers/" + transaction
              : target + (field == "backup" ? "/.seriousshot-backup-" : "/.seriousshot-stage-") +
                    transaction;
      if (directory != expected || QFileInfo(directory).isSymLink())
        return Result<bool, Error>::failure(failure("cleanup_path"));
      if (QFileInfo::exists(directory) && !QDir(directory).removeRecursively())
        return Result<bool, Error>::failure(
            failure("受管更新文件清理失败，稍后重试", ErrorCode::path_not_writable));
      record[field] = "";
      auto saved = save_json(path, record);
      if (!saved)
        return saved;
    }
    if (!QFile::remove(path))
      return Result<bool, Error>::failure(failure("cleanup_receipt_remove"));
  }
  return Result<bool, Error>::success(true);
}
} // namespace hdrshot::update
