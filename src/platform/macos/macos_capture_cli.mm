#include "platform/macos/macos_capture_cli.hpp"

#include "adapters/shared/libultrahdr_encoder.hpp"
#include "platform/macos/macos_capture_ports.hpp"
#include "platform/macos/macos_export_ports.hpp"
#include "platform/macos/macos_metal_export_pixel_processor.hpp"
#include "platform/qt/qt_settings_store_port.hpp"
#include "platform/qt/qt_session_diagnostics_port.hpp"
#include "application/export_diagnostics.hpp"
#include "core/build_metadata.hpp"
#include <chrono>

#import <CoreGraphics/CoreGraphics.h>

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QStandardPaths>
#include <QTimer>

#include <iostream>
#include <mutex>
#include <optional>
#include <utility>

namespace hdrshot {
namespace {
int failed(const Error& error) {
  std::cerr << to_string(error.code) << " module=" << error.module;
  for (const auto& [key, value] : error.safe_context) std::cerr << ' ' << key << '=' << value;
  std::cerr << '\n';
  return 1;
}

// Native callbacks can arrive on any thread. Only the Qt thread touches loop
// and result. Late callbacks retain a small mailbox, never stack references.
template <typename T, typename Start, typename Cancel>
Result<T, Error> wait_for_capture(QApplication& app, Start start, Cancel cancel) {
  struct Mailbox {
    std::mutex dispatch_mutex;
    bool accepting{true};
    QEventLoop* loop{};
    std::optional<Result<T, Error>> result;
  };
  QEventLoop loop;
  auto mailbox = std::make_shared<Mailbox>();
  mailbox->loop = &loop;
  QTimer deadline;
  deadline.setSingleShot(true);
  QObject::connect(&deadline, &QTimer::timeout, &loop, [&loop] { loop.quit(); });
  deadline.start(15000);
  start([&app, mailbox](Result<T, Error> result) mutable {
    // Closing the mailbox synchronizes with native dispatch: after timeout no
    // callback can dereference QApplication while main() is destroying it.
    const std::scoped_lock lock(mailbox->dispatch_mutex);
    if (!mailbox->accepting) return;
    QMetaObject::invokeMethod(&app, [mailbox, result = std::move(result)]() mutable {
      if (!mailbox->loop || mailbox->result) return;
      mailbox->result = std::move(result);
      mailbox->loop->quit();
    }, Qt::QueuedConnection);
  });
  loop.exec();
  {
    const std::scoped_lock lock(mailbox->dispatch_mutex);
    mailbox->accepting = false;
    mailbox->loop = nullptr;
  }
  if (!mailbox->result) {
    cancel();
    return Result<T, Error>::failure({ErrorCode::capture_failed, "MacCaptureCli",
        Retryability::after_user_action, {{"reason", "capture_timeout_15s"}}});
  }
  return std::move(*mailbox->result);
}
}  // namespace

int run_macos_capture_cli(QApplication& app, const CaptureCommand& command) {
  QtSettingsStorePort settings_store(
      QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
          .filePath(QStringLiteral("settings.ini")).toStdString(),
      "Command+Shift+2",
      QDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation))
          .filePath(QStringLiteral("SeriousShot")).toStdString());
  auto settings = settings_store.load();
  if (!settings) return failed(settings.error());
  auto log = std::make_shared<QtSessionDiagnosticsPort>(
      QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
          .filePath("logs").toStdString());
  (void)log->start(std::string(build_timestamp()), "macOS");
  log->set_detailed_logging(settings.value().detailed_logging);
  record_diagnostic_stage(log.get(), SessionId{1}, OperationId{1},
      "trigger", "received", "accepted", {{"source", "cli"}});
  const auto fail = [&log](const Error& error) {
    record_diagnostic_failure(log.get(), "cli", error);
    return failed(error);
  };
  // Noninteractive invocation must not wait for an absent/locked user to answer
  // a TCC dialog. The normal GUI remains the permission onboarding entry.
  if (!CGPreflightScreenCaptureAccess()) {
    return fail({ErrorCode::permission_denied, "MacCaptureCli", Retryability::after_user_action,
        {{"reason", "screen_recording_access_required"},
         {"action", "Authorize SeriousShot in System Settings, then retry"}}});
  }
  const DisplayId primary{CGMainDisplayID()};
  auto catalog = std::make_shared<MacDisplayCatalogPort>(log);
  if (command.mode == CaptureCommandMode::list_displays) {
    auto result = wait_for_capture<DisplaySnapshotSet>(app, [&](auto completion) {
      catalog->snapshot_displays({SessionId{1}, OperationId{1}}, std::move(completion));
    }, [] {});
    if (!result) return fail(result.error());
    if (result.value().displays.empty())
      return fail({ErrorCode::capture_failed, "MacCaptureCli", Retryability::after_user_action,
                     {{"reason", "no_capturable_display_session_may_be_locked"}}});
    std::cout << "id\tprimary\tx\ty\twidth\theight\tscale\tpixel_width\tpixel_height\n";
    for (const auto& d : result.value().displays) {
      const auto b = d.desktop_frame_points;
      std::cout << d.id.value << '\t' << (d.id == primary ? "yes" : "no") << '\t'
                << b.x << '\t' << b.y << '\t' << b.width << '\t' << b.height << '\t'
                << d.point_pixel_scale << '\t' << d.capture_size_px.width << '\t'
                << d.capture_size_px.height << '\n';
    }
    return 0;
  }

  auto capture = std::make_shared<MacCapturePort>(log);
  HeadlessCapture session(catalog, capture);
  auto snapshot = wait_for_capture<ExportSnapshot>(app, [&](auto completion) {
    session.begin(command, primary, settings.value(), std::move(completion));
  }, [&session] { session.cancel(); });
  if (!snapshot) return fail(snapshot.error());

  auto processor = MacMetalExportPixelProcessor::create();
  if (!processor) return fail(processor.error());
  LibUltraHdrEncoder encoder;
  MacClockPort clock;
  PosixFileStorePort files;
  const auto started = std::chrono::steady_clock::now();
  auto event = export_diagnostic(snapshot.value(), "save", "encode_started");
  event.outcome = "started";
  if (log->detailed_logging()) (void)log->record(event);
  const auto exported = ExportWorkflow::save_default(
      snapshot.value(), clock, files, nullptr, processor.value().get(),
      processor.value().get(), &encoder, processor.value().get());
  record_export_result(log.get(), event, exported,
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started).count());
  if (!exported) return failed(exported.error());
  const auto* receipt = std::get_if<FileReceipt>(&exported.value().destination);
  if (!receipt) return fail({ErrorCode::state_inconsistent, "MacCaptureCli",
                              Retryability::never, {{"reason", "missing_file_receipt"}}});
  std::cout << receipt->exact_path << '\n';
  return 0;
}
}  // namespace hdrshot
