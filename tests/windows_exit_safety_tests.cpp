#include "application/export_completion_coordinator.hpp"
#include "platform/cpu/cpu_analysis_port.hpp"
#include "platform/qt/qt_export_task_executor.hpp"
#include "platform/windows/windows_exit_coordinator.hpp"
#include "platform/windows/windows_qt_ports.hpp"
#include "ui/qt/analyzer_window.hpp"
#include "ui/qt/pinned_screenshot_window.hpp"
#include "test_support.hpp"
#include <QApplication>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QPointer>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTimer>
#include <chrono>
#include <thread>

using namespace hdrshot;
namespace {
void nested_events() {
  QEventLoop loop;
  QTimer::singleShot(20,&loop,&QEventLoop::quit);
  loop.exec();
}
QFileDialog *save_dialog() {
  for(auto *widget:QApplication::topLevelWidgets())
    if(auto *dialog=qobject_cast<QFileDialog *>(widget);
       dialog && dialog->objectName()==QStringLiteral("windowsSaveDialog")) return dialog;
  return nullptr;
}
analysis::Input input() {
  auto source=make_cpu_analysis_source({4,4},analysis::FloatImage(16,{.2f,.3f,.4f,1.f}));
  HDRSHOT_CHECK(source.has_value());
  return {source.value(),1,false};
}
ExportSnapshot snapshot(const QString &folder) {
  auto frame=std::make_shared<FrozenDesktop>(FrozenDesktop{
      FrameId{1},1,LogicalRect{0,0,4,4},{CanonicalFrameSegment{
        DisplayId{1},LogicalRect{0,0,4,4},1,PixelSize{4,4},PixelFormat::rgba16_float,
        {ColorPrimaries::display_p3,TransferFunction::extended_srgb,AlphaMode::straight,0},
        DisplayDynamicRange::sdr,std::vector<std::uint16_t>(64,0x3800)}}});
  return {SessionId{1},OperationId{2},frame,SelectionSnapshot{1,PixelRect{0,0,4,4}},
      AnnotationDocument::empty().snapshot(),nullptr,folder.toStdString(),DisplayId{1}};
}
struct Clipboard final : ClipboardPort {
  Result<ClipboardReceipt,Error> write(const ClipboardWriteRequest &) override {
    return Result<ClipboardReceipt,Error>::failure({ErrorCode::clipboard_rejected,
        "ExitSafetyTest",Retryability::never,{}});
  }
};
struct Files final : FileStorePort {
  WindowsFileStorePort native;
  bool fallback{};
  int opens{};
  Result<bool,Error> prepare_directory(const std::string &folder) override {
    if(fallback) return Result<bool,Error>::failure({ErrorCode::path_not_writable,
        "ExitSafetyTest",Retryability::after_user_action,{}});
    return native.prepare_directory(folder);
  }
  Result<FileReceipt,Error> write(const WriteFileRequest &r) override { return native.write(r); }
  Result<std::unique_ptr<AtomicFileSink>,Error> open_atomic(const OpenAtomicFileRequest &r) override {
    ++opens;return native.open_atomic(r);
  }
};
// Exercise the production Windows dialog, coordinator and executor. The outer
// event callback deliberately starts another nested loop after submit returns:
// one singleShot(0), or checking the dialog alone, would destroy the owner here.
void exit_while_saving(bool fallback) {
  QEventLoop outer;
  QTemporaryDir folder;
  auto *owner=new PinnedScreenshotWindow(input(),{});
  owner->show();QApplication::setActiveWindow(owner);
  QPointer<PinnedScreenshotWindow> owner_guard(owner);
  int finalized=0,completed=0;
  bool outer_returned=false;
  WindowsExitCoordinator exit([&]{return true;},[&](int code) {
    HDRSHOT_CHECK(code==7 && outer_returned && completed==1);
    HDRSHOT_CHECK(owner_guard && !save_dialog());
    ++finalized;delete owner;outer.quit();
  });
  Files files;files.fallback=fallback;
  WindowsFileDialogPort dialog(&exit);
  WindowsClockPort clock;
  Clipboard clipboard;
  QtExportTaskExecutor executor(outer);
  ExportCompletionCoordinator completion({files,dialog,clipboard,clock,executor});
  QTimer watchdog;
  watchdog.setSingleShot(true);
  QObject::connect(&watchdog,&QTimer::timeout,&outer,[&] {
    if(auto *active=save_dialog()) active->reject();outer.quit();
  });
  watchdog.start(3000);
  QTimer::singleShot(0,&outer,[&] {
    {
      auto call=exit.enter_call();
      QTimer::singleShot(0,&outer,[&] {
        auto *active=save_dialog();HDRSHOT_CHECK(active && !active->parent());
        exit.request_exit(7);exit.request_exit(9);
        HDRSHOT_CHECK(!exit.accepting_work() && finalized==0 && owner_guard);
        HDRSHOT_CHECK(!exit.shutdown_synchronously());
        const auto blocked=dialog.choose_save_path({"blocked.png",folder.path().toStdString()});
        HDRSHOT_CHECK(blocked && std::holds_alternative<UserCancelled>(blocked.value()));
        HDRSHOT_CHECK(save_dialog()==active);
      });
      HDRSHOT_CHECK(!completion.submit(fallback?UiCommand::save_default:UiCommand::save_as,
          snapshot(folder.path()),[&](auto result) {
            HDRSHOT_CHECK(result && std::holds_alternative<UserCancelled>(result.value().destination));
            ++completed;nested_events();HDRSHOT_CHECK(finalized==0 && owner_guard);
          }));
      HDRSHOT_CHECK(finalized==0 && owner_guard);
    }
    nested_events();HDRSHOT_CHECK(finalized==0 && owner_guard);
    outer_returned=true;
  });
  outer.exec();
  HDRSHOT_CHECK(finalized==1 && completed==1 && !owner_guard && files.opens==0);
  HDRSHOT_CHECK(exit.finished() && !exit.accepting_work());
  exit.request_exit();HDRSHOT_CHECK(finalized==1);
}
void owner_destruction_cancels_stack_dialog(bool analyzer,bool normal_close) {
  QEventLoop outer;
  QTemporaryDir folder;
  QWidget *owner=analyzer?static_cast<QWidget *>(new AnalyzerWindow(input())):
      static_cast<QWidget *>(new PinnedScreenshotWindow(input(),{}));
  if(analyzer) static_cast<AnalyzerWindow *>(owner)->set_close_confirmation([]{return true;});
  if(normal_close) owner->setAttribute(Qt::WA_DeleteOnClose);
  owner->show();QApplication::setActiveWindow(owner);
  QPointer<QWidget> owner_guard(owner);
  WindowsFileDialogPort dialog;
  bool returned=false;
  QTimer::singleShot(0,&outer,[&] {
    QTimer::singleShot(0,&outer,[&] {
      QPointer<QFileDialog> active(save_dialog());
      HDRSHOT_CHECK(active && !active->parent());
      if(normal_close) {
        HDRSHOT_CHECK(owner->close());
        QCoreApplication::sendPostedEvents(owner,QEvent::DeferredDelete);
      } else delete owner;
      // The owner does not own this stack object; it only rejects exec().
      HDRSHOT_CHECK(!owner_guard && active);
    });
    const auto result=dialog.choose_save_path({"test.png",folder.path().toStdString()});
    HDRSHOT_CHECK(result && std::holds_alternative<UserCancelled>(result.value()));
    returned=true;outer.quit();
  });
  outer.exec();HDRSHOT_CHECK(returned && !save_dialog());
}
void analyzer_veto_restores_work_and_coalesces_confirmation() {
  QEventLoop outer;
  QTemporaryDir folder;
  auto *owner=new AnalyzerWindow(input());
  bool allow=false;
  int confirmations=0,finalized=0;
  WindowsExitCoordinator exit([&]{return owner->close();},[&](int) {
    ++finalized;delete owner;outer.quit();
  });
  WindowsFileDialogPort dialog(&exit);
  owner->set_close_confirmation([&] {
    ++confirmations;exit.request_exit();
    nested_events();HDRSHOT_CHECK(finalized==0 && !exit.accepting_work());
    return allow;
  });
  owner->show();
  QTimer::singleShot(0,&outer,[&]{exit.request_exit();});
  QTimer retry;
  QObject::connect(&retry,&QTimer::timeout,&outer,[&] {
    if(confirmations==1 && exit.accepting_work()) {
      HDRSHOT_CHECK(!exit.finished() && finalized==0);
      retry.stop();
      {
        auto call=exit.enter_call();
        QTimer::singleShot(0,&outer,[] {
          auto *active=save_dialog();HDRSHOT_CHECK(active);active->reject();
        });
        const auto result=dialog.choose_save_path({"after-veto.png",folder.path().toStdString()});
        HDRSHOT_CHECK(result && std::holds_alternative<UserCancelled>(result.value()));
      }
      allow=true;exit.request_exit();
    }
  });
  retry.start(10);
  QTimer::singleShot(3000,&outer,&QEventLoop::quit);
  outer.exec();HDRSHOT_CHECK(confirmations==2 && finalized==1);
}
void accepted_export_drains_before_destruction(bool close_owner_first) {
  QEventLoop outer;
  QTemporaryDir folder;
  auto *owner=new PinnedScreenshotWindow(input(),{});
  QPointer<PinnedScreenshotWindow> owner_guard(owner);
  QtExportTaskExecutor executor(outer);
  Files files;WindowsFileDialogPort dialog;WindowsClockPort clock;Clipboard clipboard;
  ExportCompletionCoordinator completion({files,dialog,clipboard,clock,executor});
  int completed=0,finalized=0,widget_updates=0;
  WindowsExitCoordinator exit([&]{return true;},[&](int) {
    executor.drain();
    HDRSHOT_CHECK(completed==1);
    if(owner_guard) delete owner_guard.data();
    ++finalized;outer.quit();
  });
  QSemaphore release,requested;
  executor.background([&]{release.acquire();});
  std::thread worker([&] {
    requested.acquire();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));release.release();
  });
  QTimer::singleShot(0,&outer,[&] {
    auto call=exit.enter_call();
    HDRSHOT_CHECK(completion.submit(UiCommand::save_default,snapshot(folder.path()),[&](auto result) {
      HDRSHOT_CHECK(result.has_value());
      const auto *file=std::get_if<FileReceipt>(&result.value().destination);
      HDRSHOT_CHECK(file && QFileInfo::exists(QString::fromStdString(file->exact_path)));
      ++completed;
      if(owner_guard) {owner_guard->set_busy(false);++widget_updates;}
    }));
    HDRSHOT_CHECK(completed==0 && executor.pending());
    if(close_owner_first) delete owner;
    exit.request_exit();HDRSHOT_CHECK(finalized==0);
    requested.release();
  });
  outer.exec();worker.join();
  HDRSHOT_CHECK(finalized==1 && completed==1 && !owner_guard);
  HDRSHOT_CHECK(widget_updates==(close_owner_first?0:1));
}
void synchronous_shutdown_contract_and_destroyed_continuation() {
  int finalized=0;
  {
    WindowsExitCoordinator exit([]{return true;},[&](int){++finalized;});
    {auto call=exit.enter_call();
    HDRSHOT_CHECK(!exit.shutdown_synchronously() && exit.accepting_work());
    exit.request_exit();}
    HDRSHOT_CHECK(!exit.shutdown_synchronously());
  }
  // A queued continuation cannot retain or call a destroyed shell coordinator.
  nested_events();HDRSHOT_CHECK(finalized==0);
  WindowsExitCoordinator exit([]{return true;},[&](int){++finalized;});
  HDRSHOT_CHECK(exit.shutdown_synchronously() && exit.finished() && finalized==1);
  HDRSHOT_CHECK(exit.shutdown_synchronously() && finalized==1);
}
void ordinary_cancel_keeps_work_available() {
  QEventLoop outer;
  QTemporaryDir folder;
  WindowsExitCoordinator exit([]{return true;},[](int){HDRSHOT_CHECK(false);});
  WindowsFileDialogPort dialog(&exit);
  int cancelled=0;
  QTimer::singleShot(0,&outer,[&] {
    auto call=exit.enter_call();
    for(int attempt=0;attempt!=2;++attempt) {
      QTimer::singleShot(0,&outer,[] {
        auto *active=save_dialog();HDRSHOT_CHECK(active);active->reject();
      });
      const auto result=dialog.choose_save_path({"test.png",folder.path().toStdString()});
      HDRSHOT_CHECK(result && std::holds_alternative<UserCancelled>(result.value()));
      HDRSHOT_CHECK(exit.accepting_work());++cancelled;
    }
    outer.quit();
  });
  outer.exec();HDRSHOT_CHECK(cancelled==2 && exit.accepting_work());
}
void existing_modal_confirmation_must_return_before_exit() {
  QEventLoop outer;
  auto *owner=new QWidget;
  QPointer<QWidget> owner_guard(owner);
  bool original_returned=false;
  int finalized=0;
  WindowsExitCoordinator exit([]{return true;},[&](int) {
    HDRSHOT_CHECK(original_returned && owner_guard);
    ++finalized;delete owner;outer.quit();
  });
  QTimer::singleShot(0,&outer,[&] {
    QDialog confirmation(owner);
    QTimer::singleShot(0,&outer,[&] {
      HDRSHOT_CHECK(!exit.shutdown_synchronously());
      exit.request_exit();nested_events();
      HDRSHOT_CHECK(finalized==0 && owner_guard);
      confirmation.reject();
    });
    confirmation.exec();
    nested_events();HDRSHOT_CHECK(finalized==0 && owner_guard);
    original_returned=true;
  });
  QTimer::singleShot(3000,&outer,&QEventLoop::quit);
  outer.exec();HDRSHOT_CHECK(finalized==1 && !owner_guard);
}
} // namespace
int main(int argc,char **argv) {
  // Keep this mandatory regression independent of an unlocked desktop/native
  // file picker; QFileDialog::exec and actual widgets still run their Qt loops.
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
  return hdrshot::test::run({
    {"save as repeated exit waits for submit and outer dispatch",[]{exit_while_saving(false);}},
    {"default save fallback repeated exit waits for submit",[]{exit_while_saving(true);}},
    {"pinned destructor cannot destroy stack save dialog",[]{owner_destruction_cancels_stack_dialog(false,false);}},
    {"analyzer destructor cannot destroy stack save dialog",[]{owner_destruction_cancels_stack_dialog(true,false);}},
    {"pinned normal close rejects stack save dialog",[]{owner_destruction_cancels_stack_dialog(false,true);}},
    {"analyzer normal close rejects stack save dialog",[]{owner_destruction_cancels_stack_dialog(true,true);}},
    {"ordinary cancel leaves later saves available",ordinary_cancel_keeps_work_available},
    {"existing modal close confirmation returns before exit",existing_modal_confirmation_must_return_before_exit},
    {"analyzer veto restores work and repeat exit is coalesced",analyzer_veto_restores_work_and_coalesces_confirmation},
    {"accepted export and callback drain before destruction",[]{accepted_export_drains_before_destruction(false);}},
    {"export callback arriving after owner close is harmless",[]{accepted_export_drains_before_destruction(true);}},
    {"updater sync contract and destroyed continuation",synchronous_shutdown_contract_and_destroyed_continuation}});
}
