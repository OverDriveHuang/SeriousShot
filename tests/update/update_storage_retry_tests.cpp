#include "platform/qt/update/update_storage.hpp"
#include "test_support.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cstdio>
#include <windows.h>

using namespace hdrshot::update;
#undef HDRSHOT_CHECK
#define HDRSHOT_CHECK(...) ::hdrshot::test::check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__)

namespace {
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Handle() { close(); }
  void close() {
    if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
    value = INVALID_HANDLE_VALUE;
  }
  void hold(const QString &path, DWORD sharing) {
    close();
    value = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
                        sharing, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HDRSHOT_CHECK(value != INVALID_HANDLE_VALUE);
  }
};
struct Fixture {
  QTemporaryDir directory;
  QString path = directory.path() + "/transaction.json";
  QJsonObject old{{"phase", "prepared"}, {"sequence", 1}};
  QJsonObject next{{"phase", "replacing"}, {"sequence", 2}};
  storage_test::Hooks hooks;
  std::vector<std::pair<unsigned, bool>> attempts;
  std::vector<unsigned> waits;
  unsigned commits = 0;
  Fixture() {
    HDRSHOT_CHECK(directory.isValid());
    const auto initial = save_json(path, old);
    if (!initial) {
      std::fprintf(stderr, "fixture initial save failed\n");
      for (const auto &[field, value] : initial.error().safe_context)
        std::fprintf(stderr, "  %s=%s\n", field.c_str(), value.c_str());
    }
    HDRSHOT_CHECK(initial);
    hooks.before_attempt = [this](unsigned n, bool committed) {
      attempts.emplace_back(n, committed);
    };
    hooks.before_retry = [this](unsigned ms) { waits.push_back(ms); };
    hooks.after_commit = [this] { ++commits; };
    storage_test::set_hooks(&hooks);
  }
  ~Fixture() { storage_test::set_hooks(nullptr); }
  void reset_counts() { attempts.clear(); waits.clear(); commits = 0; }
  void expect(const QJsonObject &expected) const {
    const auto value = read_json(path);
    HDRSHOT_CHECK(value && value.value() == expected);
  }
};
struct FileIdentity {
  DWORD volume;
  DWORD high;
  DWORD low;
  bool operator==(const FileIdentity &) const = default;
};
FileIdentity file_identity(const QString &path) {
  Handle file;
  file.hold(path, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
  BY_HANDLE_FILE_INFORMATION information{};
  HDRSHOT_CHECK(GetFileInformationByHandle(file.value, &information));
  return {information.dwVolumeSerialNumber, information.nFileIndexHigh,
          information.nFileIndexLow};
}
} // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  return hdrshot::test::run({
    {"first success finishes without waiting", [] {
      Fixture fixture;
      HDRSHOT_CHECK(save_json(fixture.path, fixture.next));
      HDRSHOT_CHECK(fixture.attempts == std::vector<std::pair<unsigned, bool>>{{1, false}});
      HDRSHOT_CHECK(fixture.waits.empty() && fixture.commits == 1);
      fixture.expect(fixture.next);
    }},
    {"real deny-delete target is retried after 200ms", [] {
      Fixture fixture;
      Handle lock;
      lock.hold(fixture.path, FILE_SHARE_READ | FILE_SHARE_WRITE);
      fixture.hooks.before_retry = [&](unsigned ms) {
        fixture.waits.push_back(ms);
        fixture.expect(fixture.old); // A failed rename preserved valid old JSON.
        lock.close();
      };
      QElapsedTimer elapsed;
      elapsed.start();
      HDRSHOT_CHECK(save_json(fixture.path, fixture.next));
      HDRSHOT_CHECK(elapsed.elapsed() >= 200);
      HDRSHOT_CHECK(fixture.attempts.size() == 2 && fixture.waits == std::vector<unsigned>{200});
      HDRSHOT_CHECK(fixture.commits == 1);
      fixture.expect(fixture.next);
    }},
    {"held target stops at three and next save gets fresh budget", [] {
      Fixture fixture;
      Handle lock;
      lock.hold(fixture.path, FILE_SHARE_READ | FILE_SHARE_WRITE);
      const auto result = save_json(fixture.path, fixture.next);
      HDRSHOT_CHECK(!result);
      HDRSHOT_CHECK(fixture.attempts == std::vector<std::pair<unsigned, bool>>{
          {1, false}, {2, false}, {3, false}});
      HDRSHOT_CHECK(fixture.waits == std::vector<unsigned>{200, 800});
      HDRSHOT_CHECK(fixture.commits == 0);
      HDRSHOT_CHECK(result.error().safe_context.at("save_attempt") == "3");
      HDRSHOT_CHECK(result.error().safe_context.at("state_committed") == "false");
      HDRSHOT_CHECK(result.error().safe_context.contains("qt_error_string"));
      HDRSHOT_CHECK(!result.error().safe_context.contains("native_error"));
      fixture.expect(fixture.old);
      lock.close();
      fixture.reset_counts();
      HDRSHOT_CHECK(save_json(fixture.path, fixture.next));
      HDRSHOT_CHECK(fixture.attempts == std::vector<std::pair<unsigned, bool>>{{1, false}});
      HDRSHOT_CHECK(fixture.waits.empty());
      fixture.expect(fixture.next);
    }},
    {"rename and durability failures share three total attempts", [] {
      Fixture fixture;
      Handle lock;
      lock.hold(fixture.path, FILE_SHARE_READ | FILE_SHARE_WRITE);
      fixture.hooks.before_retry = [&](unsigned ms) {
        fixture.waits.push_back(ms);
        lock.close();
      };
      unsigned flushes = 0;
      fixture.hooks.flush_error = [&]() -> std::optional<unsigned long> {
        if (++flushes == 1) return ERROR_SHARING_VIOLATION;
        return {}; // Later attempts use the real FlushFileBuffers.
      };
      HDRSHOT_CHECK(save_json(fixture.path, fixture.next));
      HDRSHOT_CHECK(fixture.attempts == std::vector<std::pair<unsigned, bool>>{
          {1, false}, {2, false}, {3, true}});
      HDRSHOT_CHECK(fixture.waits == std::vector<unsigned>{200, 800});
      HDRSHOT_CHECK(fixture.commits == 1 && flushes == 2);
      fixture.expect(fixture.next);
    }},
    {"postcommit reopen race retries durability without rewriting", [] {
      Fixture fixture;
      Handle lock;
      fixture.hooks.after_commit = [&] {
        ++fixture.commits;
        lock.hold(fixture.path, FILE_SHARE_READ);
      };
      fixture.hooks.before_retry = [&](unsigned ms) {
        fixture.waits.push_back(ms);
        fixture.expect(fixture.next);
        lock.close();
      };
      HDRSHOT_CHECK(save_json(fixture.path, fixture.next));
      HDRSHOT_CHECK(fixture.attempts == std::vector<std::pair<unsigned, bool>>{
          {1, false}, {2, true}});
      HDRSHOT_CHECK(fixture.commits == 1);
      fixture.expect(fixture.next);
    }},
    {"pending durability is distinguishable and can be confirmed without rewriting", [] {
      Fixture fixture;
      fixture.hooks.flush_error = []() -> std::optional<unsigned long> {
        return ERROR_LOCK_VIOLATION;
      };
      const auto result = save_json(fixture.path, fixture.next);
      HDRSHOT_CHECK(!result && fixture.attempts.size() == 3 && fixture.commits == 1);
      HDRSHOT_CHECK(result.error().safe_context.at("state_committed") == "true");
      HDRSHOT_CHECK(result.error().safe_context.at("save_stage") == "FlushFileBuffers");
      HDRSHOT_CHECK(result.error().safe_context.at("native_error") == std::to_string(ERROR_LOCK_VIOLATION));
      fixture.expect(fixture.next);
      const auto identity = file_identity(fixture.path);
      fixture.reset_counts();
      fixture.hooks.flush_error = {};
      HDRSHOT_CHECK(confirm_json_durability(fixture.path));
      HDRSHOT_CHECK(fixture.attempts == std::vector<std::pair<unsigned, bool>>{{1, true}});
      HDRSHOT_CHECK(fixture.commits == 0 && fixture.waits.empty());
      HDRSHOT_CHECK(file_identity(fixture.path) == identity);
    }},
    {"durability confirmation uses at most three attempts", [] {
      Fixture fixture;
      const auto identity = file_identity(fixture.path);
      fixture.hooks.flush_error = []() -> std::optional<unsigned long> {
        return ERROR_SHARING_VIOLATION;
      };
      const auto result = confirm_json_durability(fixture.path);
      HDRSHOT_CHECK(!result);
      HDRSHOT_CHECK(fixture.attempts == std::vector<std::pair<unsigned, bool>>{
          {1, true}, {2, true}, {3, true}});
      HDRSHOT_CHECK(fixture.waits == std::vector<unsigned>{200, 800});
      HDRSHOT_CHECK(fixture.commits == 0 && file_identity(fixture.path) == identity);
      fixture.expect(fixture.old);
    }},
    {"disk-full corruption and invalid handle are terminal", [] {
      for (const auto code : {ERROR_DISK_FULL, ERROR_CRC, ERROR_INVALID_HANDLE}) {
        Fixture fixture;
        fixture.hooks.flush_error = [code]() -> std::optional<unsigned long> { return code; };
        const auto result = save_json(fixture.path, fixture.next);
        HDRSHOT_CHECK(!result);
        HDRSHOT_CHECK(fixture.attempts.size() == 1 && fixture.waits.empty());
        HDRSHOT_CHECK(result.error().safe_context.at("native_error") == std::to_string(code));
        HDRSHOT_CHECK(result.error().safe_context.at("state_committed") == "true");
        fixture.expect(fixture.next);
      }
    }},
    {"rollback receipt preserves storage diagnostics through disk readback", [] {
      Fixture fixture;
      Candidate candidate;
      candidate.version = {0, 1, 9};
      candidate.channel = "stable";
      candidate.platform = "windows";
      candidate.architecture = "x64";
      const QString owner = "storage-test-owner", channel = "stable";
      Slot slot(fixture.directory.path() + "/slot", owner, channel);
      const QJsonObject context{{"qt_file_error", "10"},
          {"qt_error_string", "Controlled Qt rename diagnostic"},
          {"save_stage", "commit"}, {"save_attempt", "3"},
          {"state_committed", "false"}, {"move_attempt", "3"}};
      const QJsonObject journal{{"owner", owner}, {"channel", channel},
          {"candidate", QString::fromStdString(candidate.key())},
          {"phase", "rolled_back"}, {"old_manifest", QJsonObject{{"version", "0.1.6"}}},
          {"failure_reason", "state_commit"}, {"failure_file", "Qt6Core.dll"},
          {"failure_context", context}};
      HDRSHOT_CHECK(slot.finish_install_result(journal));
      const auto loaded = read_json(install_result_path(slot.root, owner, channel));
      HDRSHOT_CHECK(loaded);
      const auto error = install_result_error(loaded.value(), owner, channel, candidate);
      HDRSHOT_CHECK(error);
      for (auto field = context.begin(); field != context.end(); ++field) {
        HDRSHOT_CHECK(loaded.value()[field.key()] == field.value());
        HDRSHOT_CHECK(error->safe_context.at(field.key().toStdString()) ==
                      field.value().toString().toStdString());
      }
      HDRSHOT_CHECK(!error->safe_context.contains("native_error"));
    }},
  });
}
