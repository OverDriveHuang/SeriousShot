#include "platform/windows/update/windows_update_native.hpp"
#include "test_support.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
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
  void deny_delete(const QString &path) {
    const auto native = windows_extended_path(path);
    value = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    HDRSHOT_CHECK(value != INVALID_HANDLE_VALUE);
  }
};
QByteArray read(const QString &path) {
  QFile file(path);
  HDRSHOT_CHECK(file.open(QIODevice::ReadOnly));
  return file.readAll();
}
void write(const QString &path, const QByteArray &bytes) {
  HDRSHOT_CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));
  QFile file(path);
  HDRSHOT_CHECK(file.open(QIODevice::WriteOnly));
  HDRSHOT_CHECK(file.write(bytes) == bytes.size());
  HDRSHOT_CHECK(file.flush());
}
struct Fixture {
  QTemporaryDir directory;
  QString source = directory.path() + "/source.dll";
  QString destination = directory.path() + "/destination.dll";
  QByteArray bytes = "owned update file bytes";
  move_test::Hooks hooks;
  std::vector<unsigned> attempts;
  std::vector<unsigned> waits;
  Fixture() {
    HDRSHOT_CHECK(directory.isValid());
    write(source, bytes);
    hooks.before_attempt = [this](unsigned attempt) { attempts.push_back(attempt); };
    hooks.before_retry = [this](unsigned delay) { waits.push_back(delay); };
    move_test::set_hooks(&hooks);
  }
  ~Fixture() { move_test::set_hooks(nullptr); }
  void reset_counts() { attempts.clear(); waits.clear(); }
  void expect_moved() const {
    HDRSHOT_CHECK(!QFileInfo::exists(source));
    HDRSHOT_CHECK(read(destination) == bytes);
  }
};
DWORD native_move_error(const QString &source, const QString &destination) {
  const auto from = windows_extended_path(source), to = windows_extended_path(destination);
  const bool moved = MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()),
                                reinterpret_cast<LPCWSTR>(to.utf16()), MOVEFILE_WRITE_THROUGH);
  const auto error = moved ? ERROR_SUCCESS : GetLastError();
  HDRSHOT_CHECK(!moved);
  return error;
}
} // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  return hdrshot::test::run({
    {"first successful move finishes immediately", [] {
      Fixture fixture;
      HDRSHOT_CHECK(windows_move(fixture.source, fixture.destination));
      HDRSHOT_CHECK(fixture.attempts == std::vector<unsigned>{1});
      HDRSHOT_CHECK(fixture.waits.empty());
      fixture.expect_moved();
    }},
    {"real deny-delete handle releases before second attempt", [] {
      Fixture fixture;
      Handle lock;
      lock.deny_delete(fixture.source);
      fixture.hooks.before_retry = [&](unsigned delay) {
        fixture.waits.push_back(delay);
        HDRSHOT_CHECK(read(fixture.source) == fixture.bytes);
        HDRSHOT_CHECK(!QFileInfo::exists(fixture.destination));
        lock.close();
      };
      QElapsedTimer elapsed;
      elapsed.start();
      HDRSHOT_CHECK(windows_move(fixture.source, fixture.destination));
      HDRSHOT_CHECK(elapsed.elapsed() >= 200);
      HDRSHOT_CHECK(fixture.attempts == std::vector<unsigned>{1, 2});
      HDRSHOT_CHECK(fixture.waits == std::vector<unsigned>{200});
      fixture.expect_moved();
    }},
    {"persistent handle exhausts three and next move starts fresh", [] {
      Fixture fixture;
      Handle lock;
      lock.deny_delete(fixture.source);
      const auto expected_error = native_move_error(fixture.source, fixture.destination);
      const auto result = windows_move(fixture.source, fixture.destination);
      HDRSHOT_CHECK(!result);
      HDRSHOT_CHECK(fixture.attempts == std::vector<unsigned>{1, 2, 3});
      HDRSHOT_CHECK(fixture.waits == std::vector<unsigned>{200, 800});
      HDRSHOT_CHECK(result.error().safe_context.at("native_error") == std::to_string(expected_error));
      HDRSHOT_CHECK(result.error().safe_context.at("move_attempt") == "3");
      HDRSHOT_CHECK(read(fixture.source) == fixture.bytes);
      HDRSHOT_CHECK(!QFileInfo::exists(fixture.destination));
      lock.close();
      fixture.reset_counts();
      HDRSHOT_CHECK(windows_move(fixture.source, fixture.destination));
      HDRSHOT_CHECK(fixture.attempts == std::vector<unsigned>{1} && fixture.waits.empty());
      fixture.expect_moved();
    }},
    {"third attempt succeeds when persistent handle is released", [] {
      Fixture fixture;
      Handle lock;
      lock.deny_delete(fixture.source);
      fixture.hooks.before_retry = [&](unsigned delay) {
        fixture.waits.push_back(delay);
        if (delay == 800) lock.close();
      };
      HDRSHOT_CHECK(windows_move(fixture.source, fixture.destination));
      HDRSHOT_CHECK(fixture.attempts == std::vector<unsigned>{1, 2, 3});
      HDRSHOT_CHECK(fixture.waits == std::vector<unsigned>{200, 800});
      fixture.expect_moved();
    }},
    {"missing source preserves actual native error without retry", [] {
      Fixture fixture;
      const auto missing = fixture.directory.path() + "/missing.dll";
      const auto expected_error = native_move_error(missing, fixture.destination);
      const auto result = windows_move(missing, fixture.destination);
      HDRSHOT_CHECK(!result);
      HDRSHOT_CHECK(result.error().safe_context.at("native_error") == std::to_string(expected_error));
      HDRSHOT_CHECK(result.error().safe_context.at("move_attempt") == "1");
      HDRSHOT_CHECK(fixture.attempts == std::vector<unsigned>{1} && fixture.waits.empty());
      HDRSHOT_CHECK(read(fixture.source) == fixture.bytes);
      HDRSHOT_CHECK(!QFileInfo::exists(fixture.destination));
    }},
    {"existing destination is never overwritten or retried", [] {
      Fixture fixture;
      const QByteArray occupied = "existing destination must survive";
      write(fixture.destination, occupied);
      const auto expected_error = native_move_error(fixture.source, fixture.destination);
      std::cout << "collision native_error=" << expected_error << '\n';
      const auto result = windows_move(fixture.source, fixture.destination);
      HDRSHOT_CHECK(!result);
      HDRSHOT_CHECK(result.error().safe_context.at("native_error") == std::to_string(expected_error));
      HDRSHOT_CHECK(result.error().safe_context.at("move_attempt") == "1");
      HDRSHOT_CHECK(fixture.attempts == std::vector<unsigned>{1} && fixture.waits.empty());
      HDRSHOT_CHECK(read(fixture.source) == fixture.bytes);
      HDRSHOT_CHECK(read(fixture.destination) == occupied);
    }},
    {"long and already extended paths preserve native move contract", [] {
      Fixture fixture;
      QString deep = fixture.directory.path();
      for (unsigned i = 0; i < 10; ++i) deep += "/managed_update_path_component";
      fixture.source = deep + "/source.dll";
      fixture.destination = deep + "/backup.dll";
      HDRSHOT_CHECK(fixture.source.size() > 260 && fixture.destination.size() > 260);
      write(fixture.source, fixture.bytes);
      const auto source = windows_extended_path(fixture.source);
      HDRSHOT_CHECK(windows_extended_path(source) == source);
      HDRSHOT_CHECK(windows_move(source, windows_extended_path(fixture.destination)));
      HDRSHOT_CHECK(fixture.attempts == std::vector<unsigned>{1} && fixture.waits.empty());
      fixture.expect_moved();
    }},
  });
}
