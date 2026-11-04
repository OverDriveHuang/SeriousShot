#include "platform/qt/qt_application_restart.hpp"
#include "platform/ui_language.hpp"
#include "ui/qt/ui_language_runtime.hpp"
#include "platform/macos/macos_login_startup_port.hpp"
#include "platform/macos/macos_launch_origin.hpp"
#include "platform/qt/qt_gui_instance_guard.hpp"
#include "platform/macos/macos_magnifier_card_presenter.hpp"
#include "platform/qt/update/qt_update_runtime.hpp"
#include "platform/qt/update/application_paths.hpp"
#include "application/export_completion_coordinator.hpp"
#include "platform/macos/macos_qt_analyzer_controller.hpp"
#include "platform/macos/macos_qt_pinned_screenshot.hpp"
#include "platform/macos/macos_qt_save_feedback_window.hpp"
#include "platform/macos/macos_capture_cli.hpp"
#include "platform/macos/macos_qt_overlay_window.hpp"
#include "ui/qt/qt_overlay_host.hpp"
#include "platform/macos/macos_qt_window_activation.hpp"
#include "platform/qt/qt_export_task_executor.hpp"
#include "application/capture_session.hpp"
#include "platform/macos/macos_window_catalog.hpp"
#include "application/capture_interaction_session.hpp"
#include "ui/qt/capture_error_message.hpp"
#include "platform/qt/qt_capture_runtime.hpp"
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
#include "ports/annotation_font_contract.hpp"
#include "platform/qt/qt_session_diagnostics_port.hpp"
#include "core/build_metadata.hpp"
#include "platform/qt/qt_folder_opener_port.hpp"
#include "platform/qt/qt_settings_store_port.hpp"
#include "ui/qt/overlay_editor_widget.hpp"
#include "ui/qt/settings_window.hpp"
#include "ui/qt/save_success_feedback.hpp"

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
#include <QMessageBox>
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
  MacApplicationShell(QApplication& app,
      std::optional<hdrshot::QtApplicationRestartRequest>& restart_request)
      : app_(app), restart_request_(restart_request),
        hotkey_(std::make_unique<hdrshot::MacGlobalHotkeyPort>()),
        export_executor_(dispatch_context_),
        save_feedback_(nullptr, hdrshot::configure_macos_save_feedback_window),
        clipboard_(std::make_shared<hdrshot::MacClipboardPort>()),
        file_store_(std::make_shared<hdrshot::PosixFileStorePort>()),
        file_dialog_(std::make_shared<hdrshot::MacFileDialogPort>()),
        clock_(std::make_shared<hdrshot::MacClockPort>()),
        folder_opener_(std::make_unique<hdrshot::QtFolderOpenerPort>()) {}

  ~MacApplicationShell() {
    updater_.reset();
    delete settings_window_;
    if (tray_) tray_->setContextMenu(nullptr);
    delete tray_;
    delete tray_menu_;
  }

  [[nodiscard]] bool initialize() {
    const auto paths = hdrshot::update::application_paths(
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation),
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
            QStringLiteral("/settings.ini"),
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) +
            QStringLiteral("/SeriousShot"),
        hdrshot::update::compiled_trust());
    diagnostics_folder_ = QDir(paths.data).filePath(QStringLiteral("logs")).toStdString();
    auto session_log = std::make_shared<hdrshot::QtSessionDiagnosticsPort>(diagnostics_folder_);
    const auto log_started = session_log->start(std::string(hdrshot::build_timestamp()), "macOS");
    if (!log_started) print_error(log_started.error());
    diagnostics_ = std::move(session_log);
    capture_ = std::make_shared<hdrshot::MacCapturePort>(diagnostics_);
    settings_store_ = std::make_unique<hdrshot::QtSettingsStorePort>(
        paths.config.toStdString(), kDefaultGlobalHotkey, paths.pictures.toStdString());
    const auto loaded = settings_store_->load();
    if (!loaded) {
      hdrshot::record_diagnostic_failure(diagnostics_.get(), "startup", loaded.error());
      std::cerr << "Settings load failed: ";
      print_error(loaded.error());
      return false;
    }
    settings_ = loaded.value();
    diagnostics_->set_detailed_logging(settings_.detailed_logging);
    if (!initialize_language()) return false;
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

    constexpr const char* font_identity = hdrshot::annotation_font::identity;
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
    analyzer_ = std::make_unique<hdrshot::MacQtAnalyzerController>(dispatch_context_,
        [this](auto command, auto snapshot, auto completed, QWidget *source) {
          return request_export(command, std::move(snapshot), std::move(completed), source);
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
    pinned_ = std::make_unique<hdrshot::QtPinnedScreenshotController>(
        dispatch_context_, hdrshot::prepare_macos_pinned_screenshot,
        hdrshot::make_macos_pinned_window_port(),
        [this](auto command, auto snapshot, auto completed, QWidget *source) {
          return request_export(command, std::move(snapshot), std::move(completed), source);
        }, [this](auto snapshot, auto completed) {
          analyzer_->open(std::move(snapshot), std::move(completed));
        }, [this] { return settings_; }, diagnostics_);
    create_tray();
    hotkey_->set_trigger_handler([this] {
      hdrshot::record_diagnostic_stage(diagnostics_.get(), {}, {},
          "trigger", "hotkey_callback", "received", {{"source", "hotkey"}});
      capture_runtime_.handle()->post([this] { start_capture("hotkey"); });
    });
    const auto registered = hotkey_->register_hotkey(settings_.global_capture_hotkey);
    if (!registered) {
      hdrshot::record_diagnostic_failure(diagnostics_.get(), "hotkey", registered.error());
      std::cerr << "Global hotkey registration failed: ";
      print_error(registered.error());
      tray_->showMessage(
          /*% "SeriousShot shortcut unavailable" */ qtTrId("tray.macos.shortcut_unavailable"),
          /*% "Open Settings from the menu bar and change the capture shortcut." */ qtTrId("tray.macos.change_shortcut"),
          QSystemTrayIcon::Warning,
          5000);
    } else {
      hotkey_registered_ = true;
    }

    std::cout << "appShellReady=1 hotkey=" << settings_.global_capture_hotkey
              << " settingsPath=" << paths.config.toStdString() << '\n';
    auto update_native=hdrshot::update::native_installer();
    auto update_identity=hdrshot::update::runtime_identity(update_native->install_identity());
    updater_=std::make_unique<hdrshot::update::Runtime>(std::move(update_identity),std::move(update_native),
        [this]{return bool(capture_session_) || export_executor_.pending() || (analyzer_&&analyzer_->has_windows()) || (pinned_&&pinned_->has_windows());},[this]{return shutdown();});
    updater_->set_interactive_presentation_enabled(false);
    updater_->start();
    return true;
  }

  void launch_ready(hdrshot::MacLaunchOrigin origin) {
    std::cout << "launchOrigin=" << (origin == hdrshot::MacLaunchOrigin::login ? "login" : "manual") << '\n';
    if (origin == hdrshot::MacLaunchOrigin::manual) show_settings();
  }

  bool shutdown() {
    if (shutting_down_ || close_confirmation_active_) return false;
    close_confirmation_active_ = true;
    const bool can_close = !analyzer_ || analyzer_->close_windows();
    close_confirmation_active_ = false;
    if (!can_close) return false;
    shutting_down_ = true;
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
    finish_capture({}, true);
    if (pinned_) pinned_->shutdown();
    if (analyzer_) analyzer_->shutdown();
    export_executor_.drain();
    app_.quit();
    return true;
  }

 private:
  hdrshot::UiRestartOutcome restart_application(hdrshot::UiLanguage language) {
    if (shutting_down_ || close_confirmation_active_ || restart_request_ || capture_lifecycle_.active() || capture_session_ ||
        (capture_ && capture_->busy()) || export_executor_.pending() ||
        (analyzer_ && (analyzer_->has_windows() || analyzer_->has_pending_work())) ||
        (pinned_ && pinned_->has_windows()) ||
        (updater_ && (updater_->barrier_active() || updater_->update_operation_active())))
      return hdrshot::UiRestartOutcome::busy;
    auto request = hdrshot::make_application_restart_request(language, language_runtime_);
    if (!shutdown()) return hdrshot::UiRestartOutcome::cancelled;
    restart_request_ = std::move(request);
    return hdrshot::UiRestartOutcome::accepted;
  }

  bool initialize_language() {
    const auto loaded = language_runtime_.initialize(
        app_, settings_.ui_language, hdrshot::detect_system_ui_language);
    if (!loaded) {
      hdrshot::record_diagnostic_failure(diagnostics_.get(), "ui_language", loaded.error());
      std::cerr << "SeriousShot could not load its English interface resources.\n";
      print_error(loaded.error());
      QMessageBox::critical(nullptr, QStringLiteral("SeriousShot"),
                            QStringLiteral("SeriousShot could not load its English interface resources."));
      return false;
    }
    const auto &receipt = loaded.value();
    const auto name = [](hdrshot::UiLanguage language) {
      return language == hdrshot::UiLanguage::simplified_chinese ? "zh-CN" : "en";
    };
    const auto *catalog_error = receipt.catalog_error ? &*receipt.catalog_error : nullptr;
    hdrshot::record_diagnostic_stage(diagnostics_.get(), {}, {}, "ui_language",
        std::string("requested_") + name(receipt.requested_language),
        std::string("active_") + name(receipt.active_language), {}, catalog_error);
    if (catalog_error) {
      hdrshot::record_diagnostic_failure(diagnostics_.get(), "ui_language", *catalog_error);
      print_error(*catalog_error);
    }
    return true;
  }

  void create_tray() {
    tray_ = new QSystemTrayIcon(
        QIcon::fromTheme(
            QStringLiteral("camera-photo"),
            app_.style()->standardIcon(QStyle::SP_ComputerIcon)),
        &dispatch_context_);
    tray_->setToolTip(QStringLiteral("SeriousShot"));
    tray_menu_ = new QMenu();
    capture_action_ = tray_menu_->addAction(QString());
    QObject::connect(capture_action_, &QAction::triggered, &dispatch_context_, [this] { start_capture("menu"); });
    auto* settings_action = tray_menu_->addAction(/*% "Settings…" */ qtTrId("tray.settings"));
    QObject::connect(settings_action, &QAction::triggered, &dispatch_context_, [this] {
      show_settings();
    });
    tray_menu_->addSeparator();
    auto* quit_action = tray_menu_->addAction(/*% "Quit SeriousShot" */ qtTrId("tray.quit"));
    QObject::connect(quit_action, &QAction::triggered, &dispatch_context_, [this] { shutdown(); });
    // On macOS the native context-menu path opens on mouse-down. During the
    // first accessory-app activation that same event can also deactivate and
    // immediately close the newly created menu. Finish the status-item click
    // first, then open the existing QMenu from the queued activation signal.
    QObject::connect(
        tray_, &QSystemTrayIcon::activated, &dispatch_context_,
        [this](const QSystemTrayIcon::ActivationReason reason) {
      if (reason != QSystemTrayIcon::Trigger &&
          reason != QSystemTrayIcon::Context) {
        return;
      }
      QTimer::singleShot(0, &dispatch_context_, [this] { show_tray_menu(); });
    });
    tray_menu_->ensurePolished();
    tray_menu_->adjustSize();
    update_capture_action_text();
    tray_->show();
  }

  void show_tray_menu() {
    if (shutting_down_ || close_confirmation_active_ || tray_menu_ == nullptr || tray_menu_->isVisible()) {
      return;
    }
    const auto tray_geometry = tray_->geometry();
    const auto position = tray_geometry.isEmpty()
        ? QCursor::pos()
        : QPoint(tray_geometry.left(), tray_geometry.bottom() + 1);
    tray_menu_->popup(position);
  }

  void show_settings() {
    if (shutting_down_ || close_confirmation_active_ || settings_open_queued_) return;
    settings_open_queued_ = true;
    // Let QMenu/native menu tracking restore focus first. Then activate the
    // accessory application and its window, once, on the next event-loop turn.
    if (tray_menu_ != nullptr) tray_menu_->hide();
    QTimer::singleShot(0, &dispatch_context_, [this] {
      settings_open_queued_ = false;
      show_settings_after_menu();
    });
  }

  void show_settings_after_menu() {
    if (shutting_down_ || close_confirmation_active_) return;
    if (updater_) updater_->allow_interactive_presentation();
    if (settings_window_ == nullptr) {
      settings_window_ = new hdrshot::SettingsWindow(
          *settings_store_,
          *hotkey_,
          *folder_opener_,
          input_platform_,
          kDefaultGlobalHotkey,
          diagnostics_folder_);
      settings_window_->set_language_runtime(language_runtime_);
      settings_window_->set_restart_requester([this](hdrshot::UiLanguage language) {
        return restart_application(language);
      });
      settings_window_->set_login_startup_port(&login_startup_);
      settings_window_->set_applied(
          [this](const hdrshot::SettingsSnapshot& settings) {
        settings_ = settings;
        diagnostics_->set_detailed_logging(settings_.detailed_logging);
        if (capture_) capture_->diagnostic_snapshot();
        if (catalog_) catalog_->diagnostic_snapshot();
        record_runtime_environment();
        update_capture_action_text();
      });
      QObject::connect(
          settings_window_, &QObject::destroyed, &dispatch_context_,
          [this] { settings_window_ = nullptr; });
    }
    if(updater_)updater_->attach(*settings_window_);
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

  void notify_capture_failure(const hdrshot::Error& error, hdrshot::SessionId session = {}, hdrshot::OperationId operation = {}) {
    if (!tray_) return;
    //% "SeriousShot capture failed"
    const auto title = qtTrId("tray.capture_failed");
    //% "Allow SeriousShot under System Settings → Privacy & Security → Screen & System Audio Recording. Press the capture shortcut or choose “Capture” to retry. SeriousShot captures the screen without recording audio."
    const auto guidance = qtTrId("tray.macos.capture_permission");
    tray_->showMessage(title, hdrshot::capture_error_message(
        error, guidance, QString::fromStdString(settings_.global_capture_hotkey)),
        QSystemTrayIcon::Critical, 10000);
    hdrshot::record_diagnostic_stage(diagnostics_.get(), session, operation,
        "capture", "notification_requested", "submitted", {}, nullptr, true);
  }

  void record_capture_event(
      const hdrshot::SessionId session_id,
      const hdrshot::OperationId operation_id,
      std::string stage,
      std::string outcome,
      const hdrshot::Error* error = nullptr) {
    hdrshot::record_diagnostic_stage(diagnostics_.get(), session_id, operation_id,
        "capture", stage, outcome, {}, error);
    if (error && outcome == "failure") notify_capture_failure(*error, session_id, operation_id);
  }

  void update_capture_action_text() {
    if (capture_action_ != nullptr) {
      capture_action_->setText(
          /*% "Capture (%1)" */ qtTrId("tray.capture_with_shortcut")
              .arg(QString::fromStdString(settings_.global_capture_hotkey)));
    }
  }

  void report_background_export_result(const QtOverlayHost::ExportResult& result) {
    if (!result) {
      tray_->showMessage(
          /*% "SeriousShot background export failed" */ qtTrId("tray.background_export_failed"),
          /*% "Error: %1. Capture the screen again and retry." */ qtTrId("tray.background_export_retry")
              .arg(QString::fromStdString(hdrshot::to_string(result.error().code))),
          QSystemTrayIcon::Critical,
          5000);
      return;
    }
  }


  bool request_export(
      const hdrshot::UiCommand command, hdrshot::ExportSnapshot snapshot,
      QtOverlayHost::ExportCompleted completed, QWidget *source) {
    if (shutting_down_) {
      completed(hdrshot::ExportWorkflow::cancel(snapshot));
      return false;
    }
    auto anchor = hdrshot::SaveSuccessFeedback::capture(source);
    return completion_->submit(command, std::move(snapshot),
        [this, anchor = std::move(anchor), completed = std::move(completed)](QtOverlayHost::ExportResult result) mutable {
          report_background_export_result(result);
          save_feedback_.completed(result, std::move(anchor));
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
    if (shutting_down_ || close_confirmation_active_) return;
    if(updater_&&updater_->barrier_active())return;
    const auto requested_session = capture_lifecycle_.active() ? capture_lifecycle_.id()
        : hdrshot::SessionId{next_session_id_};
    const bool draining = capture_ && capture_->busy() && !capture_lifecycle_.active();
    const bool busy = capture_lifecycle_.active() || draining;
    hdrshot::record_diagnostic_stage(diagnostics_.get(), requested_session, hdrshot::OperationId{1},
        "trigger", "received", busy ? "ignored" : "accepted",
        {{"source", source}, {"reason", draining ? "capture_draining" :
          capture_lifecycle_.phase() == hdrshot::UiPhase::capturing ? "preparing" : "user_trigger"}});
    if (busy) {
      tray_->showMessage(
          QStringLiteral("SeriousShot"),
          hdrshot::capture_busy_message(capture_lifecycle_.phase(), !capture_lifecycle_.active()),
          QSystemTrayIcon::Information,
          2000);
      return;
    }
    const auto screens = NSScreen.screens;
    if (screens.count == 0) {
      const hdrshot::Error error{hdrshot::ErrorCode::capture_failed, "MacApplicationShell",
          hdrshot::Retryability::after_user_action, {{"reason", "no_available_screen"}}};
      record_capture_event(requested_session, hdrshot::OperationId{1}, "screens", "failure", &error);
      return;
    }
    const auto session_id = hdrshot::SessionId{next_session_id_++};
    if (!capture_lifecycle_.begin(session_id)) return;
    save_feedback_.set_capture_active(true);
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
      const auto prepared = host->prepare_hidden_native_surface();
      if (!prepared) {
        record_capture_event(session_id, operation_id, "surface_prepare", "failure", &prepared.error());
        host->deleteLater(); finish_capture(); return;
      }
      host->set_diagnostics(diagnostics_);
      host->set_text_rasterizer(text_rasterizer_, text_editor_font_family_);
      host->set_background_clean_composition(export_executor_, export_pixel_processor_);
      host->set_annotation_preview_optimization(true);
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
              QtOverlayHost::ExportCompleted completed, QWidget *source) -> bool {
            return request_export(command, std::move(snapshot), std::move(completed), source);
          },
          settings_);
      host->set_pin_requested([this](auto snapshot, auto completed) {
        pinned_->open(std::move(snapshot), std::move(completed));
      });
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
      auto card_created = hdrshot::MacMetalEdrPresenter::create();
      if (!card_created) {
        record_capture_event(session_id, operation_id, "card_create", "failure", &card_created.error());
        host->deleteLater(); finish_capture(); return;
      }
      auto card_presenter = std::shared_ptr<hdrshot::MacMetalEdrPresenter>(std::move(card_created.value()));
      auto card_configured = card_presenter->configure_target_layer(host->native_magnifier_surface(), surface_range);
      if (!card_configured) {
        record_capture_event(session_id, operation_id, "card_configure", "failure", &card_configured.error());
        host->deleteLater(); finish_capture(); return;
      }
      host->set_magnifier_card(std::make_shared<hdrshot::MacMagnifierCardPresenter>(
          std::move(card_presenter),host->native_magnifier_surface(),surface_range));
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
      finish_capture();
      return;
    }
    multi_preview_ = std::make_shared<hdrshot::MultiDisplayPreviewPresenterPort>(
        std::move(endpoints), capture_runtime_.handle());
    catalog_ = std::make_shared<hdrshot::MacDisplayCatalogPort>(diagnostics_);
    capture_session_ = std::make_shared<hdrshot::CaptureSession>(
        catalog_, capture_, multi_preview_, window_catalog_, diagnostics_, capture_runtime_.handle());
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
      capture_runtime_.handle()->post(
          [this, session_id, operation_id, result = std::move(result)]() mutable {
            if (!capture_lifecycle_.accepts(session_id)) return;
            if (!result) {
              hdrshot::record_diagnostic_stage(diagnostics_.get(), session_id, operation_id,
                  "capture", "prepare_overlay", "failure", {}, &result.error(), true);
              std::cerr << "CaptureSession failed: ";
              print_error(result.error());
              notify_capture_failure(result.error(), session_id, operation_id);
              finish_capture(session_id);
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
              finish_capture(session_id);
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
          });
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

  void finish_capture(hdrshot::SessionId expected = {}, bool shutting_down = false) {
    if (expected.value == 0) expected = capture_lifecycle_.id();
    if (!capture_lifecycle_.finish(expected) && !shutting_down) return;
    // Invalidate before cancellation can queue callbacks. Native resources are
    // then released; stale callbacks cannot close a newer session.
    capture_session_.reset(); // owner-thread detach invalidates/cancels before any surface teardown
    if (shutting_down) {
      capture_runtime_.shutdown();
      if (capture_) capture_->shutdown();
    }
    for (auto& overlay : display_overlays_) {
      if (overlay.host != nullptr) {
        overlay.host->hide();
        overlay.host->deleteLater();
        overlay.host = nullptr;
      }
    }
    multi_preview_.reset();
    display_overlays_.clear();
    catalog_.reset();
    save_feedback_.set_capture_active(false);
    std::cout << "captureSessionReturnedToResident=1\n";
  }

  hdrshot::MacLoginStartupPort login_startup_;
  QApplication& app_;
  QObject dispatch_context_;
  std::optional<hdrshot::QtApplicationRestartRequest>& restart_request_;
  hdrshot::QtUiLanguageRuntime language_runtime_;
  hdrshot::QtCaptureRuntime capture_runtime_;
  QSystemTrayIcon* tray_{};
  QMenu* tray_menu_{};
  QAction* capture_action_{};
  std::unique_ptr<hdrshot::update::Runtime> updater_;
  hdrshot::SettingsWindow* settings_window_{};
  hdrshot::MacQtWindowActivation settings_activation_;
  bool settings_open_queued_{};
  bool shutting_down_{};
  bool close_confirmation_active_{};
  std::unique_ptr<hdrshot::QtSettingsStorePort> settings_store_;
  std::shared_ptr<hdrshot::DiagnosticsPort> diagnostics_;
  std::unique_ptr<hdrshot::MacGlobalHotkeyPort> hotkey_;
  hdrshot::MacQtInputPlatformAdapter input_platform_;
  hdrshot::SettingsSnapshot settings_{};
  hdrshot::QtExportTaskExecutor export_executor_;
  std::unique_ptr<hdrshot::ExportCompletionCoordinator> completion_;
  hdrshot::SaveSuccessFeedback save_feedback_;
  std::unique_ptr<hdrshot::MacQtAnalyzerController> analyzer_;
  std::unique_ptr<hdrshot::QtPinnedScreenshotController> pinned_;
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
  const auto restart_launch = hdrshot::parse_application_restart_arguments(arguments);
  if (!restart_launch) { print_error(restart_launch.error()); return 2; }
  if (restart_launch.value()) arguments.clear();
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
  hdrshot::MacLaunchOriginObserver launch_origin;
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

  std::optional<hdrshot::QtApplicationRestartRequest> restart_request;
  bool startup_ready = false;
  bool already_running = false;
  const auto run_gui = [&] {
    hdrshot::QtGuiInstanceGuard instance_guard(
        QDir(hdrshot::update::testing_namespace(
                 QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation),
                 hdrshot::update::compiled_trust()))
            .filePath(QStringLiteral("gui-instance.lock")));
    if (instance_guard.status() == hdrshot::GuiInstanceStatus::already_running) {
      std::cout << "guiInstanceAlreadyRunning=1\n";
      already_running = true;
      return 0;
    }
    if (instance_guard.status() != hdrshot::GuiInstanceStatus::owned) {
      std::cerr << instance_guard.diagnostic().toStdString() << '\n';
      return 1;
    }
    MacApplicationShell shell(app, restart_request);
    if (!shell.initialize()) return 1;
    startup_ready = true;
    launch_origin.when_ready([&shell](hdrshot::MacLaunchOrigin origin) { shell.launch_ready(origin); });
    const int result = app.exec();
    launch_origin.when_ready({});
    return result;
  };
  const int result = run_gui();
  // The shell and its queued-callback context are gone, and the lock is released.
  hdrshot::MacQtWindowActivation activation;
  if (restart_request) return hdrshot::relaunch_application(*restart_request, activation);
  if (restart_launch.value() && !startup_ready) {
    hdrshot::show_application_restart_failure(*restart_launch.value(), already_running, activation);
    return 1;
  }
  return result;
}
