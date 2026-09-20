#include "application/export_completion_coordinator.hpp"
#include "platform/macos/macos_qt_analyzer_controller.hpp"
#include "platform/macos/macos_capture_cli.hpp"
#include "platform/macos/macos_qt_overlay_window.hpp"
#include "ui/qt/qt_overlay_host.hpp"
#include "platform/macos/macos_qt_window_activation.hpp"
#include "platform/qt/qt_export_task_executor.hpp"
#include "application/capture_session.hpp"
#include "platform/macos/macos_window_catalog.hpp"
#include "application/capture_interaction_session.hpp"
#include "ui/qt/capture_error_message.hpp"
#include "application/export_workflow.hpp"
#include "application/multi_display_preview_presenter.hpp"
#include "adapters/shared/libultrahdr_encoder.hpp"
#include "domain/annotation/annotation_document.hpp"
#include "domain/geometry/selection_model.hpp"
#include "platform/macos/macos_capture_ports.hpp"
#include "platform/macos/macos_export_ports.hpp"
#include "platform/macos/macos_global_hotkey_port.hpp"
#include "platform/macos/macos_metal_export_pixel_processor.hpp"
#include "platform/macos/macos_qt_input_platform_adapter.hpp"
#include "platform/macos/macos_preview_presenter_port.hpp"
#include "platform/freetype/freetype_text_rasterizer_port.hpp"
#include "platform/qt/qt_session_diagnostics_port.hpp"
#include "core/build_metadata.hpp"
#include "platform/qt/qt_folder_opener_port.hpp"
#include "platform/qt/qt_settings_store_port.hpp"
#include "ui/qt/overlay_editor_widget.hpp"
#include "ui/qt/settings_window.hpp"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QIcon>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmap>
#include <QPointer>
#include <QResizeEvent>
#include <QScreen>
#include <QStandardPaths>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QThreadPool>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <QtGui/qscreen_platform.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

constexpr const char* kDefaultGlobalHotkey = "Command+Shift+2";

std::uint32_t display_id(NSScreen* screen) {
  const auto key = NSDeviceDescriptionKey(@"NSScreenNumber");
  NSNumber* number = screen.deviceDescription[key];
  return number == nil ? 0U : number.unsignedIntValue;
}

QScreen* find_qt_screen(const std::uint32_t target_display_id) {
  for (QScreen* screen : QGuiApplication::screens()) {
    auto* cocoa = screen->nativeInterface<QNativeInterface::QCocoaScreen>();
    if (cocoa != nullptr && display_id(cocoa->nativeScreen()) == target_display_id) {
      return screen;
    }
  }
  return nullptr;
}

void print_error(const hdrshot::Error& error) {
  std::cerr << hdrshot::to_string(error.code);
  for (const auto& [key, value] : error.safe_context) {
    std::cerr << ' ' << key << '=' << value;
  }
  std::cerr << '\n';
}

using QtOverlayHost = hdrshot::QtOverlayHost;

struct DisplayOverlayRuntime {
  hdrshot::DisplayId display_id{};
  QtOverlayHost* host{};
  std::shared_ptr<hdrshot::MacMetalEdrPresenter> native_presenter;
  std::shared_ptr<hdrshot::MacPreviewPresenterPort> preview;
};

class MacApplicationShell final {
 public:
  explicit MacApplicationShell(QApplication& app)
      : app_(app),
        hotkey_(std::make_unique<hdrshot::MacGlobalHotkeyPort>()),
        export_executor_(app),
        clipboard_(std::make_shared<hdrshot::MacClipboardPort>()),
        file_store_(std::make_shared<hdrshot::PosixFileStorePort>()),
        file_dialog_(std::make_shared<hdrshot::MacFileDialogPort>()),
        clock_(std::make_shared<hdrshot::MacClockPort>()),
        folder_opener_(std::make_unique<hdrshot::QtFolderOpenerPort>()) {}

  [[nodiscard]] bool initialize() {
    const auto pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) +
        QStringLiteral("/SeriousShot");
    const auto config = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
        QStringLiteral("/settings.ini");
    const auto diagnostics_folder =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
        QStringLiteral("/logs");
    diagnostics_folder_ = diagnostics_folder.toStdString();
    auto session_log = std::make_shared<hdrshot::QtSessionDiagnosticsPort>(diagnostics_folder_);
    const auto log_started = session_log->start(std::string(hdrshot::build_timestamp()), "macOS");
    if (!log_started) print_error(log_started.error());
    diagnostics_ = std::move(session_log);
    settings_store_ = std::make_unique<hdrshot::QtSettingsStorePort>(
        config.toStdString(), kDefaultGlobalHotkey, pictures.toStdString());
    const auto loaded = settings_store_->load();
    if (!loaded) {
      hdrshot::record_diagnostic_failure(diagnostics_.get(), "startup", loaded.error());
      std::cerr << "Settings load failed: ";
      print_error(loaded.error());
      return false;
    }
    settings_ = loaded.value();
    diagnostics_->set_detailed_logging(settings_.detailed_logging);
    record_runtime_environment();
    auto export_processor = hdrshot::MacMetalExportPixelProcessor::create();
    if (!export_processor) {
      hdrshot::record_diagnostic_failure(diagnostics_.get(), "startup", export_processor.error());
      std::cerr << "Metal export processor creation failed: ";
      print_error(export_processor.error());
      return false;
    }
    export_pixel_processor_ =
        std::shared_ptr<hdrshot::MacMetalExportPixelProcessor>(
            std::move(export_processor.value()));
    ultra_hdr_encoder_ = std::make_shared<hdrshot::LibUltraHdrEncoder>();

    constexpr const char* font_identity =
        "NotoSansSC-VF.ttf@f8d1575;wght=450;hinting=native#d68bafcb48a2707749396aa12bbbd833cb70401f3a9a689fd2902c7e0d295964";
    const auto font_path = QCoreApplication::applicationDirPath() +
        QStringLiteral("/assets/fonts/NotoSansSC-VF.ttf");
    text_rasterizer_ = std::make_shared<hdrshot::FreeTypeTextRasterizerPort>(
        font_path.toStdString(), font_identity);
    const auto font_id = QFontDatabase::addApplicationFont(font_path);
    const auto families = QFontDatabase::applicationFontFamilies(font_id);
    if (font_id < 0 || families.isEmpty()) {
      hdrshot::record_diagnostic_failure(diagnostics_.get(), "startup",
          hdrshot::Error{hdrshot::ErrorCode::invalid_input, "font", hdrshot::Retryability::never, {}});
      std::cerr << "Qt text editor font registration failed path="
                << font_path.toStdString() << '\n';
      return false;
    }
    text_editor_font_family_ = families.front().toStdString();

    completion_ = std::make_unique<hdrshot::ExportCompletionCoordinator>(
        hdrshot::ExportCompletionPorts{
            *file_store_, *file_dialog_, *clipboard_, *clock_, export_executor_,
            text_rasterizer_.get(), export_pixel_processor_.get(),
            export_pixel_processor_.get(), ultra_hdr_encoder_.get(), export_pixel_processor_.get()},
        [this](bool active, hdrshot::DisplayId display) {
          set_capture_overlays_system_dialog_active(active, display);
        }, hdrshot::ExportCompletionCoordinator::Event{}, diagnostics_.get());
    analyzer_ = std::make_unique<hdrshot::MacQtAnalyzerController>(app_,
        [this](auto command, auto snapshot, auto completed) {
          return request_export(command, std::move(snapshot), std::move(completed));
        }, [this] { return settings_; }, [this](std::string preferences) {
          hdrshot::SettingsPatch patch;
          patch.analyzer_preferences_json = std::move(preferences);
          const auto saved = settings_store_->save(patch);
          if (saved) {
            const auto current = settings_store_->load();
            if (current) settings_ = current.value();
          }
          else hdrshot::record_diagnostic_failure(diagnostics_.get(), "analysis.preferences", saved.error());
        }, diagnostics_);
    create_tray();
    hotkey_->set_trigger_handler([this] {
      hdrshot::record_diagnostic_stage(diagnostics_.get(), {}, {},
          "trigger", "hotkey_callback", "received", {{"source", "hotkey"}});
      QMetaObject::invokeMethod(
          &app_, [this] { start_capture("hotkey"); }, Qt::QueuedConnection);
    });
    const auto registered = hotkey_->register_hotkey(settings_.global_capture_hotkey);
    if (!registered) {
      hdrshot::record_diagnostic_failure(diagnostics_.get(), "hotkey", registered.error());
      std::cerr << "Global hotkey registration failed: ";
      print_error(registered.error());
      tray_->showMessage(
          QStringLiteral("SeriousShot 快捷键不可用"),
          QStringLiteral("请从菜单栏打开设置并更换全局快捷键。"),
          QSystemTrayIcon::Warning,
          5000);
    } else {
      hotkey_registered_ = true;
    }

    std::cout << "appShellReady=1 hotkey=" << settings_.global_capture_hotkey
              << " settingsPath=" << config.toStdString() << '\n';
    // Every cold process start opens settings, including existing profiles.
    // show_settings() already defers presentation until the event loop runs.
    // The legacy initial-settings flag must not gate or be written by startup.
    show_settings();
    return true;
  }

  void shutdown() {
    if (analyzer_ && !analyzer_->close_windows()) return;
    if (hotkey_registered_) {
      const auto unregistered = hotkey_->unregister_hotkey();
      if (!unregistered) {
        std::cerr << "Global hotkey unregister failed: ";
        print_error(unregistered.error());
      }
      hotkey_registered_ = false;
    }
    if (tray_ != nullptr) {
      tray_->hide();
    }
    finish_capture();
    if (analyzer_) analyzer_->shutdown();
    export_executor_.drain();
    app_.quit();
  }

 private:
  void create_tray() {
    tray_ = new QSystemTrayIcon(
        QIcon::fromTheme(
            QStringLiteral("camera-photo"),
            app_.style()->standardIcon(QStyle::SP_ComputerIcon)),
        &app_);
    tray_->setToolTip(QStringLiteral("SeriousShot"));
    tray_menu_ = new QMenu();
    capture_action_ = tray_menu_->addAction(QString());
    QObject::connect(capture_action_, &QAction::triggered, &app_, [this] { start_capture("menu"); });
    auto* settings_action = tray_menu_->addAction(QStringLiteral("设置…"));
    QObject::connect(settings_action, &QAction::triggered, &app_, [this] {
      show_settings();
    });
    tray_menu_->addSeparator();
    auto* quit_action = tray_menu_->addAction(QStringLiteral("退出 SeriousShot"));
    QObject::connect(quit_action, &QAction::triggered, &app_, [this] { shutdown(); });
    // On macOS the native context-menu path opens on mouse-down. During the
    // first accessory-app activation that same event can also deactivate and
    // immediately close the newly created menu. Finish the status-item click
    // first, then open the existing QMenu from the queued activation signal.
    QObject::connect(
        tray_, &QSystemTrayIcon::activated, &app_,
        [this](const QSystemTrayIcon::ActivationReason reason) {
      if (reason != QSystemTrayIcon::Trigger &&
          reason != QSystemTrayIcon::Context) {
        return;
      }
      QTimer::singleShot(0, &app_, [this] { show_tray_menu(); });
    });
    tray_menu_->ensurePolished();
    tray_menu_->adjustSize();
    update_capture_action_text();
    tray_->show();
  }

  void show_tray_menu() {
    if (tray_menu_ == nullptr || tray_menu_->isVisible()) {
      return;
    }
    const auto tray_geometry = tray_->geometry();
    const auto position = tray_geometry.isEmpty()
        ? QCursor::pos()
        : QPoint(tray_geometry.left(), tray_geometry.bottom() + 1);
    tray_menu_->popup(position);
  }

  void show_settings() {
    if (settings_open_queued_) return;
    settings_open_queued_ = true;
    // Let QMenu/native menu tracking restore focus first. Then activate the
    // accessory application and its window, once, on the next event-loop turn.
    if (tray_menu_ != nullptr) tray_menu_->hide();
    QTimer::singleShot(0, &app_, [this] {
      settings_open_queued_ = false;
      show_settings_after_menu();
    });
  }

  void show_settings_after_menu() {
    if (settings_window_ == nullptr) {
      settings_window_ = new hdrshot::SettingsWindow(
          *settings_store_,
          *hotkey_,
          *folder_opener_,
          input_platform_,
          kDefaultGlobalHotkey,
          diagnostics_folder_);
      settings_window_->set_applied(
          [this](const hdrshot::SettingsSnapshot& settings) {
        settings_ = settings;
        diagnostics_->set_detailed_logging(settings_.detailed_logging);
        record_runtime_environment();
        update_capture_action_text();
      });
      QObject::connect(
          settings_window_, &QObject::destroyed, &app_,
          [this] { settings_window_ = nullptr; });
    }
    settings_window_->set_window_activation_port(&settings_activation_);
    settings_window_->reload_and_show();
  }

  void record_runtime_environment() {
    if (!diagnostics_ || !diagnostics_->detailed_logging()) return;
    const auto version = NSProcessInfo.processInfo.operatingSystemVersion;
    hdrshot::record_diagnostic_stage(diagnostics_.get(), {}, {}, "runtime", "environment", "observed",
        {{"osVersion", std::to_string(version.majorVersion) + "." +
             std::to_string(version.minorVersion) + "." + std::to_string(version.patchVersion)},
         {"api", "captureImageWithFilter"}, {"requestedSpace", "ExtendedDisplayP3"}});
  }

  void record_capture_event(
      const hdrshot::SessionId session_id,
      const hdrshot::OperationId operation_id,
      std::string stage,
      std::string outcome,
      const hdrshot::Error* error = nullptr) {
    hdrshot::record_diagnostic_stage(diagnostics_.get(), session_id, operation_id,
        "capture", stage, outcome, {}, error);
  }

  void update_capture_action_text() {
    if (capture_action_ != nullptr) {
      capture_action_->setText(
          QStringLiteral("截图（%1）")
              .arg(QString::fromStdString(settings_.global_capture_hotkey)));
    }
  }

  void report_background_export_result(const QtOverlayHost::ExportResult& result) {
    if (!result) {
      tray_->showMessage(
          QStringLiteral("SeriousShot 后台导出失败"),
          QStringLiteral("错误：%1。请重新截图后重试。")
              .arg(QString::fromStdString(hdrshot::to_string(result.error().code))),
          QSystemTrayIcon::Critical,
          5000);
      return;
    }
    if (const auto* file = std::get_if<hdrshot::FileReceipt>(
            &result.value().destination)) {
      tray_->showMessage(
          QStringLiteral("SeriousShot 已保存"),
          QString::fromStdString(file->exact_path),
          QSystemTrayIcon::Information,
          2500);
    }
  }


  bool request_export(
      const hdrshot::UiCommand command, hdrshot::ExportSnapshot snapshot,
      QtOverlayHost::ExportCompleted completed) {
    return completion_->submit(command, std::move(snapshot),
        [this, completed = std::move(completed)](QtOverlayHost::ExportResult result) mutable {
          report_background_export_result(result);
          completed(std::move(result));
        });
  }

  void set_capture_overlays_system_dialog_active(
      const bool active,
      const hdrshot::DisplayId owner_display_id) {
    for (auto& overlay : display_overlays_) {
      if (overlay.host != nullptr) {
        overlay.host->set_system_dialog_active(
            active,
            !active && overlay.display_id == owner_display_id);
      }
    }
  }

  void start_capture(const char* source = "app") {
    const auto requested_session = capture_lifecycle_.active() ? capture_lifecycle_.id()
        : hdrshot::SessionId{next_session_id_};
    hdrshot::record_diagnostic_stage(diagnostics_.get(), requested_session, hdrshot::OperationId{1},
        "trigger", "received", capture_lifecycle_.active() ? "ignored" : "accepted",
        {{"source", source}});
    if (capture_lifecycle_.active()) {
      tray_->showMessage(
          QStringLiteral("SeriousShot"),
          QStringLiteral("截图界面已经打开。"),
          QSystemTrayIcon::Information,
          2000);
      return;
    }
    const auto screens = NSScreen.screens;
    if (screens.count == 0) {
      const hdrshot::Error error{hdrshot::ErrorCode::capture_failed, "MacApplicationShell",
          hdrshot::Retryability::after_user_action, {{"reason", "no_available_screen"}}};
      record_capture_event(requested_session, hdrshot::OperationId{1}, "screens", "failure", &error);
      tray_->showMessage(
          QStringLiteral("SeriousShot"),
          QStringLiteral("没有可用显示器。"),
          QSystemTrayIcon::Critical,
          3000);
      return;
    }
    const auto session_id = hdrshot::SessionId{next_session_id_++};
    if (!capture_lifecycle_.begin(session_id)) return;
    const auto operation_id = hdrshot::OperationId{1};
    const auto initial_selection = hdrshot::SelectionSnapshot{
        1,
        {},
    };
    std::vector<hdrshot::DisplayId> targets;
    std::vector<hdrshot::DisplayPreviewEndpoint> endpoints;
    display_overlays_.clear();
    auto input_recovery = std::make_shared<hdrshot::OverlayInputRecovery>();
    for (NSScreen* screen in screens) {
      const auto native_display_id = display_id(screen);
      QScreen* qt_screen = find_qt_screen(native_display_id);
      if (native_display_id == 0U || qt_screen == nullptr) {
        hdrshot::record_diagnostic_stage(diagnostics_.get(), session_id, operation_id,
            "overlay", "screen_mapping", "ignored",
            {{"displayId", std::to_string(native_display_id)}, {"reason", "qt_screen_unavailable"}});
        continue;
      }
      auto* host = new QtOverlayHost(
          std::make_unique<hdrshot::MacQtOverlayWindow>((__bridge void*)screen),
          hdrshot::DisplayId{native_display_id},
          initial_selection,
          input_platform_, input_recovery);
      host->windowHandle()->setScreen(qt_screen);
      host->setGeometry(qt_screen->geometry());
      host->prepare_hidden_native_surface();
      host->set_diagnostics(diagnostics_);
      host->set_text_rasterizer(text_rasterizer_, text_editor_font_family_);
      host->set_clean_composition(export_pixel_processor_);
      host->set_finished([this, session_id] { finish_capture(session_id); });
      host->set_interaction_allowed(
          [this, session_id](const hdrshot::DisplayId activated) {
            return capture_lifecycle_.allows(session_id, activated);
          });
      host->set_initial_gesture_changed(
          [this, session_id](const hdrshot::DisplayId display, bool begin) {
            return capture_lifecycle_.initial_gesture(session_id, display, begin);
          });
      host->set_selection_established(
          [this, session_id](const hdrshot::DisplayId selected) {
            (void)lock_target_display(session_id, selected);
          });
      host->set_export_requested(
          [this](
              const hdrshot::UiCommand command,
              hdrshot::ExportSnapshot snapshot,
              QtOverlayHost::ExportCompleted completed) -> bool {
            return request_export(command, std::move(snapshot), std::move(completed));
          },
          settings_);
      host->set_analysis_requested([this](auto snapshot, auto completed) {
        analyzer_->open(std::move(snapshot), std::move(completed));
      });

      auto created = hdrshot::MacMetalEdrPresenter::create();
      if (!created) {
        std::cerr << "Presenter creation failed displayId=" << native_display_id << ' ';
        print_error(created.error());
        record_capture_event(session_id, operation_id, "presenter_create", "failure", &created.error());
        host->deleteLater();
        finish_capture();
        return;
      }
      auto native_presenter = std::shared_ptr<hdrshot::MacMetalEdrPresenter>(
          std::move(created.value()));
      const auto surface_range =
          screen.maximumExtendedDynamicRangeColorComponentValue > 1.0
          ? hdrshot::MacMetalSurfaceRange::edr
          : hdrshot::MacMetalSurfaceRange::sdr;
      auto configured = native_presenter->configure_target_layer(
          host->native_surface(), surface_range);
      if (!configured) {
        std::cerr << "Metal layer configuration failed displayId="
                  << native_display_id << ' ';
        print_error(configured.error());
        record_capture_event(session_id, operation_id, "layer_configure", "failure", &configured.error());
        host->deleteLater();
        finish_capture();
        return;
      }
      hdrshot::record_diagnostic_stage(diagnostics_.get(), session_id, operation_id,
          "overlay", "layer_configured", "success", {
              {"displayId", std::to_string(native_display_id)},
              {"surface", surface_range == hdrshot::MacMetalSurfaceRange::edr ? "edr" : "sdr"},
              {"requestedSpace", "ExtendedLinearDisplayP3"}, {"requestedFormat", "RGBA16Float"},
              {"currentEDR", std::to_string(screen.maximumExtendedDynamicRangeColorComponentValue)},
              {"potentialEDR", std::to_string(screen.maximumPotentialExtendedDynamicRangeColorComponentValue)}});
      auto preview = std::make_shared<hdrshot::MacPreviewPresenterPort>(
          native_presenter,
          host->native_surface(),
          static_cast<double>(screen.maximumExtendedDynamicRangeColorComponentValue));
      targets.push_back(hdrshot::DisplayId{native_display_id});
      endpoints.push_back(hdrshot::DisplayPreviewEndpoint{
          hdrshot::DisplayId{native_display_id}, preview});
      display_overlays_.push_back(DisplayOverlayRuntime{
          hdrshot::DisplayId{native_display_id},
          host,
          std::move(native_presenter),
          std::move(preview),
      });
      std::cout << "overlayDisplayPrepared displayId=" << native_display_id
                << " currentMaximumEDR="
                << screen.maximumExtendedDynamicRangeColorComponentValue
                << " maximumPotentialEDR="
                << screen.maximumPotentialExtendedDynamicRangeColorComponentValue
                << " surfacePolicy="
                << (surface_range == hdrshot::MacMetalSurfaceRange::edr ? "edr" : "sdr")
                << '\n';
    }
    if (display_overlays_.empty()) {
      const hdrshot::Error error{hdrshot::ErrorCode::presenter_failed, "MacApplicationShell",
          hdrshot::Retryability::after_user_action, {{"reason", "no_overlay_surface"}}};
      record_capture_event(session_id, operation_id, "surface_prepare", "failure", &error);
      tray_->showMessage(
          QStringLiteral("SeriousShot"),
          QStringLiteral("没有可用显示器。"),
          QSystemTrayIcon::Critical,
          3000);
      finish_capture();
      return;
    }
    multi_preview_ = std::make_shared<hdrshot::MultiDisplayPreviewPresenterPort>(
        std::move(endpoints));
    catalog_ = std::make_shared<hdrshot::MacDisplayCatalogPort>(diagnostics_);
    capture_ = std::make_shared<hdrshot::MacCapturePort>(diagnostics_);
    capture_session_ = std::make_shared<hdrshot::CaptureSession>(
        catalog_, capture_, multi_preview_, window_catalog_, diagnostics_);
    capture_session_->begin(
        hdrshot::BeginCaptureRequest{
            session_id,
            operation_id,
            hdrshot::FrameId{session_id.value},
            std::move(targets),
            initial_selection,
        },
        [this, session_id, operation_id](
            hdrshot::Result<hdrshot::CaptureReadyPayload, hdrshot::Error> result) mutable {
      QMetaObject::invokeMethod(
          &app_,
          [this, session_id, operation_id, result = std::move(result)]() mutable {
            if (!capture_lifecycle_.accepts(session_id)) return;
            if (!result) {
              record_capture_event(
                  session_id, operation_id, "prepare_overlay", "failure", &result.error());
              std::cerr << "CaptureSession failed: ";
              print_error(result.error());
              tray_->showMessage(
                  QStringLiteral("SeriousShot 截图失败"),
                  hdrshot::capture_error_message(result.error(), QStringLiteral(
                      "尚未获得屏幕录制权限。请在系统设置 → 隐私与安全性 → "
                      "录屏与系统录音中允许 SeriousShot，再按截图快捷键或点菜单“截图”重试。"
                      "应用只捕获画面，不录音。")),
                  QSystemTrayIcon::Critical,
                  10000);
              finish_capture();
              return;
            }
            auto payload = std::move(result.value());
            if (!capture_lifecycle_.ready(payload.present_receipt)) {
              finish_capture(session_id);
              return;
            }
            const auto all_targets_present = std::all_of(
                display_overlays_.begin(), display_overlays_.end(),
                [&payload](const DisplayOverlayRuntime& overlay) {
                  return std::any_of(
                      payload.frozen_desktop->canonical_segments.begin(),
                      payload.frozen_desktop->canonical_segments.end(),
                      [&overlay](const hdrshot::CanonicalFrameSegment& segment) {
                        return segment.display_id == overlay.display_id;
                      });
                });
            if (!all_targets_present) {
              const auto error = hdrshot::Error{
                  hdrshot::ErrorCode::display_configuration_changed,
                  "MacApplicationShell",
                  hdrshot::Retryability::same_input,
                  {{"reason", "incomplete_multi_display_frame"}},
              };
              record_capture_event(
                  session_id, operation_id, "prepare_overlay", "failure", &error);
              std::cerr << "CaptureSession returned an incomplete multi-display frame\n";
              tray_->showMessage(
                  QStringLiteral("SeriousShot 截图失败"),
                  QStringLiteral("多显示器捕获结果不完整"),
                  QSystemTrayIcon::Critical,
                  4000);
              finish_capture();
              return;
            }
            auto operation_sequence = std::make_shared<std::atomic<std::uint64_t>>(
                payload.present_receipt.operation_id.value + 1U);
            for (auto& overlay : display_overlays_) {
              if (overlay.host != nullptr) {
                overlay.host->activate_capture(
                    payload, overlay.preview, operation_sequence);
                hdrshot::record_diagnostic_stage(diagnostics_.get(), session_id, operation_id,
                    "overlay", "shown", "success", {{"displayId", std::to_string(overlay.display_id.value)}});
              }
            }
          },
          Qt::QueuedConnection);
    });
  }

  [[nodiscard]] bool lock_target_display(
      const hdrshot::SessionId session_id, const hdrshot::DisplayId display_id) {
    if (!capture_lifecycle_.lock(session_id, display_id)) return false;
    for (auto& overlay : display_overlays_) {
      if (overlay.display_id != display_id && overlay.host != nullptr) {
        overlay.host->set_interaction_locked(true);
      }
    }
    std::cout << "selectionTargetLocked displayId=" << display_id.value << '\n';
    return true;
  }

  void finish_capture(hdrshot::SessionId expected = {}) {
    if (expected.value == 0) expected = capture_lifecycle_.id();
    if (!capture_lifecycle_.finish(expected)) return;
    // Invalidate before cancellation can queue callbacks. Native resources are
    // then released; stale callbacks cannot close a newer session.
    if (capture_session_) capture_session_->cancel(expected, hdrshot::OperationId{1});
    for (auto& overlay : display_overlays_) {
      if (overlay.host != nullptr) {
        overlay.host->hide();
        overlay.host->deleteLater();
        overlay.host = nullptr;
      }
    }
    capture_session_.reset();
    multi_preview_.reset();
    display_overlays_.clear();
    capture_.reset();
    catalog_.reset();
    std::cout << "captureSessionReturnedToResident=1\n";
  }

  QApplication& app_;
  QSystemTrayIcon* tray_{};
  QMenu* tray_menu_{};
  QAction* capture_action_{};
  hdrshot::SettingsWindow* settings_window_{};
  hdrshot::MacQtWindowActivation settings_activation_;
  bool settings_open_queued_{};
  std::unique_ptr<hdrshot::QtSettingsStorePort> settings_store_;
  std::shared_ptr<hdrshot::DiagnosticsPort> diagnostics_;
  std::unique_ptr<hdrshot::MacGlobalHotkeyPort> hotkey_;
  hdrshot::MacQtInputPlatformAdapter input_platform_;
  hdrshot::SettingsSnapshot settings_{};
  hdrshot::QtExportTaskExecutor export_executor_;
  std::unique_ptr<hdrshot::ExportCompletionCoordinator> completion_;
  std::unique_ptr<hdrshot::MacQtAnalyzerController> analyzer_;
  bool hotkey_registered_{};
  hdrshot::CaptureInteractionSession capture_lifecycle_;
  std::uint64_t next_session_id_{1};
  std::shared_ptr<hdrshot::MacClipboardPort> clipboard_;
  std::shared_ptr<hdrshot::PosixFileStorePort> file_store_;
  std::shared_ptr<hdrshot::MacFileDialogPort> file_dialog_;
  std::shared_ptr<hdrshot::MacClockPort> clock_;
  std::unique_ptr<hdrshot::QtFolderOpenerPort> folder_opener_;
  std::string diagnostics_folder_;
  std::shared_ptr<hdrshot::TextRasterizerPort> text_rasterizer_;
  std::shared_ptr<hdrshot::MacMetalExportPixelProcessor> export_pixel_processor_;
  std::shared_ptr<hdrshot::LibUltraHdrEncoder> ultra_hdr_encoder_;
  std::string text_editor_font_family_;
  std::vector<DisplayOverlayRuntime> display_overlays_;
  std::shared_ptr<hdrshot::MacDisplayCatalogPort> catalog_;
  // Reuse across sessions so an unresponsive OS metadata query cannot spawn
  // an additional worker for every subsequent capture; those fall back manual.
  std::shared_ptr<hdrshot::MacWindowCatalogPort> window_catalog_ =
      std::make_shared<hdrshot::MacWindowCatalogPort>();
  std::shared_ptr<hdrshot::MacCapturePort> capture_;
  std::shared_ptr<hdrshot::MultiDisplayPreviewPresenterPort> multi_preview_;
  std::shared_ptr<hdrshot::CaptureSession> capture_session_;
};

}  // namespace

int main(int argc, char** argv) {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;
  std::vector<std::string_view> arguments;
  for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
  const auto command = hdrshot::parse_capture_command(arguments);
  if (!command) {
    print_error(command.error());
    std::cerr << hdrshot::capture_command_help();
    return 2;
  }
  if (command.value().mode == hdrshot::CaptureCommandMode::help) {
    std::cout << hdrshot::capture_command_help();
    return 0;
  }
  QApplication app(argc, argv);
  QApplication::setOrganizationName(QStringLiteral("Overdrive"));
  // SeriousShot is the stable release storage identity. Pre-release HDRShot /
  // XDRShot configuration is intentionally not migrated.
  QApplication::setApplicationName(QStringLiteral("SeriousShot"));
  QApplication::setApplicationDisplayName(QStringLiteral("SeriousShot"));
  app.setQuitOnLastWindowClosed(false);
  [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

  if (command.value().mode != hdrshot::CaptureCommandMode::gui)
    return hdrshot::run_macos_capture_cli(app, command.value());

  MacApplicationShell shell(app);
  if (!shell.initialize()) {
    return 1;
  }
  return app.exec();
}
