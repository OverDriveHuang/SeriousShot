#include "platform/qt/update/update_catalog.hpp"
#include "platform/windows/update/windows_update_native.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QSet>
#include <QThread>
#include <cstdio>
#include <windows.h>
using namespace hdrshot;
using namespace hdrshot::update;
namespace {
unsigned boundary{};
Result<bool, Error> persist(const QString &path, QJsonObject &j, const QString &action,
                            const QString &file = {}) {
  j["intent"] = action;
  j["file"] = file;
  auto result = save_json(path, j);
#if HDRSHOT_UPDATE_TESTING
  bool ok = false;
  const auto fail = qEnvironmentVariable("SERIOUSSHOT_UPDATE_FAIL_AFTER").toUInt(&ok);
  if (ok && ++boundary == fail)
    ExitProcess(91);
#endif
  return result;
}
Result<bool, Error> move_checked(const QString &from, const QString &to) {
  if (QFileInfo(from).isSymLink() || QFileInfo(to).isSymLink() || QFileInfo::exists(to) ||
      !QDir().mkpath(QFileInfo(to).absolutePath()) || !windows_move(from, to))
    return Result<bool, Error>::failure(failure("replace_move"));
  return Result<bool, Error>::success(true);
}
Result<bool, Error> restore(const QString &path, QJsonObject &j, const Package &old,
                            const Package &fresh) {
  const auto target = j["target"].toString(), backup = j["backup"].toString(),
             entry = old.entrypoint;
  if (!windows_install_quiet(target + "/" + entry, backup + "/" + entry))
    return Result<bool, Error>::failure(failure("rollback_requires_all_instances_exit"));
  auto result = persist(path, j, "rollback_withdraw_exe", entry);
  if (!result)
    return result;
  const auto live = target + "/" + entry;
  if (QFileInfo::exists(live) && QFileInfo::exists(backup + "/" + entry)) {
    const auto quarantine = j["staging"].toString() + "/failed-entry.exe";
    if (QFileInfo::exists(quarantine))
      return Result<bool, Error>::failure(failure("rollback_duplicate_entry"));
    result = move_checked(live, quarantine);
    if (!result)
      return result;
  }
  QSet<QString> old_names;
  for (const auto &f : old.files)
    old_names.insert(f.path);
  for (const auto &f : fresh.files) {
    if (f.path == entry)
      continue;
    const auto livefile = target + "/" + f.path, previous = backup + "/" + f.path;
    if (QFileInfo::exists(previous)) {
      result = persist(path, j, "rollback_file", f.path);
      if (!result)
        return result;
      if (QFileInfo::exists(livefile)) {
        auto hash = hash_file(livefile);
        if (!hash || hash.value() != f.sha256)
          return Result<bool, Error>::failure(failure("rollback_unowned_file"));
        if (!QFile::remove(livefile))
          return Result<bool, Error>::failure(failure("rollback_remove"));
      }
      result = move_checked(previous, livefile);
      if (!result)
        return result;
    } else if (!old_names.contains(f.path) && QFileInfo::exists(livefile)) {
      auto hash = hash_file(livefile);
      if (!hash || hash.value() != f.sha256 || !QFile::remove(livefile))
        return Result<bool, Error>::failure(failure("rollback_new_file"));
    }
  }
  // Restore files removed in the new manifest, plus the original manifest
  // itself.
  for (const auto &f : old.files) {
    if (f.path == entry)
      continue;
    const auto previous = backup + "/" + f.path;
    if (QFileInfo::exists(previous)) {
      result = persist(path, j, "rollback_old_file", f.path);
      if (!result)
        return result;
      result = move_checked(previous, target + "/" + f.path);
      if (!result)
        return result;
    }
  }
  if (QFileInfo::exists(backup + "/package_manifest.json")) {
    result = persist(path, j, "rollback_manifest");
    if (!result)
      return result;
    if (QFileInfo::exists(target + "/package_manifest.json") &&
        !QFile::remove(target + "/package_manifest.json"))
      return Result<bool, Error>::failure(failure("rollback_manifest_remove"));
    result = move_checked(backup + "/package_manifest.json", target + "/package_manifest.json");
    if (!result)
      return result;
  }
  // Verify every old dependency before publishing the old EXE last.
  for (const auto &f : old.files) {
    const auto file =
        (f.path == entry && QFileInfo::exists(backup + "/" + entry) ? backup : target) + "/" +
        f.path;
    auto h = hash_file(file);
    if (!h || h.value() != f.sha256)
      return Result<bool, Error>::failure(failure("rollback_verify"));
  }
  if (QFileInfo::exists(backup + "/" + entry)) {
    result = persist(path, j, "rollback_publish_exe");
    if (!result)
      return result;
    result = move_checked(backup + "/" + entry, live);
    if (!result)
      return result;
  }
  j["phase"] = "rolled_back";
  j.remove("intent");
  return save_json(path, j);
}
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const auto args = app.arguments();
  if (args.size() != 3 || (args[1] != "--apply" && args[1] != "--recover")) {
    std::fprintf(stderr, "usage: SeriousShotUpdater --apply|--recover transaction.json\n");
    return 2;
  }
  const bool recovery = args[1] == "--recover";
  const auto journal = QFileInfo(args[2]).canonicalFilePath();
  if (journal.isEmpty() ||
      QFileInfo(journal).absolutePath() != QCoreApplication::applicationDirPath())
    return 3;
  auto loaded = read_json(journal);
  if (!loaded)
    return 4;
  auto j = loaded.value();
  const auto envelope = QByteArray::fromBase64(j["envelope"].toString().toLatin1());
  const auto catalog = verify_catalog(envelope, compiled_trust());
  if (!catalog)
    return 5;
  std::optional<Candidate> candidate;
  for (const auto &c : catalog.value().targets)
    if (c.key() == j["candidate"].toString().toStdString())
      candidate = c;
  if (!candidate || candidate->platform != "windows" || candidate->draft || candidate->prerelease ||
      candidate->channel != j["channel"].toString().toStdString() || j["protocol"] != 1)
    return 6;
  const auto target = j["target"].toString(), backup = j["backup"].toString(),
             staging = j["staging"].toString(), exe = j["exe"].toString(),
             nonce = j["nonce"].toString(), transaction = j["transaction"].toString();
  if (nonce.isEmpty() || transaction.isEmpty() || QFileInfo(target).canonicalFilePath() != target ||
      exe != target + "/" + j["old_manifest"].toObject()["entrypoint"].toString() ||
      backup != target + "/.seriousshot-backup-" + transaction ||
      staging != target + "/.seriousshot-stage-" + transaction)
    return 7;
  // Manifest copies are frozen in the journal; parse them through the same
  // bounded validator.
  const auto runtime = QCoreApplication::applicationDirPath();
  const auto oldroot = runtime + "/old-description", newroot = runtime + "/new-description";
  if (!save_json(oldroot + "/package_manifest.json", j["old_manifest"].toObject()) ||
      !save_json(newroot + "/package_manifest.json", j["new_manifest"].toObject()))
    return 8;
  auto oldc = *candidate;
  auto oldversion =
      Version::parse(j["old_manifest"].toObject()["version"].toString().toStdString());
  if (!oldversion)
    return 9;
  oldc.version = oldversion.value();
  auto old = package_manifest(oldroot, oldc), fresh = package_manifest(newroot, *candidate);
  if (!old || !fresh || old.value().entrypoint != fresh.value().entrypoint)
    return 10;
  const auto pid = j["parent_pid"].toString().toLongLong();
  const auto creation = j["parent_creation"].toString();
  if (!recovery) {
    if (!process_alive(pid, creation))
      return 11;
    // Bind target to the real parent's image; the catalog never chooses an
    // install path.
    HANDLE parent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    wchar_t image[32768];
    DWORD size = 32768;
    const bool matches =
        parent && QueryFullProcessImageNameW(parent, 0, image, &size) &&
        QDir::fromNativeSeparators(QString::fromWCharArray(image, static_cast<int>(size)))
                .compare(exe, Qt::CaseInsensitive) == 0;
    if (parent)
      CloseHandle(parent);
    if (!matches)
      return 12;
    const auto event_name =
        QString("Local\\Overdrive.SeriousShot.Update.Arm.") + transaction + "." + nonce;
    if (j["event_name"] != event_name)
      return 21;
    HANDLE arm_event =
        OpenEventW(SYNCHRONIZE, FALSE, reinterpret_cast<LPCWSTR>(event_name.utf16()));
    if (!arm_event)
      return 22;
    struct CloseEvent {
      HANDLE handle;
      ~CloseEvent() {
        CloseHandle(handle);
      }
    } close_event{arm_event};
    if (!save_json(runtime + "/ready.json", {{"nonce", nonce},
                                             {"pid", QString::number(app.applicationPid())},
                                             {"creation", process_identity(app.applicationPid())}}))
      return 13;
    while (process_alive(pid, creation)) {
      auto canceled = read_json(runtime + "/cancel.json");
      if (canceled && canceled.value()["nonce"] == nonce) {
        j["phase"] = "cancelled";
        (void)save_json(journal, j);
        return 0;
      }
      QThread::msleep(100);
    }
    const auto armed = read_json(runtime + "/armed.json");
    const auto canceled = read_json(runtime + "/cancel.json");
    if (WaitForSingleObject(arm_event, 0) != WAIT_OBJECT_0 || !armed ||
        armed.value()["nonce"] != nonce || armed.value()["transaction"] != transaction ||
        armed.value()["generation"] != j["generation"] ||
        (canceled && canceled.value()["nonce"] == nonce)) {
      j["phase"] = "cancelled";
      (void)save_json(journal, j);
      return 0;
    }
  } else if (process_alive(pid, creation))
    return 14;
  Slot slot(j["slot"].toString(), j["owner"].toString(), j["channel"].toString());
  slot.lock = std::make_unique<QLockFile>(slot.root + "/operation.lock");
  slot.lock->setStaleLockTime(0);
  if (!slot.lock->tryLock(10000))
    return 15;
  auto slotstate = read_json(slot.root + "/slot.json");
  if (!slotstate)
    return 16;
  slot.record = slotstate.value();
  if (slot.record["generation"] != j["generation"] || slot.record["transaction"] != transaction ||
      slot.record["nonce"] != nonce || slot.record["owner"] != j["owner"])
    return 17;
  if (!slot.ensure_single_payload())
    return 23;
  auto receipt = slot.verified(*candidate); // reserved phase deliberately not
                                            // accepted by normal Ready verifier
  if (!recovery) {
    const auto hash = hash_file(slot.payload());
    if (!hash || hash.value().toStdString() != candidate->sha256 ||
        static_cast<std::uint64_t>(QFileInfo(slot.payload()).size()) != candidate->asset_size ||
        !verify_tree(staging, fresh.value()))
      return 18;
  }
  slot.record["phase"] = "installing";
  slot.record["helper_pid"] = QString::number(app.applicationPid());
  slot.record["helper_creation"] = process_identity(app.applicationPid());
  if (!slot.save())
    return 19;
  Result<bool, Error> result = Result<bool, Error>::success(true);
  if (j["phase"] == "complete") { /* Already committed: never roll back a
                                     successful update. */
  } else if (recovery || j["phase"] != "prepared")
    result = restore(journal, j, old.value(), fresh.value());
  else {
    // Every GUI/CLI process from this installation must exit; no process is
    // killed.
    int wait = 0;
    while (!windows_install_quiet(exe) && wait++ < 1200)
      QThread::msleep(100);
    if (!windows_install_quiet(exe))
      result = Result<bool, Error>::failure(failure("other_install_instance"));
    if (result) {
      j["phase"] = "replacing";
      result = persist(journal, j, "withdraw_exe", old.value().entrypoint);
    }
    if (result)
      result = move_checked(exe, backup + "/" + old.value().entrypoint);
    // A launch begun before EXE withdrawal must still be observed. No DLL is
    // touched before this second barrier.
    wait = 0;
    while (result && !windows_install_quiet(exe, backup + "/" + old.value().entrypoint) &&
           wait++ < 1200)
      QThread::msleep(100);
    if (result && !windows_install_quiet(exe, backup + "/" + old.value().entrypoint))
      result = Result<bool, Error>::failure(failure("launch_race"));
    for (const auto &f : old.value().files) {
      if (!result || f.path == old.value().entrypoint)
        continue;
      result = persist(journal, j, "backup_file", f.path);
      if (result)
        result = move_checked(target + "/" + f.path, backup + "/" + f.path);
    }
    if (result) {
      result = persist(journal, j, "backup_manifest");
      if (result)
        result = move_checked(target + "/package_manifest.json", backup + "/package_manifest.json");
    }
    for (const auto &f : fresh.value().files) {
      if (!result || f.path == fresh.value().entrypoint)
        continue;
      result = persist(journal, j, "publish_file", f.path);
      if (result)
        result = move_checked(staging + "/" + f.path, target + "/" + f.path);
    }
    if (result) {
      result = persist(journal, j, "publish_manifest");
      if (result)
        result =
            move_checked(staging + "/package_manifest.json", target + "/package_manifest.json");
    }
    if (result) {
      for (const auto &f : fresh.value().files) {
        const auto p = (f.path == fresh.value().entrypoint ? staging : target) + "/" + f.path;
        auto h = hash_file(p);
        if (!h || h.value() != f.sha256) {
          result = Result<bool, Error>::failure(failure("new_tree_verify"));
          break;
        }
      }
    }
    if (result) {
      result = persist(journal, j, "publish_exe");
      if (result)
        result = move_checked(staging + "/" + fresh.value().entrypoint, exe);
    }
    qint64 launched = 0;
    if (result) {
      j["phase"] = "awaiting_receipt";
      result = persist(journal, j, "launch");
      QProcess process;
      process.setProgram(exe);
      process.setWorkingDirectory(target);
      process.setArguments({"--update-receipt", runtime + "/started.json", nonce});
      if (result && !process.startDetached(&launched))
        result = Result<bool, Error>::failure(failure("new_start"));
    }
    if (result) {
      bool accepted = false;
      for (int n = 0; n < 600; ++n) {
        auto started = read_json(runtime + "/started.json");
        if (started && started.value()["nonce"] == nonce &&
            started.value()["version"].toString().toStdString() == candidate->version.text() &&
            started.value()["exe"].toString().compare(exe, Qt::CaseInsensitive) == 0 &&
            started.value()["pid"].toString().toLongLong() == launched &&
            process_alive(launched, started.value()["creation"].toString())) {
          accepted = true;
          break;
        }
        QThread::msleep(100);
      }
      if (!accepted)
        result = Result<bool, Error>::failure(failure("startup_receipt_timeout"));
    }
    if (result) {
      j["phase"] = "complete";
      result = save_json(journal, j);
    } else {
      const auto restored = restore(journal, j, old.value(), fresh.value());
      if (!restored)
        result = restored;
    }
  }
  const bool complete = j["phase"] == "complete", rolled = j["phase"] == "rolled_back";
  if (complete) {
    slot.record["phase"] = "ready";
    auto removed = slot.remove_payload();
    slot.record["phase"] = removed ? "empty" : "cleanup_blocked";
    (void)slot.save();
  } else if (rolled) {
    slot.record["phase"] = "ready";
    slot.record.remove("transaction");
    slot.record.remove("nonce");
    (void)slot.save();
    qint64 restarted;
    QProcess::startDetached(exe, {}, target, &restarted);
  } else {
    slot.record["phase"] = "installing";
    (void)slot.save();
  }
  return complete || rolled ? 0 : 20;
}
