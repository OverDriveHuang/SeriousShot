#include "platform/qt/qt_session_diagnostics_port.hpp"
#include "test_support.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

namespace {
using namespace hdrshot;
constexpr const char* build = "2026-09-08 00:00:00";
QByteArray read(const std::string& path) {
  QFile file(QString::fromStdString(path));
  HDRSHOT_CHECK(file.open(QIODevice::ReadOnly));
  return file.readAll();
}
void write(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  HDRSHOT_CHECK(file.open(QIODevice::WriteOnly));
  HDRSHOT_CHECK(file.write(bytes) == bytes.size());
}
DiagnosticEvent completion(std::string format = "PNG", std::string range = "HDR") {
  DiagnosticEvent event{};
  event.subsystem = "export"; event.command = "copy"; event.stage = "complete";
  event.outcome = "success";
  event.safe_context = {{"format", std::move(format)}, {"range", std::move(range)}, {"elapsedMs", "1234"}};
  return event;
}
void starts_rotate_only_owned_logs() {
  QTemporaryDir dir;
  HDRSHOT_CHECK(dir.isValid());
  const auto legacy = dir.filePath("runtime.jsonl");
  const auto foreign = dir.filePath("notes.log");
  write(legacy, "old log"); write(foreign, "keep this");
  HDRSHOT_CHECK(QDir(dir.path()).mkdir("runtime-20200101-000000-000-000.log"));
  const auto link = dir.filePath("runtime-20200101-000000-001-000.log");
  std::error_code ec;
  std::filesystem::create_symlink(foreign.toStdString(), link.toStdString(), ec);
  const bool linked = !ec; // Windows may require a symlink privilege.
  std::string previous;
  for (int launch = 0; launch < 5; ++launch) {
    QtSessionDiagnosticsPort log(dir.path().toStdString());
    HDRSHOT_CHECK(log.start(build, launch % 2 == 0 ? "macOS" : "Windows").has_value());
    const auto path = log.exact_path();
    HDRSHOT_CHECK(path != previous);
    HDRSHOT_CHECK(read(path).contains("start SeriousShot build=2026-09-08_00:00:00 platform="));
    if (launch == 0) HDRSHOT_CHECK(QFileInfo::exists(legacy));
    else {
      HDRSHOT_CHECK(!QFileInfo::exists(legacy));
      HDRSHOT_CHECK(QFileInfo::exists(QString::fromStdString(previous)));
    }
    const auto logs = QDir(dir.path()).entryList({"runtime*.log", "runtime.jsonl"}, QDir::Files | QDir::NoSymLinks);
    HDRSHOT_CHECK(logs.size() == 2);
    HDRSHOT_CHECK(read(foreign.toStdString()) == "keep this");
    HDRSHOT_CHECK(QFileInfo(dir.filePath("runtime-20200101-000000-000-000.log")).isDir());
    if (linked) HDRSHOT_CHECK(QFileInfo(link).isSymLink());
    previous = path;
  }
  HDRSHOT_CHECK(QFileInfo::exists(QString::fromStdString(previous))); // no delete on exit
}
void failed_start_preserves_previous() {
  QTemporaryDir dir;
  const auto old = dir.filePath("runtime.jsonl");
  write(old, "keep");
  const auto blocked = dir.filePath("not-a-directory");
  write(blocked, "block");
  QtSessionDiagnosticsPort failed(blocked.toStdString());
  HDRSHOT_CHECK(!failed.start(build, "macOS"));
  HDRSHOT_CHECK(read(old.toStdString()) == "keep");
  QtSessionDiagnosticsPort valid(dir.path().toStdString());
  HDRSHOT_CHECK(valid.start(build, "Windows").has_value());
  const auto before = read(valid.exact_path());
  HDRSHOT_CHECK(!valid.start(build, "Windows"));
  HDRSHOT_CHECK(read(valid.exact_path()) == before);
  HDRSHOT_CHECK(read(old.toStdString()) == "keep");
}
void compact_results_and_errors_only() {
  QTemporaryDir dir;
  QtSessionDiagnosticsPort log(dir.path().toStdString());
  HDRSHOT_CHECK(log.start(build, "macOS").has_value());
  const auto header = read(log.exact_path());
  for (const auto* subsystem : {"export", "capture", "tray_menu", "settings_window"}) {
    for (const auto* outcome : {"started", "triggered", "success", "observed"}) {
      DiagnosticEvent noise{};
      noise.subsystem = subsystem; noise.stage = "encode_started"; noise.outcome = outcome;
      HDRSHOT_CHECK(log.record(noise).has_value());
    }
  }
  HDRSHOT_CHECK(read(log.exact_path()) == header);
  auto result = completion("JPEG", "SDR");
  result.safe_context["path"] = "/private/secret";
  result.safe_context["text"] = "secret-content";
  result.destination_name = "secret-filename.png";
  HDRSHOT_CHECK(log.record(result).has_value());
  result.outcome = "failure"; result.error_code = "storage_full";
  result.safe_context["range"] = "unknown";
  result.safe_context["reason"] = "publication_not_visible";
  HDRSHOT_CHECK(log.record(result).has_value());
  result.outcome = "cancelled";
  HDRSHOT_CHECK(log.record(result).has_value());
  DiagnosticEvent capture{};
  capture.subsystem = "capture"; capture.outcome = "failure";
  capture.error_code = "permission_denied\nINJECTED";
  capture.safe_context["reason"] = "/private/secret\nINJECTED";
  HDRSHOT_CHECK(log.record(capture).has_value());
  const auto bytes = read(log.exact_path());
  HDRSHOT_CHECK(bytes.count('\n') == 5);
  HDRSHOT_CHECK(bytes.contains("copy JPEG SDR 1234ms ok\n"));
  HDRSHOT_CHECK(bytes.contains("copy JPEG unknown 1234ms failure code=storage_full reason=publication_not_visible\n"));
  HDRSHOT_CHECK(bytes.contains("copy JPEG unknown 1234ms cancelled\n"));
  HDRSHOT_CHECK(!bytes.contains("secret"));
}
void two_hundred_results_under_twenty_kb() {
  QTemporaryDir dir;
  QtSessionDiagnosticsPort log(dir.path().toStdString());
  HDRSHOT_CHECK(log.start(build, "Windows").has_value());
  for (int i = 0; i < 200; ++i) {
    auto result = completion(i % 2 ? "JPEG" : "PNG", i % 3 ? "HDR" : "SDR");
    result.command = i % 3 == 0 ? "save_as" : i % 3 == 1 ? "save" : "copy";
    result.safe_context["elapsedMs"] = "12345";
    if (i % 40 == 0) { result.outcome = "failure"; result.error_code = "path_not_writable"; }
    HDRSHOT_CHECK(log.record(result).has_value());
  }
  const auto bytes = read(log.exact_path());
  HDRSHOT_CHECK(bytes.count('\n') == 201);
  HDRSHOT_CHECK(bytes.size() < 20000);
  std::cout << "200 operations + startup: " << bytes.size() << " bytes\n";
}
void concurrent_results_do_not_interleave() {
  QTemporaryDir dir;
  QtSessionDiagnosticsPort log(dir.path().toStdString());
  HDRSHOT_CHECK(log.start(build, "macOS").has_value());
  const auto worker = [&] {
    for (int i = 0; i < 50; ++i) HDRSHOT_CHECK(log.record(completion()).has_value());
  };
  std::thread a(worker), b(worker);
  a.join(); b.join();
  const auto bytes = read(log.exact_path());
  HDRSHOT_CHECK(bytes.count('\n') == 101);
  HDRSHOT_CHECK(bytes.count("copy PNG HDR 1234ms ok\n") == 100);
}
void detailed_mode_hot_switch_preserves_file_and_privacy() {
  QTemporaryDir dir;
  QtSessionDiagnosticsPort log(dir.path().toStdString());
  HDRSHOT_CHECK(log.start(build, "macOS").has_value());
  const auto path = log.exact_path();
  HDRSHOT_CHECK(!log.detailed_logging());
  DiagnosticEvent event{};
  event.session_id = SessionId{12}; event.operation_id = OperationId{3};
  event.subsystem = "capture"; event.stage = "native_return"; event.outcome = "success";
  event.safe_context = {{"width","4112"},{"currentEDR","4.5"},{"api","captureImageWithFilter"},
      {"path","/Users/private/company"}, {"text","secret content"}, {"ICC","private-profile"},
      {"nativeCode","1\nsecret"}, {"reason","user secret"}, {"windowTitle","secret window"}};
  HDRSHOT_CHECK(log.record(event).has_value());
  HDRSHOT_CHECK(!read(path).contains("native_return"));
  log.set_detailed_logging(true);
  HDRSHOT_CHECK(log.detailed_logging());
  HDRSHOT_CHECK(log.record(event).has_value());
  const Error error{ErrorCode::capture_failed, "fixture", Retryability::same_input,
      {{"reason","fixture_failure"},{"nativeCode","-3801"},{"path","/private/hidden"}}};
  auto copied = error;
  record_diagnostic_stage(&log, SessionId{12}, OperationId{3}, "capture",
      "failure_origin", "failure", {}, &copied);
  auto detailed = read(path);
  HDRSHOT_CHECK(detailed.contains("session=12 op=3 capture.native_return success"));
  HDRSHOT_CHECK(detailed.contains("width=4112"));
  HDRSHOT_CHECK(detailed.contains("currentEDR=4.5"));
  HDRSHOT_CHECK(detailed.contains("nativeCode=-3801"));
  HDRSHOT_CHECK(detailed.contains("at=tests/qt_session_diagnostics_port_tests.cpp:"));
  HDRSHOT_CHECK(detailed.contains("function="));
  for (auto* forbidden : {"secret","/Users","/private","company","private-profile"})
    HDRSHOT_CHECK(!detailed.contains(forbidden));
  log.set_detailed_logging(false);
  log.set_detailed_logging(false); // Idempotent: one transition only.
  HDRSHOT_CHECK(log.record(event).has_value());
  HDRSHOT_CHECK(log.exact_path() == path);
  auto bytes = read(path);
  HDRSHOT_CHECK(bytes.count("capture.native_return") == 1);
  HDRSHOT_CHECK(bytes.count("mode=detailed") == 1);
  HDRSHOT_CHECK(bytes.count("mode=simple") == 1);
}

void hot_switch_is_safe_while_worker_records() {
  QTemporaryDir dir;
  QtSessionDiagnosticsPort log(dir.path().toStdString());
  HDRSHOT_CHECK(log.start(build, "Windows").has_value());
  std::thread worker([&] { for (int i=0;i<100;++i) (void)log.record(completion()); });
  for (int i=0;i<20;++i) log.set_detailed_logging(i%2==0);
  worker.join();
  auto bytes=read(log.exact_path());
  HDRSHOT_CHECK(bytes.count('\n') == 121);
  HDRSHOT_CHECK(bytes.count("logging mode=") == 20);
}
}  // namespace
int main() {
  return hdrshot::test::run({
      {"detailed hot switch, source origin and privacy", detailed_mode_hot_switch_preserves_file_and_privacy},
      {"hot switch during background writes", hot_switch_is_safe_while_worker_records},
      {"session rotation and safe targets", starts_rotate_only_owned_logs},
      {"creation failure preserves old logs", failed_start_preserves_previous},
      {"compact results and errors only", compact_results_and_errors_only},
      {"200 operations log size", two_hundred_results_under_twenty_kb},
      {"concurrent records are complete lines", concurrent_results_do_not_interleave}});
}
