#include "platform/qt/qt_capture_runtime.hpp"
#include "ui/qt/capture_error_message.hpp"
#include "test_support.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <thread>
using namespace hdrshot;
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  return test::run({{"queued owner and shutdown", [] {
    QtCaptureRuntime owner; auto runtime = owner.handle(); int called{};
    auto gui = std::this_thread::get_id();
    std::thread worker([&] { HDRSHOT_CHECK(runtime->post([&] { HDRSHOT_CHECK(std::this_thread::get_id() == gui); ++called; })); }); worker.join();
    HDRSHOT_CHECK(called == 0); QCoreApplication::processEvents(); HDRSHOT_CHECK(called == 1);
    runtime->post([&] { ++called; }); owner.shutdown(); QCoreApplication::processEvents();
    HDRSHOT_CHECK(called == 1); HDRSHOT_CHECK(!runtime->post([&] { ++called; }));
    std::thread release([runtime = std::move(runtime)] {}); release.join();
  }}, {"timer disarm and rearm", [] {
    QtCaptureRuntime owner; auto runtime = owner.handle(); QEventLoop loop; int calls{};
    runtime->arm(1, runtime->now(), [&] { ++calls; }); runtime->disarm(1);
    runtime->arm(2, runtime->now(), [&] { ++calls; loop.quit(); });
    QTimer::singleShot(500, &loop, &QEventLoop::quit); loop.exec(); HDRSHOT_CHECK(calls == 1);
  }}, {"recovery wording", [] {
    Error timeout{ErrorCode::capture_failed,"test",Retryability::after_user_action,{{"reason","capture_progress_timeout"}}};
    const auto message = capture_error_message(timeout, "permission", "Ctrl+F9");
    HDRSHOT_CHECK(message.contains("Ctrl+F9")); HDRSHOT_CHECK(message.contains(QStringLiteral("全部")));
    HDRSHOT_CHECK(!capture_busy_message(UiPhase::capturing, false).contains(QStringLiteral("已经打开")));
    HDRSHOT_CHECK(capture_busy_message(UiPhase::idle, true).contains(QStringLiteral("稍后")));
    timeout.code = ErrorCode::permission_denied; HDRSHOT_CHECK(capture_error_message(timeout,"permission","X") == "permission");
  }}});
}
