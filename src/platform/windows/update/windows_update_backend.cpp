#include "core/build_metadata.hpp"
#include "platform/qt/update/native_update_backend.hpp"
#include "platform/qt/update/update_catalog.hpp"
#include "platform/qt/update/application_paths.hpp"
#include "platform/windows/update/windows_update_native.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QThread>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <io.h>
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
namespace hdrshot::update {
QString windows_executable() {
  wchar_t path[32768];
  const auto n = GetModuleFileNameW(nullptr, path, 32768);
  return n ? QDir::fromNativeSeparators(QString::fromWCharArray(path, static_cast<int>(n)))
           : QString{};
}
QString process_identity(qint64 pid) {
  if (pid <= 0 || pid > MAXDWORD)
    return {};
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (!process)
    return {};
  FILETIME created{}, exited{}, kernel{}, user{};
  const bool ok = GetProcessTimes(process, &created, &exited, &kernel, &user) != 0;
  CloseHandle(process);
  return ok ? QString::number((quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime)
            : QString{};
}
bool process_alive(qint64 pid, const QString &creation) {
  if (pid <= 0 || pid > MAXDWORD)
    return false;
  // Creation-time queries can still succeed after exit while a launcher holds
  // the process object. Query its signaled state as well as its PID identity.
  HANDLE p = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                         static_cast<DWORD>(pid));
  if (!p)
    return GetLastError() == ERROR_ACCESS_DENIED;
  const auto wait = WaitForSingleObject(p, 0);
  FILETIME created{}, exited{}, kernel{}, user{};
  const bool known = GetProcessTimes(p, &created, &exited, &kernel, &user) != 0;
  const auto current = known ? QString::number((quint64(created.dwHighDateTime) << 32) |
                                                created.dwLowDateTime) : QString{};
  const bool active = wait == WAIT_TIMEOUT &&
                      (creation.isEmpty() || !known || current == creation);
  CloseHandle(p);
  return active;
}
bool durable_flush(QFile &f) {
  const auto h = reinterpret_cast<HANDLE>(_get_osfhandle(f.handle()));
  return f.flush() && FlushFileBuffers(h) != 0;
}
bool durable_directory(const QString &) {
  // Win32 has no portable directory fsync. Rename uses MOVEFILE_WRITE_THROUGH;
  // each replaced/state file is flushed. Actual power-loss behavior remains a
  // native gate.
  return true;
}
QString windows_extended_path(const QString &path) {
  auto native=QDir::toNativeSeparators(path);
  if(native.startsWith(QStringLiteral("\\\\?\\")))return native;
  native=QDir::toNativeSeparators(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
  if(native.startsWith(QStringLiteral("\\\\")))
    return QStringLiteral("\\\\?\\UNC\\")+native.mid(2);
  return QStringLiteral("\\\\?\\")+native;
}
#ifdef HDRSHOT_UPDATE_MOVE_TESTING
namespace move_test {
namespace { thread_local Hooks *hooks = nullptr; }
void set_hooks(Hooks *value) { hooks = value; }
} // namespace move_test
#endif
Result<bool, Error> windows_move(const QString &from, const QString &to) {
  const auto source=windows_extended_path(from),destination=windows_extended_path(to);
  for (unsigned attempt = 1; attempt <= 3; ++attempt) {
    if (attempt > 1) {
      const unsigned delay = attempt == 2 ? 200 : 800;
#ifdef HDRSHOT_UPDATE_MOVE_TESTING
      if (move_test::hooks && move_test::hooks->before_retry)
        move_test::hooks->before_retry(delay);
#endif
      QThread::msleep(delay);
    }
#ifdef HDRSHOT_UPDATE_MOVE_TESTING
    if (move_test::hooks && move_test::hooks->before_attempt)
      move_test::hooks->before_attempt(attempt);
#endif
    // Preserve non-overwriting moves for backup, publication and rollback.
    if (MoveFileExW(reinterpret_cast<LPCWSTR>(source.utf16()),
                    reinterpret_cast<LPCWSTR>(destination.utf16()), MOVEFILE_WRITE_THROUGH))
      return Result<bool, Error>::success(true);
    const auto native_error = GetLastError();
    const bool transient = native_error == ERROR_SHARING_VIOLATION ||
        native_error == ERROR_LOCK_VIOLATION || native_error == ERROR_ACCESS_DENIED ||
        native_error == ERROR_BUSY || native_error == ERROR_RETRY;
    if (transient && attempt < 3) continue;
    auto error = failure("replace_move", ErrorCode::path_not_writable,
                         Retryability::after_user_action);
    error.safe_context["native_error"] = std::to_string(native_error);
    error.safe_context["move_attempt"] = std::to_string(attempt);
    return Result<bool, Error>::failure(std::move(error));
  }
  Q_UNREACHABLE();
}
bool windows_install_quiet(const QString &target, const QString &backup) {
  DWORD ids[16384], bytes = 0;
  if (!EnumProcesses(ids, sizeof(ids), &bytes))
    return false;
  for (DWORD i = 0; i < bytes / sizeof(DWORD); ++i) {
    if (!ids[i] || ids[i] == GetCurrentProcessId())
      continue;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ids[i]);
    if (!p)
      continue;
    wchar_t path[32768];
    DWORD length = 32768;
    const bool found = QueryFullProcessImageNameW(p, 0, path, &length) != 0;
    CloseHandle(p);
    if (!found)
      continue;
    const auto image =
        QDir::fromNativeSeparators(QString::fromWCharArray(path, static_cast<int>(length)));
    if (image.compare(target, Qt::CaseInsensitive) == 0 ||
        (!backup.isEmpty() && image.compare(backup, Qt::CaseInsensitive) == 0))
      return false;
  }
  return true;
}
static bool safe_destination(const QString &root, const QString &path) {
  if (!safe_relative(path))
    return false;
  QString full = root;
  for (const auto &component : path.split('/')) {
    full += "/" + component;
    const DWORD attributes =
        GetFileAttributesW(reinterpret_cast<LPCWSTR>(windows_extended_path(full).utf16()));
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
      return false;
  }
  return true;
}
class WindowsInstaller final : public NativeInstaller {
public:
  QString install_identity() const override {
    return QFileInfo(windows_executable()).canonicalFilePath();
  }
  bool manual() const override {
    return !HDRSHOT_WINDOWS_UPDATE_NATIVE_VALIDATED;
  }
  Result<bool,Error> open_manual(Slot &slot) override {
    slot.record["phase"]="manual_pending";
    auto saved=slot.save();if(!saved)return saved;
    const auto opened=ShellExecuteW(nullptr,L"open",
      reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(slot.payload()).utf16()),nullptr,nullptr,SW_SHOWNORMAL);
    if(reinterpret_cast<INT_PTR>(opened)>32)return Result<bool,Error>::success(true);
    slot.record["phase"]="ready";(void)slot.save();
    auto error=failure("无法打开安装包，请检查系统的 ZIP 打开方式后重试。",ErrorCode::permission_denied,Retryability::after_user_action);
    error.safe_context["manual_install_required"]="true";
    return Result<bool,Error>::failure(std::move(error));
  }
  Result<bool, Error> recover(Slot &slot) override {
    const auto runtime = slot.record["runtime"].toString(),
               journal = slot.record["journal"].toString();
    if (runtime.isEmpty() || !runtime.startsWith(slot.root + "/helpers/") ||
        journal != runtime + "/transaction.json" || QFileInfo(runtime).isSymLink() ||
        process_alive(slot.record["parent_pid"].toString().toLongLong(),
                      slot.record["parent_creation"].toString()) ||
        process_alive(slot.record["helper_pid"].toString().toLongLong(),
                      slot.record["helper_creation"].toString()))
      return Result<bool, Error>::failure(failure("恢复事务仍被占用"));
    QProcess helper;
    // FreeType's CRT font stream is inheritable. The independent helper must
    // never retain application resources after the GUI has exited.
    helper.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
      args->inheritHandles=false;
    });
    helper.setProgram(runtime + "/SeriousShotUpdater.exe");
    helper.setArguments({"--recover", journal});
    helper.setWorkingDirectory(runtime);
    qint64 pid = 0;
    if (!helper.startDetached(&pid))
      return Result<bool, Error>::failure(failure("recovery_helper_start"));
    const auto creation = process_identity(pid);
    const auto journal_state = read_json(journal);
    if (journal_state)
      (void)register_cleanup(slot.root, journal_state.value(), pid);
    slot.record["helper_pid"] = QString::number(pid);
    slot.record["helper_creation"] = creation;
    auto saved = slot.save();
    slot.release();
    if (!saved)
      return saved;
    for (int wait = 0; wait < 1800 && process_alive(pid, creation); ++wait)
      QThread::msleep(100);
    auto recovered = read_json(journal);
    if (!recovered ||
        !(recovered.value()["phase"] == "complete" || recovered.value()["phase"] == "rolled_back"))
      return Result<bool, Error>::failure(failure("恢复需要人工处理"));
    return slot.acquire();
  }
  Result<bool, Error> cancel_manual(Slot &slot) override {
    slot.record["phase"] = "ready";
    return slot.save();
  }
  Result<bool, Error> install(Slot &slot, const Candidate &c, const QByteArray &envelope,
                              std::function<bool()> quit) override {
    if (manual()) {
      const auto opened =
          ShellExecuteW(nullptr, L"open",
                        reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(slot.payload()).utf16()),
                        nullptr, nullptr, SW_SHOWNORMAL);
      if (reinterpret_cast<INT_PTR>(opened) <= 32)
        return Result<bool, Error>::failure(
            failure("请从发布页面手动安装", ErrorCode::permission_denied));
      slot.record["phase"] = "manual_pending";
      return slot.save();
    }
    const auto exe = windows_executable();
    const auto target = QFileInfo(exe).absolutePath();
    if (QFileInfo(target).isSymLink())
      return Result<bool, Error>::failure(failure("target_reparse"));
    const auto manual_required=[&target](const char *reason) {
      auto error=failure(reason,ErrorCode::permission_denied,Retryability::after_user_action);
      error.safe_context["manual_install_required"]="true";
      error.safe_context["install_directory"]=target.toStdString();
      return Result<bool,Error>::failure(std::move(error));
    };
    if(!QFileInfo::exists(target+"/package_manifest.json"))
      return manual_required("当前目录缺少安装文件清单，自动更新未执行。当前版本仍可使用。");
    auto old_json = read_json(target + "/package_manifest.json");
    if (!old_json)
      return manual_required("安装文件清单无法读取，自动更新未执行。当前版本仍可使用。");
    auto old_version = Version::parse(old_json.value()["version"].toString().toStdString());
    if (!old_version)
      return manual_required("安装文件清单的版本信息无效，自动更新未执行。当前版本仍可使用。");
    auto old_candidate = c;
    old_candidate.version = old_version.value();
    auto old = package_manifest(target, old_candidate);
    if (!old || (target + "/" + old.value().entrypoint).compare(exe,Qt::CaseInsensitive)!=0)
      return manual_required("安装文件清单与当前程序不一致，自动更新未执行。当前版本仍可使用。");
    auto old_verified = verify_tree(target, old.value());
    if (!old_verified)
      return manual_required("当前目录的应用文件不完整或已被修改，自动更新未执行。当前版本仍可使用。");
    const auto transaction = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto staging = target + "/.seriousshot-stage-" + transaction;
    const auto backup = target + "/.seriousshot-backup-" + transaction;
    const auto runtime = slot.root + "/helpers/" + transaction;
    const auto journal = runtime + "/transaction.json";
    const auto event_name =
        QString("Local\\Overdrive.SeriousShot.Update.Arm.") + transaction + "." + nonce;
    QJsonObject j{{"protocol", 1},
                  {"transaction", transaction},
                  {"nonce", nonce},
                  {"target", target},
                  {"exe", exe},
                  {"staging", staging},
                  {"backup", backup},
                  {"event_name", event_name},
                  {"phase", "preparing"},
                  {"parent_pid", QString::number(QCoreApplication::applicationPid())},
                  {"parent_creation", process_identity(QCoreApplication::applicationPid())}};
    auto registered = register_cleanup(slot.root, j);
    if (!registered)
      return registered;
    qint64 helper_pid = 0;
    QString helper_creation;
    bool transferred = false;
    HANDLE arm_event = nullptr;
    struct ScopeExit {
      std::function<void()> finish;
      ~ScopeExit() {
        finish();
      }
    } guard{[&] {
      if (!transferred) {
        if (arm_event)
          ResetEvent(arm_event);
        if (helper_pid > 0) {
          (void)save_json(runtime + "/cancel.json", {{"nonce", nonce}});
          for (int wait = 0; wait < 50 && process_alive(helper_pid, helper_creation); ++wait)
            QThread::msleep(100);
          if (process_alive(helper_pid, helper_creation)) {
            HANDLE helper =
                OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(helper_pid));
            if (helper) {
              if (process_identity(helper_pid) == helper_creation) {
                TerminateProcess(helper, 90);
                WaitForSingleObject(helper, 5000);
              }
              CloseHandle(helper);
            }
          }
        }
        j["phase"] = "cancelled";
        if (QFileInfo::exists(runtime))
          (void)save_json(journal, j);
        auto pending = read_json(slot.root + "/cleanup/" + transaction + ".json");
        if (pending) {
          auto record = pending.value();
          record["terminal"] = true;
          record["preparing"] = false;
          record["helper_pid"] = QString::number(helper_pid);
          record["helper_creation"] = helper_creation;
          (void)save_json(slot.root + "/cleanup/" + transaction + ".json", record);
        }
        if (slot.record["transaction"] == transaction) {
          slot.record["phase"] = "ready";
          slot.record.remove("transaction");
          slot.record.remove("nonce");
          (void)slot.save();
        }
      }
      if (arm_event)
        CloseHandle(arm_event);
    }};
    auto prepared = extract_zip(slot.payload(), staging, c);
    if (!prepared)
      return Result<bool, Error>::failure(prepared.error());
    for (const auto &file : prepared.value().files) {
      if (!safe_destination(target, file.path))
        return Result<bool, Error>::failure(failure("target_reparse"));
      const bool known =
          std::any_of(old.value().files.begin(), old.value().files.end(), [&](const auto &f) {
            return f.path.compare(file.path, Qt::CaseInsensitive) == 0;
          });
      if (!known && QFileInfo::exists(target + "/" + file.path))
        return Result<bool, Error>::failure(failure("更新文件与用户文件重名"));
    }
    if (!QDir().mkpath(backup))
      return Result<bool, Error>::failure(
          failure("当前目录不可写，请手动安装", ErrorCode::path_not_writable));
    if (!QDir().mkpath(runtime))
      return Result<bool, Error>::failure(failure("helper_directory"));
    // A deployed helper runtime has the same pinned Qt dependency files as the
    // app.
    for (const auto &file : old.value().files) {
      if (file.path == old.value().entrypoint)
        continue;
      const auto to = runtime + "/" + file.path;
      if (!QDir().mkpath(QFileInfo(to).absolutePath()) ||
          !QFile::copy(target + "/" + file.path, to))
        return Result<bool, Error>::failure(failure("helper_copy"));
    }
    if (!QFileInfo::exists(runtime + "/SeriousShotUpdater.exe"))
      return Result<bool, Error>::failure(failure("helper_missing"));
    j["old_manifest"] = old.value().raw;
    j["new_manifest"] = prepared.value().raw;
    j["envelope"] = QString::fromLatin1(envelope.toBase64());
    j["candidate"] = QString::fromStdString(c.key());
    j["phase"] = "prepared";
    j["slot"] = slot.root;
    j["generation"] = slot.record["generation"];
    j["owner"] = slot.owner;
    j["channel"] = slot.channel;
    auto written = save_json(journal, j);
    if (!written)
      return written;
    written = register_cleanup(slot.root, j);
    if (!written)
      return written;
    slot.record["phase"] = "reserved";
    slot.record["transaction"] = transaction;
    slot.record["nonce"] = nonce;
    slot.record["journal"] = journal;
    slot.record["runtime"] = runtime;
    written = slot.save();
    if (!written)
      return written;
    arm_event = CreateEventW(nullptr, TRUE, FALSE, reinterpret_cast<LPCWSTR>(event_name.utf16()));
    if (!arm_event || GetLastError() == ERROR_ALREADY_EXISTS)
      return Result<bool, Error>::failure(failure("arm_event_create"));
    QProcess helper;
    helper.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
      args->inheritHandles=false;
    });
    helper.setProgram(runtime + "/SeriousShotUpdater.exe");
    helper.setArguments({"--apply", journal});
    helper.setWorkingDirectory(runtime);
    if (!helper.startDetached(&helper_pid)) {
      slot.record["phase"] = "ready";
      (void)slot.save();
      return Result<bool, Error>::failure(failure("helper_start"));
    }
    helper_creation = process_identity(helper_pid);
    written = register_cleanup(slot.root, j, helper_pid);
    if (!written)
      return written;
    slot.record["helper_pid"] = QString::number(helper_pid);
    slot.record["helper_creation"] = process_identity(helper_pid);
    written = slot.save();
    if (!written)
      return written;
    bool ready = false;
    for (int n = 0; n < 100; ++n) {
      const auto receipt = read_json(runtime + "/ready.json");
      if (receipt && receipt.value()["nonce"] == nonce &&
          receipt.value()["pid"].toString().toLongLong() == helper_pid) {
        ready = true;
        break;
      }
      QThread::msleep(100);
    }
    if (!ready)
      return Result<bool, Error>::failure(
          failure("helper_ready_timeout", ErrorCode::operation_cancelled));
    // A ready helper cannot apply merely because the parent later exits.
    written = save_json(runtime + "/armed.json", {{"nonce", nonce},
                                                  {"transaction", transaction},
                                                  {"generation", slot.record["generation"]}});
    if (!written)
      return written;
    if (!quit())
      return Result<bool, Error>::failure(failure("退出已取消", ErrorCode::operation_cancelled));
    if (!SetEvent(arm_event))
      return Result<bool, Error>::failure(failure("arm_event_signal"));
    transferred = true;
    // The helper survives via startDetached semantics: QProcess destructor must
    // not kill it. Release before parent shutdown. The reservation still blocks
    // every other installer.
    slot.release();
    return Result<bool, Error>::success(true);
  }
};
std::unique_ptr<NativeInstaller> native_installer() {
  return std::make_unique<WindowsInstaller>();
}
Result<bool, Error> windows_begin_startup_recovery(const QString &isolated_root, bool login_startup) {
  const auto trust=compiled_trust();
  const auto root=isolated_root.isEmpty()?testing_namespace(
      QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)+
      "/OverDrive/SeriousShot/updates",trust):QDir::fromNativeSeparators(isolated_root);
  const auto exe=windows_executable(), owner=QFileInfo(exe).canonicalFilePath();
  Slot slot(root,owner,trust.channel);
  const auto acquired=slot.acquire();
  if (acquired || !slot.lock || slot.record["owner"].toString().compare(owner,Qt::CaseInsensitive)!=0 ||
      slot.record["channel"]!=trust.channel ||
      !(slot.record["phase"]=="reserved" || slot.record["phase"]=="installing") ||
      process_alive(slot.record["parent_pid"].toString().toLongLong(),slot.record["parent_creation"].toString()) ||
      process_alive(slot.record["helper_pid"].toString().toLongLong(),slot.record["helper_creation"].toString()))
    return Result<bool,Error>::success(false);
  const auto runtime=slot.record["runtime"].toString(),journal=slot.record["journal"].toString();
  const auto loaded=read_json(journal);
  if (!loaded || !QFileInfo::exists(journal))
    return Result<bool,Error>::failure(failure("startup_recovery_journal_missing"));
  const auto j=loaded.value();
  const auto transaction=j["transaction"].toString();
  if (transaction.isEmpty() || transaction.contains('/') || transaction.contains('\\') ||
      runtime!=root+"/helpers/"+transaction || journal!=runtime+"/transaction.json" ||
      QFileInfo(runtime).isSymLink() || j["exe"].toString().compare(exe,Qt::CaseInsensitive)!=0 ||
      j["generation"]!=slot.record["generation"] || j["nonce"]!=slot.record["nonce"] ||
      j["owner"]!=slot.record["owner"] || j["slot"]!=root)
    return Result<bool,Error>::failure(failure("startup_recovery_identity"));
  const auto nonce=QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto event_name=QString("Local\\Overdrive.SeriousShot.Update.Recover.Arm.")+transaction+"."+nonce;
  HANDLE event=CreateEventW(nullptr,TRUE,FALSE,reinterpret_cast<LPCWSTR>(event_name.utf16()));
  if (!event || GetLastError()==ERROR_ALREADY_EXISTS) {
    if(event)CloseHandle(event);
    return Result<bool,Error>::failure(failure("startup_recovery_event"));
  }
  bool armed=false;
  struct CloseEvent { HANDLE event; bool &armed; ~CloseEvent(){if(!armed)ResetEvent(event);CloseHandle(event);} } close{event,armed};
  const QJsonObject handoff{{"transaction",transaction},{"generation",j["generation"]},
      {"nonce",j["nonce"]},{"recovery_nonce",nonce},{"event_name",event_name},
      {"caller_pid",QString::number(QCoreApplication::applicationPid())},
      {"caller_creation",process_identity(QCoreApplication::applicationPid())},{"caller_exe",exe},
      {"login_startup",login_startup}};
  auto saved=save_json(runtime+"/recovery.json",handoff);
  if(!saved)return saved;
  QProcess helper;helper.setProgram(runtime+"/SeriousShotUpdater.exe");
  helper.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
    args->inheritHandles=false;
  });
  helper.setArguments({"--recover-after-exit",journal});helper.setWorkingDirectory(runtime);
  qint64 pid=0;
  if(!helper.startDetached(&pid))return Result<bool,Error>::failure(failure("startup_recovery_helper"));
  slot.record["helper_pid"]=QString::number(pid);slot.record["helper_creation"]=process_identity(pid);
  saved=register_cleanup(root,j,pid);
  if(saved)saved=slot.save();
  if(!saved)return saved;
  bool ready=false;
  for(int n=0;n<100;++n){
    const auto receipt=read_json(runtime+"/recovery-ready.json");
    if(receipt && receipt.value()["recovery_nonce"]==nonce &&
       receipt.value()["pid"].toString().toLongLong()==pid){
      if(login_startup && (!receipt.value()["login_startup"].isBool() || !receipt.value()["login_startup"].toBool())) {
        auto error=failure("startup_recovery_login_origin_incompatible");
        error.safe_context["description"]="旧更新恢复程序无法保留登录启动来源。本次未执行恢复；请手动启动 SeriousShot 完成恢复。";
        return Result<bool,Error>::failure(std::move(error));
      }
      ready=true;break;
    }
    QThread::msleep(100);
  }
  if(!ready)return Result<bool,Error>::failure(failure("startup_recovery_ready_timeout"));
#if HDRSHOT_UPDATE_TESTING
  if(qEnvironmentVariableIsSet("SERIOUSSHOT_UPDATE_STARTUP_REFUSE"))
    return Result<bool,Error>::failure(failure("startup_recovery_refused",ErrorCode::operation_cancelled));
#endif
  saved=save_json(runtime+"/recovery-armed.json",handoff);
  if(!saved)return saved;
  if(!SetEvent(event))return Result<bool,Error>::failure(failure("startup_recovery_arm"));
  armed=true;slot.release();
  return Result<bool,Error>::success(true);
}
void windows_write_start_receipt() {
  const auto args = QCoreApplication::arguments();
  const auto pos = args.indexOf("--update-receipt");
  if (pos >= 0 && pos + 2 < args.size())
    (void)save_json(
        args[pos + 1],
        {{"nonce", args[pos + 2]},
         {"exe", windows_executable()},
         {"version", QString::fromUtf8(product_version().data(),
                                       static_cast<qsizetype>(product_version().size()))},
         {"pid", QString::number(QCoreApplication::applicationPid())},
         {"creation", process_identity(QCoreApplication::applicationPid())}});
}
} // namespace hdrshot::update
