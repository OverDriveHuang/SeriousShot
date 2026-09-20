#include "application/capture_interaction_session.hpp"
#include "application/capture_session.hpp"
#include "application/export_completion_coordinator.hpp"
#include "application/multi_display_preview_presenter.hpp"
#include "adapters/shared/libultrahdr_encoder.hpp"
#include "platform/windows/windows_capture.hpp"
#include "platform/windows/windows_d3d_export.hpp"
#include "platform/windows/windows_d3d_presenter.hpp"
#include "platform/windows/windows_qt_ports.hpp"
#include "platform/windows/windows_application_icon.hpp"
#include "platform/freetype/freetype_text_rasterizer_port.hpp"
#include "platform/qt/qt_export_task_executor.hpp"
#include "platform/qt/qt_folder_opener_port.hpp"
#include "platform/qt/qt_session_diagnostics_port.hpp"
#include "core/build_metadata.hpp"
#include "platform/qt/qt_settings_store_port.hpp"
#include "ui/qt/qt_overlay_host.hpp"
#include "ui/qt/settings_window.hpp"
#include <windows.h>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QIcon>
#include <QMenu>
#include <QMouseEvent>
#include <QScreen>
#include <QStandardPaths>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QWindow>
#include <atomic>
#include <iostream>

namespace {
using namespace hdrshot;
constexpr const char* default_hotkey="Control+Shift+2";
void print_error(const Error& error) {
  std::cerr<<error.module<<' '<<to_string(error.code);
  for(const auto& [k,v]:error.safe_context) std::cerr<<' '<<k<<'='<<v;
  std::cerr<<'\n';
}
struct Overlay {
  DisplayId display;
  QPointer<QtOverlayHost> host;
  std::shared_ptr<WindowsD3DPreviewPresenter> presenter;
};
// Composition root: all selection, annotation, settings, export and completion
// policies are the existing shared objects. Only native effects are wired here.
class WindowsApplicationShell {
 public:
  WindowsApplicationShell(QApplication& app,QString smoke_folder,bool smoke_copy=false,bool smoke_sequence=false)
      :app_(app),smoke_folder_(std::move(smoke_folder)),smoke_copy_(smoke_copy),
       smoke_sequence_(smoke_sequence),
       clipboard_(smoke_folder_.isEmpty()?std::string{}:(smoke_folder_+QStringLiteral("/clipboard")).toStdString()),executor_(app){}
  ~WindowsApplicationShell() {
    finish_capture();
    executor_.drain();
    delete settings_window_;
    delete tray_menu_;
  }
  bool initialize() {
    const QString data_root=smoke_folder_.isEmpty() ?
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation):smoke_folder_;
    const QString pictures=smoke_folder_.isEmpty() ?
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)+QStringLiteral("/SeriousShot"):smoke_folder_;
    const QString config=smoke_folder_.isEmpty() ?
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)+QStringLiteral("/settings.ini"):
        smoke_folder_+QStringLiteral("/settings.ini");
    diagnostics_folder_=(data_root+QStringLiteral("/logs")).toStdString();
    auto session_log=std::make_unique<QtSessionDiagnosticsPort>(diagnostics_folder_);
    const auto log_started=session_log->start(std::string(build_timestamp()),"Windows");
    if(!log_started) print_error(log_started.error());
    diagnostics_=std::move(session_log);
    store_=std::make_unique<QtSettingsStorePort>(config.toStdString(),default_hotkey,pictures.toStdString());
    const auto loaded=store_->load();
    if(!loaded) { record_diagnostic_failure(diagnostics_.get(),"startup",loaded.error());print_error(loaded.error());return false; }
    settings_=loaded.value();
    diagnostics_->set_detailed_logging(settings_.detailed_logging);
    const auto font=QCoreApplication::applicationDirPath()+QStringLiteral("/assets/fonts/NotoSansSC-VF.ttf");
    rasterizer_=std::make_shared<FreeTypeTextRasterizerPort>(font.toStdString(),
        "NotoSansSC-VF.ttf@f8d1575;wght=450;hinting=native#d68bafcb48a2707749396aa12bbbd833cb70401f3a9a689fd2902c7e0d295964");
    const int font_id=QFontDatabase::addApplicationFont(font);
    const auto families=QFontDatabase::applicationFontFamilies(font_id);
    if(font_id<0 || families.isEmpty()) {
      record_diagnostic_failure(diagnostics_.get(),"startup",{ErrorCode::invalid_input,"font",Retryability::never,{}});
      std::cerr<<"font unavailable\n";return false;
    }
    font_family_=families.front().toStdString();
    auto gpu=WindowsD3DExportPixelProcessor::create();
    if(!gpu) {record_diagnostic_failure(diagnostics_.get(),"startup",gpu.error());print_error(gpu.error());return false;}
    pixels_=std::move(gpu.value());
    completion_=std::make_unique<ExportCompletionCoordinator>(
        ExportCompletionPorts{files_,dialog_,clipboard_,clock_,executor_,rasterizer_.get(),pixels_.get(),pixels_.get(),&encoder_,&range_},
        [this](bool active,DisplayId owner) {
          for(auto& overlay:overlays_) if(overlay.host) overlay.host->set_system_dialog_active(active,!active && overlay.display==owner);
        },ExportCompletionCoordinator::Event{},diagnostics_.get());
    tray_=new QSystemTrayIcon(app_.windowIcon(),&app_);
    tray_->setToolTip(QStringLiteral("SeriousShot"));
    tray_menu_=new QMenu;
    capture_action_=tray_menu_->addAction(QString());
    update_capture_action_text();
    QObject::connect(capture_action_,&QAction::triggered,&app_,[this] {start_capture();});
    auto* settings_action=tray_menu_->addAction(QStringLiteral("设置…"));
    QObject::connect(settings_action,&QAction::triggered,&app_,[this] {show_settings();});
    tray_menu_->addSeparator();
    auto* quit=tray_menu_->addAction(QStringLiteral("退出 SeriousShot"));
    QObject::connect(quit,&QAction::triggered,&app_,[this] {shutdown();});
    tray_->setContextMenu(tray_menu_);
    QObject::connect(tray_,&QSystemTrayIcon::activated,&app_,[this](auto reason) {
      if(reason==QSystemTrayIcon::DoubleClick) show_settings();
    });
    tray_->show();
    hotkey_.set_trigger_handler([this] {QTimer::singleShot(0,&app_,[this]{start_capture();});});
    const auto registered=hotkey_.register_hotkey(settings_.global_capture_hotkey);
    if(!registered) {record_diagnostic_failure(diagnostics_.get(),"hotkey",registered.error());print_error(registered.error());tray_->showMessage(QStringLiteral("快捷键不可用"),QStringLiteral("请打开设置更换截图快捷键。"));}
    if(!smoke_folder_.isEmpty()) {
      QTimer::singleShot(100,&app_,[this] {start_capture();});
      QTimer::singleShot(smoke_sequence_?45000:15000,&app_,[this] {if(!smoke_exported_) {std::cerr<<"smoke timeout\n";shutdown(1);}});
    } else if(!settings_.initial_settings_presented) {
      QTimer::singleShot(0,&app_,[this] {
        show_settings();
        const auto marked=SettingsWorkflow::mark_initial_settings_presented(*store_);
        if(!marked) print_error(marked.error());
      });
    }
    std::cout<<"appShellReady=1 windowsBuild="<<windows_build_number()<<" hotkey="<<settings_.global_capture_hotkey<<'\n';
    return true;
  }
  void shutdown(int code=0) {
    (void)hotkey_.unregister_hotkey();
    if(tray_) tray_->hide();
    finish_capture();
    executor_.drain();
    app_.exit(code);
  }
 private:
  void update_capture_action_text() {
    if(capture_action_) {
      capture_action_->setText(QStringLiteral("截图（%1）")
          .arg(QString::fromStdString(settings_.global_capture_hotkey)));
    }
  }
  void show_settings() {
    if(settings_open_queued_) return;
    settings_open_queued_=true;
    // All entry points, including tray double-click, must finish menu/input
    // dispatch before restoring the window. Match the Mac shell's ordering.
    if(tray_menu_) tray_menu_->hide();
    QTimer::singleShot(0,&app_,[this] {
      settings_open_queued_=false;
      show_settings_after_menu();
    });
  }
  void show_settings_after_menu() {
    if(!settings_window_) {
      settings_window_=new SettingsWindow(*store_,hotkey_,folders_,input_,default_hotkey,diagnostics_folder_);
      settings_window_->set_window_activation_port(&activation_);
      settings_window_->set_applied([this](const SettingsSnapshot& settings){
        settings_=settings;
        diagnostics_->set_detailed_logging(settings_.detailed_logging);
        update_capture_action_text();
      });
    }
    settings_window_->reload_and_show();
  }
  void report(const Error& error, bool log = true) {
    print_error(error);
    if(log) record_diagnostic_failure(diagnostics_.get(),"capture",error);
    tray_->showMessage(QStringLiteral("SeriousShot 操作失败"),QString::fromStdString(to_string(error.code)),QSystemTrayIcon::Critical);
    if(!smoke_folder_.isEmpty()) QTimer::singleShot(0,&app_,[this]{shutdown(1);});
  }
  void start_capture() {
    if(lifecycle_.active()) return;
    if(settings_window_) {
      settings_window_->hide();
    }
    const auto native=windows_enumerate_displays();
    if(!native) {report(native.error());return;}
    auto displays=std::make_shared<const std::vector<WindowsDisplayInfo>>(native.value());
    const SessionId session{next_session_++};
    if(!lifecycle_.begin(session)) return;
    const SelectionSnapshot initial{1,{}};
    std::vector<DisplayId> targets;
    std::vector<DisplayPreviewEndpoint> endpoints;
    auto input_recovery=std::make_shared<OverlayInputRecovery>();
      for(const auto& display:*displays) {
        QScreen* qt_screen=nullptr;
        for(auto* screen:QGuiApplication::screens()) {
          const auto* native_screen=screen->nativeInterface<QNativeInterface::QWindowsScreen>();
          if(native_screen && reinterpret_cast<std::uintptr_t>(native_screen->handle())==display.monitor) {qt_screen=screen;break;}
        }
      if(!qt_screen) {report({ErrorCode::display_configuration_changed,"WindowsShell",Retryability::after_recreate,{{"reason","qt_screen_not_found"}}});finish_capture();return;}
      auto* host=new QtOverlayHost(std::make_unique<WindowsQtOverlayWindow>(display),display.snapshot.id,initial,input_,input_recovery);
      host->setGeometry(qt_screen->geometry());
      host->prepare_hidden_native_surface();
      host->windowHandle()->setScreen(qt_screen);
      host->set_text_rasterizer(rasterizer_,font_family_);
      host->set_finished([this,session] {finish_capture(session);});
      host->set_interaction_allowed([this,session](DisplayId id){return lifecycle_.allows(session,id);});
      host->set_selection_established([this,session](DisplayId id) {
        if(!lifecycle_.lock(session,id)) return;
        for(auto& overlay:overlays_) if(overlay.host && overlay.display!=id) overlay.host->set_interaction_locked(true);
      });
      host->set_export_requested([this](UiCommand command,ExportSnapshot snapshot,QtOverlayHost::ExportCompleted completed) {
        return completion_->submit(command,std::move(snapshot),[this,completed=std::move(completed)](auto result) mutable {
          if(!result) report(result.error(),false); // Shared completion already logged it.
          else if(const auto* file=std::get_if<FileReceipt>(&result.value().destination)) {
            std::cout<<"exported="<<file->exact_path<<" bytes="<<file->bytes_written<<'\n';
            if(!smoke_folder_.isEmpty()) smoke_completed();
          } else if((smoke_copy_ || smoke_sequence_) && std::holds_alternative<ClipboardReceipt>(result.value().destination)) {
            std::cout<<"clipboardExported=1\n";
            smoke_completed();
          }
          completed(std::move(result));
        });
      },settings_);
      auto presenter=WindowsD3DPreviewPresenter::create(host->native_surface(),display);
      if(!presenter) {host->deleteLater();report(presenter.error());finish_capture();return;}
      targets.push_back(display.snapshot.id);
      endpoints.push_back({display.snapshot.id,presenter.value()});
      overlays_.push_back({display.snapshot.id,host,presenter.value()});
    }
    catalog_=std::make_shared<WindowsDisplayCatalogPort>(displays);
    capture_=std::make_shared<WindowsCapturePort>(displays);
    preview_=std::make_shared<MultiDisplayPreviewPresenterPort>(std::move(endpoints));
    session_=std::make_shared<CaptureSession>(catalog_,capture_,preview_);
    session_->begin({session,OperationId{1},FrameId{session.value},std::move(targets),initial},
        [this,session](Result<CaptureReadyPayload,Error> result) mutable {
      QMetaObject::invokeMethod(&app_,[this,session,result=std::move(result)]() mutable {
        if(!lifecycle_.accepts(session)) return;
        if(!result) {report(result.error());finish_capture(session);return;}
        if(!lifecycle_.ready(result.value().present_receipt)) {finish_capture(session);return;}
        auto sequence=std::make_shared<std::atomic_uint64_t>(result.value().present_receipt.operation_id.value+1U);
        for(auto& overlay:overlays_) if(overlay.host) overlay.host->activate_capture(result.value(),overlay.presenter,sequence);
        std::cout<<"captureReady=1 displays="<<overlays_.size()<<'\n';
        if(!smoke_folder_.isEmpty()) QTimer::singleShot(150,&app_,[this]{smoke_select_and_save();});
      },Qt::QueuedConnection);
    });
  }
  // Explicit diagnostic mode only. Exercise PNG save -> JPEG copy -> PNG copy
  // through the same settings workflow and fresh capture snapshots, in an
  // isolated directory. Normal startup and production logging are unchanged.
  void smoke_completed() {
    if(smoke_sequence_ && ++smoke_step_<3) {
      QTimer::singleShot(150,&app_,[this] {
        const auto format=smoke_step_==1?SaveFormat::ultra_hdr_jpeg:SaveFormat::png_display_p3_dual_range;
        const auto changed=SettingsWorkflow::change_save_format(format,*store_);
        if(!changed) {report(changed.error());return;}
        const auto loaded=store_->load();
        if(!loaded) {report(loaded.error());return;}
        settings_=loaded.value();
        start_capture();
      });
    } else {
      smoke_exported_=true;QTimer::singleShot(50,&app_,[this]{shutdown();});
    }
  }
  void smoke_select_and_save() {
    if(overlays_.empty() || !overlays_.front().host) {shutdown(1);return;}
    auto* host=overlays_.front().host.data();
    for(auto* child:host->children()) if(auto* editor=dynamic_cast<OverlayEditorWidget*>(child)) {
      const auto surface=static_cast<HWND>(host->native_surface());
      const auto scale=host->devicePixelRatioF();
      const auto point=[scale](int x,int y) {return MAKELPARAM(static_cast<int>(x*scale),static_cast<int>(y*scale));};
      SendMessageW(surface,WM_LBUTTONDOWN,MK_LBUTTON,point(80,80));
      SendMessageW(surface,WM_MOUSEMOVE,MK_LBUTTON,point(400,250));
      SendMessageW(surface,WM_LBUTTONUP,0,point(400,250));
      const bool copy=smoke_copy_ || (smoke_sequence_ && smoke_step_>0);
      QKeyEvent save(QEvent::KeyPress,copy?Qt::Key_Return:Qt::Key_S,
          copy?Qt::NoModifier:Qt::ControlModifier);
      QApplication::sendEvent(editor,&save);return;
    }
    shutdown(1);
  }
  void finish_capture(SessionId expected={}) {
    if(expected.value==0) expected=lifecycle_.id();
    if(!lifecycle_.finish(expected)) return;
    if(session_) session_->cancel(expected,OperationId{1});
    for(auto& overlay:overlays_) if(overlay.host) {overlay.host->hide();overlay.host->deleteLater();}
    session_.reset();preview_.reset();overlays_.clear();capture_.reset();catalog_.reset();
    std::cout<<"captureSessionReturnedToResident=1\n";
  }
  QApplication& app_;
  QString smoke_folder_;
  bool smoke_copy_{};
  bool smoke_sequence_{};
  unsigned smoke_step_{};
  bool smoke_exported_{};
  QSystemTrayIcon* tray_{};
  QMenu* tray_menu_{};
  QAction* capture_action_{};
  SettingsWindow* settings_window_{};
  bool settings_open_queued_{};
  WindowsQtWindowActivation activation_;
  WindowsQtInputPlatformAdapter input_;
  WindowsGlobalHotkeyPort hotkey_;
  WindowsFileStorePort files_;
  WindowsClipboardPort clipboard_;
  WindowsFileDialogPort dialog_;
  WindowsClockPort clock_;
  QtFolderOpenerPort folders_;
  std::unique_ptr<QtSettingsStorePort> store_;
  SettingsSnapshot settings_;
  std::unique_ptr<DiagnosticsPort> diagnostics_;
  std::string diagnostics_folder_;
  std::shared_ptr<FreeTypeTextRasterizerPort> rasterizer_;
  std::string font_family_;
  WindowsLinearP3RangeProbe range_;
  std::unique_ptr<WindowsD3DExportPixelProcessor> pixels_;
  LibUltraHdrEncoder encoder_;
  QtExportTaskExecutor executor_;
  std::unique_ptr<ExportCompletionCoordinator> completion_;
  CaptureInteractionSession lifecycle_;
  std::uint64_t next_session_{1};
  std::vector<Overlay> overlays_;
  std::shared_ptr<WindowsDisplayCatalogPort> catalog_;
  std::shared_ptr<WindowsCapturePort> capture_;
  std::shared_ptr<MultiDisplayPreviewPresenterPort> preview_;
  std::shared_ptr<CaptureSession> session_;
};
}
int main(int argc,char** argv) {
  std::cout<<std::unitbuf;std::cerr<<std::unitbuf;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  QApplication app(argc,argv);
  app.setOrganizationName(QStringLiteral("Overdrive"));
  app.setApplicationName(QStringLiteral("SeriousShot"));
  app.setApplicationDisplayName(QStringLiteral("SeriousShot"));
  app.setWindowIcon(windows_application_icon());
  app.setQuitOnLastWindowClosed(false);
  const bool smoke_copy=argc==3 && std::string_view(argv[1])=="--smoke-copy";
  const bool smoke_sequence=argc==3 && std::string_view(argv[1])=="--smoke-sequence";
  const bool smoke=smoke_copy || smoke_sequence || (argc==3 && std::string_view(argv[1])=="--smoke");
  const auto smoke_folder=smoke?QFileInfo(QString::fromUtf8(argv[2])).absoluteFilePath():QString{};
  if(smoke && (!QDir().mkpath(smoke_folder) || QFileInfo::exists(smoke_folder+QStringLiteral("/settings.ini")))) return 2;
  const HANDLE instance=smoke?nullptr:CreateMutexW(nullptr,FALSE,L"Local\\Overdrive.SeriousShot.Desktop");
  if(!smoke && (!instance || GetLastError()==ERROR_ALREADY_EXISTS)) {if(instance) CloseHandle(instance);return 0;}
  int result=1;
  { WindowsApplicationShell shell(app,smoke_folder,smoke_copy,smoke_sequence);
    if(shell.initialize()) result=app.exec(); }
  if(instance) CloseHandle(instance);
  return result;
}
