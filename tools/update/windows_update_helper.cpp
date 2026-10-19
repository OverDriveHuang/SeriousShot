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
QString current_action;
QString current_file;
void test_trace(const QString &event) {
#if HDRSHOT_UPDATE_TESTING
  if (qEnvironmentVariableIsSet("SERIOUSSHOT_UPDATE_TRACE")) {
    QFile trace(QCoreApplication::applicationDirPath() + "/test-boundaries.txt");
    if (trace.open(QIODevice::Append))
      trace.write((event + "\n").toUtf8());
  }
#else
  Q_UNUSED(event);
#endif
}
void mutation_boundary(const char *operation) {
#if HDRSHOT_UPDATE_TESTING
  static unsigned mutation{};
  ++mutation;
  test_trace(QString("mutation %1 %2 %3 %4").arg(mutation).arg(QString::fromLatin1(operation), current_action, current_file));
  bool ok = false;
  const auto fail = qEnvironmentVariable("SERIOUSSHOT_UPDATE_FAIL_MUTATION").toUInt(&ok);
  if (ok && mutation == fail)
    ExitProcess(91);
#else
  Q_UNUSED(operation);
#endif
}
Result<bool, Error> checkpoint(const QString &path, const QJsonObject &j) {
#if HDRSHOT_UPDATE_TESTING
  if (qEnvironmentVariable("SERIOUSSHOT_UPDATE_FAIL_CHECKPOINT") == j["phase"].toString())
    return Result<bool, Error>::failure(failure("injected_checkpoint"));
#endif
  auto result = save_json(path, j);
  if (result)
    test_trace("checkpoint " + j["phase"].toString());
  return result;
}
Result<bool, Error> terminal_save(const QString &path, const QJsonObject &j) {
#if HDRSHOT_UPDATE_TESTING
  if (qEnvironmentVariable("SERIOUSSHOT_UPDATE_FAIL_TERMINAL") == j["phase"].toString())
    return Result<bool, Error>::failure(failure("injected_terminal_commit"));
#endif
  return checkpoint(path,j);
}
Result<bool, Error> track(QJsonObject &j, const QString &action,
                            const QString &file = {}) {
  current_action = action;
  current_file = file;
  j["intent"] = action;
  j["file"] = file;
#if HDRSHOT_UPDATE_TESTING
  bool ok = false;
  const auto fail = qEnvironmentVariable("SERIOUSSHOT_UPDATE_FAIL_AFTER").toUInt(&ok);
  if (ok && ++boundary == fail)
    ExitProcess(91);
#endif
  return Result<bool, Error>::success(true);
}
Result<bool, Error> move_checked(const QString &from, const QString &to) {
  if (QFileInfo(from).isSymLink() || QFileInfo(to).isSymLink() || QFileInfo::exists(to) ||
      !QDir().mkpath(QFileInfo(to).absolutePath()))
    return Result<bool, Error>::failure(failure("replace_move"));
  mutation_boundary("move_before");
  auto result = windows_move(from,to);
  if (result)
    mutation_boundary("move_after");
  return result;
}
bool remove_checked(const QString &path) {
  mutation_boundary("remove_before");
  const bool removed = QFile::remove(path);
  if (removed)
    mutation_boundary("remove_after");
  return removed;
}
Result<bool, Error> restore(const QString &path, QJsonObject &j, const Package &old,
                            const Package &fresh) {
  const auto target = j["target"].toString(), backup = j["backup"].toString(),
             entry = old.entrypoint;
  if (!windows_install_quiet(target + "/" + entry, backup + "/" + entry))
    return Result<bool, Error>::failure(failure("rollback_requires_all_instances_exit"));
  // Frozen manifests and the verified live/backup trees describe recovery.
  // No per-file journal progress (including legacy intent/file) is required.
  auto result = track(j, "rollback_withdraw_exe", entry);
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
      result = track(j, "rollback_file", f.path);
      if (!result)
        return result;
      if (QFileInfo::exists(livefile)) {
        auto hash = hash_file(livefile);
        if (!hash || hash.value() != f.sha256)
          return Result<bool, Error>::failure(failure("rollback_unowned_file"));
        if (!remove_checked(livefile))
          return Result<bool, Error>::failure(failure("rollback_remove"));
      }
      result = move_checked(previous, livefile);
      if (!result)
        return result;
    } else if (!old_names.contains(f.path) && QFileInfo::exists(livefile)) {
      // This removal had no legacy diagnostic boundary; still identify its
      // actual before/after mutation probes and any failure precisely.
      current_action = "rollback_new_file";
      current_file = f.path;
      j["intent"] = current_action;
      j["file"] = f.path;
      auto hash = hash_file(livefile);
      if (!hash || hash.value() != f.sha256 || !remove_checked(livefile))
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
      result = track(j, "rollback_old_file", f.path);
      if (!result)
        return result;
      result = move_checked(previous, target + "/" + f.path);
      if (!result)
        return result;
    }
  }
  if (QFileInfo::exists(backup + "/package_manifest.json")) {
    result = track(j, "rollback_manifest");
    if (!result)
      return result;
    if (QFileInfo::exists(target + "/package_manifest.json") &&
        !remove_checked(target + "/package_manifest.json"))
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
    result = track(j, "rollback_publish_exe");
    if (!result)
      return result;
    result = move_checked(backup + "/" + entry, live);
    if (!result)
      return result;
  }
  j["phase"] = "rolled_back";
  j.remove("intent");
  return terminal_save(path, j);
}
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  test_trace("run");
  const auto args = app.arguments();
  if (args.size() != 3 || (args[1] != "--apply" && args[1] != "--recover" && args[1] != "--recover-after-exit")) {
    std::fprintf(stderr, "usage: SeriousShotUpdater --apply|--recover transaction.json\n");
    return 2;
  }
  const bool recovery = args[1] != "--apply";
  const bool startup_recovery = args[1] == "--recover-after-exit";
  const auto journal = QFileInfo(args[2]).canonicalFilePath();
  if (journal.isEmpty() ||
      QFileInfo(journal).absolutePath().compare(QCoreApplication::applicationDirPath(),Qt::CaseInsensitive)!=0)
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
  if (nonce.isEmpty() || transaction.isEmpty() || QFileInfo(target).canonicalFilePath().compare(target,Qt::CaseInsensitive)!=0 ||
      exe.compare(target + "/" + j["old_manifest"].toObject()["entrypoint"].toString(),Qt::CaseInsensitive)!=0 ||
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
  if(startup_recovery) {
    const auto loaded_handoff=read_json(runtime+"/recovery.json");
    if(!loaded_handoff)return 25;
    const auto handoff=loaded_handoff.value();
    const auto recovery_nonce=handoff["recovery_nonce"].toString();
    const auto caller=handoff["caller_pid"].toString().toLongLong();
    const auto caller_creation=handoff["caller_creation"].toString();
    const auto event_name=QString("Local\\Overdrive.SeriousShot.Update.Recover.Arm.")+transaction+"."+recovery_nonce;
    if(recovery_nonce.isEmpty() || caller_creation.isEmpty() ||
       handoff["transaction"]!=transaction || handoff["generation"]!=j["generation"] ||
       handoff["nonce"]!=nonce || handoff["event_name"]!=event_name ||
       handoff["caller_exe"].toString().compare(exe,Qt::CaseInsensitive)!=0 ||
       !process_alive(caller,caller_creation))return 26;
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,static_cast<DWORD>(caller));
    wchar_t image[32768];DWORD length=32768;
    const bool matches=process && QueryFullProcessImageNameW(process,0,image,&length) &&
        QDir::fromNativeSeparators(QString::fromWCharArray(image,static_cast<int>(length))).compare(exe,Qt::CaseInsensitive)==0;
    if(process)CloseHandle(process);
    if(!matches)return 27;
    HANDLE event=OpenEventW(SYNCHRONIZE,FALSE,reinterpret_cast<LPCWSTR>(event_name.utf16()));
    if(!event)return 28;
    struct CloseRecoveryEvent{HANDLE event;~CloseRecoveryEvent(){CloseHandle(event);}} close{event};
    if(!save_json(runtime+"/recovery-ready.json",{{"recovery_nonce",recovery_nonce},
       {"pid",QString::number(app.applicationPid())},{"creation",process_identity(app.applicationPid())}}))return 29;
    while(process_alive(caller,caller_creation))QThread::msleep(100);
    const auto armed=read_json(runtime+"/recovery-armed.json");
    if(WaitForSingleObject(event,0)!=WAIT_OBJECT_0 || !armed || armed.value()!=handoff)return 0;
  }
  // Terminal recovery is idempotent. Commit/rollback releases the slot's
  // transaction and nonce; do not require that obsolete reservation on a
  // repeated invocation, or restart a second application instance. The
  // signed catalog and frozen manifests were validated above.
  const bool was_terminal = j["phase"] == "complete" || j["phase"] == "rolled_back";
  Slot slot(j["slot"].toString(), j["owner"].toString(), j["channel"].toString());
  slot.lock = std::make_unique<QLockFile>(slot.root + "/operation.lock");
  slot.lock->setStaleLockTime(0);
  if (!slot.lock->tryLock(10000))
    return 15;
  auto slotstate = read_json(slot.root + "/slot.json");
  if (!slotstate)
    return 16;
  slot.record = slotstate.value();
  const bool owns_reservation = slot.record["generation"] == j["generation"] &&
      slot.record["transaction"] == transaction && slot.record["nonce"] == nonce &&
      slot.record["owner"] == j["owner"] &&
      (slot.record["phase"] == "reserved" || slot.record["phase"] == "installing");
  if (!owns_reservation) {
    if (recovery && was_terminal) {
      const auto &expected=j["phase"]=="complete"?fresh.value():old.value();
      return verify_tree(target,expected)?0:24;
    }
    return 17;
  }
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
  bool terminal_durable=false;
  if (was_terminal) {
    // A terminal journal can precede slot finalization. Verify its committed
    // tree and finish the still-owned reservation without rolling it back.
    result=verify_tree(target,j["phase"]=="complete"?fresh.value():old.value());
    if (result)
      result=confirm_json_durability(journal);
    terminal_durable=bool(result);
  } else if (recovery || j["phase"] != "prepared")
    { result = restore(journal, j, old.value(), fresh.value()); terminal_durable=bool(result); }
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
      j["intent"] = "withdraw_exe";
      j["file"] = old.value().entrypoint;
      // The frozen descriptor must be durable before the first mutation.
      result = checkpoint(journal, j);
      if (result)
        result = track(j, "withdraw_exe", old.value().entrypoint);
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
      result = track(j, "backup_file", f.path);
      if (result)
        result = move_checked(target + "/" + f.path, backup + "/" + f.path);
    }
    if (result) {
      result = track(j, "backup_manifest");
      if (result)
        result = move_checked(target + "/package_manifest.json", backup + "/package_manifest.json");
    }
    for (const auto &f : fresh.value().files) {
      if (!result || f.path == fresh.value().entrypoint)
        continue;
      result = track(j, "publish_file", f.path);
      if (result)
        result = move_checked(staging + "/" + f.path, target + "/" + f.path);
    }
    if (result) {
      result = track(j, "publish_manifest");
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
      result = track(j, "publish_exe");
      if (result)
        result = move_checked(staging + "/" + fresh.value().entrypoint, exe);
    }
    qint64 launched = 0;
    if (result) {
      j["phase"] = "awaiting_receipt";
      j["intent"] = "launch";
      j["file"] = QString{};
      result = checkpoint(journal, j);
      if (result)
        result = track(j, "launch");
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
      result = terminal_save(journal, j);
      terminal_durable=bool(result);
    } else {
      const auto failed_intent=j.value("intent").toString();
      const auto failed_file=j.value("file").toString();
      j["failure_intent"]=failed_intent;
      j["failure_file"]=failed_file;
      const auto reason = result.error().safe_context.find("reason");
      if (reason != result.error().safe_context.end())
        j["failure_reason"] = QString::fromStdString(reason->second);
      QJsonObject context;
      for (const auto &[key,value] : result.error().safe_context)
        context[QString::fromStdString(key)]=QString::fromStdString(value);
      j["failure_context"]=context;
      const auto restored = restore(journal, j, old.value(), fresh.value());
      terminal_durable=bool(restored);
      if (!restored) {
        // Preserve the original failure when cleanup also fails.
        auto original = result.error();
        for (const auto &[key, value] : restored.error().safe_context)
          original.safe_context["rollback_" + key] = value;
        result = Result<bool, Error>::failure(std::move(original));
      }
    }
  }
#if HDRSHOT_UPDATE_TESTING
  if (terminal_durable && qEnvironmentVariableIsSet("SERIOUSSHOT_UPDATE_CRASH_AFTER_TERMINAL"))
    ExitProcess(92);
#endif
  const bool complete = terminal_durable && j["phase"] == "complete",
             rolled = terminal_durable && j["phase"] == "rolled_back";
  if(complete || rolled) {
    const auto retained=slot.finish_install_result(j);
    if(!retained)return 20; // Keep reservation/runtime for retryable finalization.
  }
  if (!result && !terminal_durable) {
    const auto value=[&](const char *key) {
      const auto found=result.error().safe_context.find(key);
      return found==result.error().safe_context.end()?std::string{}:found->second;
    };
    std::fprintf(stderr,"native recovery remains pending: reason=%s qt=%s native=%s stage=%s attempt=%s committed=%s qt_text=%s move_attempt=%s rollback_reason=%s rollback_qt=%s rollback_native=%s rollback_stage=%s rollback_attempt=%s rollback_committed=%s rollback_qt_text=%s rollback_move_attempt=%s\n",
                 value("reason").c_str(),value("qt_file_error").c_str(),value("native_error").c_str(),
                 value("save_stage").c_str(),value("save_attempt").c_str(),
                 value("state_committed").c_str(),value("qt_error_string").c_str(),
                 value("move_attempt").c_str(),
                 value("rollback_reason").c_str(),value("rollback_qt_file_error").c_str(),
                 value("rollback_native_error").c_str(),value("rollback_save_stage").c_str(),
                 value("rollback_save_attempt").c_str(),value("rollback_state_committed").c_str(),
                 value("rollback_qt_error_string").c_str(),value("rollback_move_attempt").c_str());
  }
  if (complete) {
    slot.record["phase"] = "ready";
    auto removed = slot.remove_payload();
    slot.record["phase"] = removed ? "empty" : "cleanup_blocked";
    slot.record.remove("transaction");
    slot.record.remove("nonce");
    (void)slot.save();
  } else if (rolled) {
    slot.record["phase"] = "ready";
    slot.record.remove("transaction");
    slot.record.remove("nonce");
    (void)slot.save();
    if (!recovery || !was_terminal) {
      qint64 restarted;
      QProcess::startDetached(exe, {}, target, &restarted);
    }
  } else {
    slot.record["phase"] = "installing";
    (void)slot.save();
  }
  if(startup_recovery && complete) {
    qint64 restarted;
    QProcess::startDetached(exe,{},target,&restarted);
  } else if(startup_recovery && rolled && was_terminal) {
    qint64 restarted;
    QProcess::startDetached(exe,{},target,&restarted);
  }
  return complete || rolled ? 0 : 20;
}
