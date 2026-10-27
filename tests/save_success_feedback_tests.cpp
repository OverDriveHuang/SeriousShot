#include "ui/qt/save_success_feedback.hpp"
#include "ui/qt/status_message_style.hpp"
#include "application/export_completion_coordinator.hpp"
#include "test_support.hpp"
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QMenu>
#include <QTimer>
#include <QFileInfo>
#ifdef __APPLE__
#include "macos_native_ui_test_support.hpp"
#include "platform/macos/macos_qt_save_feedback_window.hpp"
#endif
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace hdrshot;
namespace {
void wait(int ms) {
  QElapsedTimer elapsed; elapsed.start();
  while (elapsed.elapsed() < ms) { qApp->processEvents(); QThread::msleep(5); }
}
QLabel *feedback() {
  for (auto *widget : QApplication::topLevelWidgets())
    if (widget->objectName() == "saveSuccessFeedbackHost") return widget->findChild<QLabel *>("saveSuccessFeedback");
  return nullptr;
}
struct Executor : ExportTaskExecutorPort {
  std::function<void()> work, done;
  void background(std::function<void()> f) override { work = std::move(f); }
  void main_thread(std::function<void()> f) override { done = std::move(f); }
  void finish() { auto task=std::move(work); task(); auto ui=std::move(done); ui(); }
};
struct Files : FileStorePort {
  bool reject{}, committed{};
  struct Sink : AtomicFileSink {
    Files &owner; QSaveFile file; std::string path;
    Sink(Files &o,const std::string &p):owner(o),file(QString::fromStdString(p)),path(p) { HDRSHOT_CHECK(file.open(QIODevice::WriteOnly)); }
    Result<std::size_t,Error> write(std::span<const std::uint8_t> bytes) override {
      if(owner.reject) return Result<std::size_t,Error>::failure({ErrorCode::storage_full,"test",Retryability::same_input,{}});
      const auto written=file.write(reinterpret_cast<const char*>(bytes.data()),qint64(bytes.size()));
      HDRSHOT_CHECK(written==qint64(bytes.size()));
      return Result<std::size_t,Error>::success(bytes.size());
    }
    Result<FileReceipt,Error> commit() override {
      const auto bytes=file.size(); HDRSHOT_CHECK(file.commit()); owner.committed=true;
      return Result<FileReceipt,Error>::success({path,std::size_t(bytes)});
    }
    void abort() noexcept override { file.cancelWriting(); }
  };
  Result<bool,Error> prepare_directory(const std::string &) override { return Result<bool,Error>::success(true); }
  Result<std::unique_ptr<AtomicFileSink>,Error> open_atomic(const OpenAtomicFileRequest &r) override {
    return Result<std::unique_ptr<AtomicFileSink>,Error>::success(std::make_unique<Sink>(*this,r.exact_path));
  }
  Result<FileReceipt,Error> write(const WriteFileRequest &) override { throw std::runtime_error("unexpected legacy write"); }
};
struct Dialog : FileDialogPort {
  QString path; bool cancel{};
  Result<ChooseSavePathOutcome,Error> choose_save_path(const ChooseSavePathRequest &) override {
    if(cancel) return Result<ChooseSavePathOutcome,Error>::success(UserCancelled{});
    return Result<ChooseSavePathOutcome,Error>::success(ChosenPath{path.toStdString()});
  }
};
struct Clipboard : ClipboardPort {
  Result<ClipboardReceipt,Error> write(const ClipboardWriteRequest &r) override {
    return Result<ClipboardReceipt,Error>::success({r.bytes.size(),r.mime_types});
  }
};
struct Clock : ClockPort { LocalDateTime now_local() const override { return {2026,10,25,22,0,0}; } };
ExportSnapshot shot(const QString &directory) {
  auto frame=std::make_shared<FrozenDesktop>(FrozenDesktop{FrameId{1},1,LogicalRect{0,0,4,4},{CanonicalFrameSegment{
    DisplayId{1},LogicalRect{0,0,4,4},1,PixelSize{4,4},PixelFormat::rgba16_float,
    {ColorPrimaries::display_p3,TransferFunction::extended_srgb,AlphaMode::straight,0},DisplayDynamicRange::sdr,
    std::vector<std::uint16_t>(64,0x3800)}}});
  return {SessionId{1},OperationId{1},frame,SelectionSnapshot{1,PixelRect{0,0,4,4}},
    AnnotationDocument::empty().snapshot(),nullptr,directory.toStdString(),DisplayId{1}};
}
struct Fixture {
  Executor executor; Files files; Dialog dialog; Clipboard clipboard; Clock clock;
#ifdef __APPLE__
  SaveSuccessFeedback notice{nullptr, configure_macos_save_feedback_window};
#else
  SaveSuccessFeedback notice;
#endif
  ExportCompletionCoordinator coordinator{{files,dialog,clipboard,clock,executor}};
  bool submit(UiCommand command,const QString &folder,SaveFeedbackAnchor anchor) {
    return coordinator.submit(command,shot(folder),[this,anchor=std::move(anchor)](auto result) {
      if(result && std::holds_alternative<FileReceipt>(result.value().destination)) HDRSHOT_CHECK(files.committed);
      notice.completed(result,anchor);
    });
  }
};
void pipeline_and_lifecycle() {
  QTemporaryDir temp; HDRSHOT_CHECK(temp.isValid());
  Fixture f;
  auto *source=new QWidget; source->resize(450,80); source->show(); wait(30);
  auto anchor=SaveSuccessFeedback::capture(source);
  f.dialog.path=temp.path()+QStringLiteral("/第一张_实际选择_含空格 file.png");
  HDRSHOT_CHECK(f.submit(UiCommand::save_as,temp.path(),anchor));
  HDRSHOT_CHECK(!feedback()); // queued is not saved
  delete source;
  f.executor.finish(); wait(30);
  HDRSHOT_CHECK(!QImage(f.dialog.path).isNull());
  auto *label=feedback(); HDRSHOT_CHECK(label && label->isVisible());
  HDRSHOT_CHECK(label->text().contains(f.dialog.path));
  HDRSHOT_CHECK(label->window()->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
  HDRSHOT_CHECK(label->window()->testAttribute(Qt::WA_ShowWithoutActivating));
  HDRSHOT_CHECK(label->styleSheet()==status_message_style(anchor.palette,false));
  // A second real save is queued, never rewrites the first path mid-read.
  f.dialog.path=temp.path()+QStringLiteral("/第二张.png");
  HDRSHOT_CHECK(f.submit(UiCommand::save_as,temp.path(),anchor)); f.executor.finish(); wait(50);
  HDRSHOT_CHECK(!label->text().contains(QStringLiteral("第二张")));
  wait(1550);
  HDRSHOT_CHECK(label->isVisible() && label->text().contains(QStringLiteral("第二张")));
  wait(800); HDRSHOT_CHECK(label->isVisible()); // no first timer hides the new message
  // Starting another capture hides any current message and defers new completions.
  f.notice.set_capture_active(true); HDRSHOT_CHECK(!label->isVisible());
  f.dialog.path=temp.path()+QStringLiteral("/第三张.png");
  HDRSHOT_CHECK(f.submit(UiCommand::save_as,temp.path(),anchor)); f.executor.finish(); wait(30);
  HDRSHOT_CHECK(!label->isVisible());
  f.notice.set_capture_active(false); wait(30);
  HDRSHOT_CHECK(label->isVisible() && label->text().contains(QStringLiteral("第二张")));
  wait(1550);
  HDRSHOT_CHECK(label->isVisible() && label->text().contains(QStringLiteral("第三张")));
  HDRSHOT_CHECK(!QImage(f.dialog.path).isNull());
  wait(1650); HDRSHOT_CHECK(!label->isVisible());
}
void cancelled_failed_and_clipboard() {
  QTemporaryDir temp; Fixture f; QWidget source;
  auto anchor=SaveSuccessFeedback::capture(&source);
  f.dialog.cancel=true;
  HDRSHOT_CHECK(!f.submit(UiCommand::save_as,temp.path(),anchor)); wait(20); HDRSHOT_CHECK(!feedback());
  f.dialog.cancel=false; f.dialog.path=temp.path()+"/failed.png"; f.files.reject=true;
  HDRSHOT_CHECK(f.submit(UiCommand::save_as,temp.path(),anchor)); f.executor.finish(); wait(20);
  HDRSHOT_CHECK(!feedback() && !QFile::exists(f.dialog.path));
  f.files.reject=false;
  HDRSHOT_CHECK(f.submit(UiCommand::copy_and_close,temp.path(),anchor)); f.executor.finish(); wait(20);
  HDRSHOT_CHECK(!feedback());
  HDRSHOT_CHECK(f.submit(UiCommand::save_as,temp.path(),anchor)); f.executor.finish(); wait(20);
  HDRSHOT_CHECK(feedback() && feedback()->isVisible());
  HDRSHOT_CHECK(!QImage(f.dialog.path).isNull());
}
#ifdef __APPLE__
int native_window_contract() {
  if(QGuiApplication::platformName()!="cocoa") {
    std::cerr << "[INCOMPLETE] Native window contract requires Cocoa\n"; return 77;
  }
  QTemporaryDir temp; HDRSHOT_CHECK(temp.isValid());
  Fixture f; QWidget source;
  const auto anchor=SaveSuccessFeedback::capture(&source);
  f.dialog.path=temp.path()+QStringLiteral("/原生配置合同_真实保存.png");
  HDRSHOT_CHECK(f.submit(UiCommand::save_as,temp.path(),anchor));
  f.executor.finish(); wait(30);
  auto *label=feedback(); HDRSHOT_CHECK(label);
  auto *host=label->window();
  HDRSHOT_CHECK(host && host->objectName()=="saveSuccessFeedbackHost" && host->isWindow() && !host->parentWidget());
  HDRSHOT_CHECK(label->parentWidget()==host && label->text().contains(f.dialog.path));
  HDRSHOT_CHECK(!QImage(f.dialog.path).isNull());
  const auto native=test::native_window_contract(*host);
  std::cout << "native-window-contract platform=cocoa host=" << host->objectName().toStdString()
            << " view=" << native.view << " window=" << native.window
            << " hides-on-deactivate=" << native.hides_on_deactivate
            << " can-become-key=" << native.can_become_key
            << " animation-none=" << native.animation_none << std::endl;
  if(!native.view || !native.window) {
    std::cerr << "[INCOMPLETE] Production host has no valid NSView/NSWindow\n"; return 77;
  }
  HDRSHOT_CHECK(!native.hides_on_deactivate);
  HDRSHOT_CHECK(!native.can_become_key);
  HDRSHOT_CHECK(native.animation_none);
  HDRSHOT_CHECK(host->windowFlags().testFlag(Qt::Tool));
  HDRSHOT_CHECK(host->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
  HDRSHOT_CHECK(host->testAttribute(Qt::WA_ShowWithoutActivating) && host->focusPolicy()==Qt::NoFocus);
  return 0;
}
int native_background_demo(const QString &folder) {
  if(!test::native_desktop_ready()) return 77;
  HDRSHOT_CHECK(QDir::isAbsolutePath(folder) && QDir().mkpath(folder));
  test::NativeBackgroundScope foreground_restore;
  QWidget baseline; baseline.resize(500,250); baseline.show();
  if(!test::native_make_key(baseline)) return 77;
  QWidget source; source.resize(450,80);
  auto *screen=QGuiApplication::primaryScreen();
  if(!screen) { std::cerr << "[INCOMPLETE] No primary screen\n"; return 77; }
  source.move(screen->availableGeometry().center()-QPoint(225,40));
  bool passed=true;
  for(bool background_completion:{false,true}) {
    Fixture f;
    if(!test::native_make_key(baseline)) return 77;
    const auto anchor=SaveSuccessFeedback::capture(&source);
    const QString scenario=background_completion?"background_completion":"switch_after_show";
    f.dialog.path=folder+"/"+scenario+QStringLiteral("_中文实际保存.png");
    HDRSHOT_CHECK(f.submit(UiCommand::save_as,folder,anchor));
    if(background_completion && !foreground_restore.activate()) return 77;
    QElapsedTimer elapsed; elapsed.start();
    f.executor.finish(); wait(100);
    auto *label=feedback(); HDRSHOT_CHECK(label && label->isVisible());
    HDRSHOT_CHECK(!QImage(f.dialog.path).isNull() && label->text().contains(f.dialog.path));
    if(!background_completion && !foreground_restore.activate()) return 77;
    wait(150);
    const auto expected_pid=foreground_restore.process();
    std::cout << "background-focus established-external=" << expected_pid
              << " after-completion-and-switch=" << test::native_focus().process << std::endl;
    HDRSHOT_CHECK(test::native_focus().process==expected_pid);
    const auto rect=label->window()->frameGeometry().adjusted(-8,-8,8,8).translated(-screen->geometry().topLeft());
    auto sample=[&](const QString &phase) {
      const auto state=test::native_window_state(*label->window());
      const auto pixels=screen->grabWindow(0,rect.x(),rect.y(),rect.width(),rect.height()).toImage();
      HDRSHOT_CHECK(!pixels.isNull() && pixels.save(folder+"/"+scenario+"_"+phase+".png"));
      std::cout << "background-feedback scenario=" << scenario.toStdString() << " phase=" << phase.toStdString()
        << " elapsed-ms=" << elapsed.elapsed() << " front-pid=" << test::native_focus().process
        << " expected-external-pid=" << expected_pid << " window=" << state.number
        << " ns-visible=" << state.visible << " ws-onscreen=" << state.on_screen
        << " hides-on-deactivate=" << state.hides_on_deactivate << " qt-visible=" << label->isVisible() << std::endl;
      HDRSHOT_CHECK(test::native_focus().process==expected_pid);
      return std::pair{state,pixels};
    };
    const auto early=sample("early");
    wait(qMax(0,1200-int(elapsed.elapsed())));
    const auto late=sample("1200ms");
    wait(qMax(0,1700-int(elapsed.elapsed())));
    const auto expired=sample("1700ms");
    const bool visible=early.first.visible && early.first.on_screen && late.first.visible && late.first.on_screen;
    const bool expired_ok=!label->isVisible() && !expired.first.visible && !expired.first.on_screen;
    const bool pixels_changed=early.second!=expired.second && late.second!=expired.second;
    std::cout << "background-result scenario=" << scenario.toStdString() << " visible=" << visible
      << " expired=" << expired_ok << " actual-desktop-difference=" << pixels_changed << std::endl;
    passed &= visible && expired_ok && pixels_changed;
  }
  return passed?0:1;
}
#endif
int native_demo(const QString &folder) {
#ifdef __APPLE__
  if(!test::native_desktop_ready()) return 77;
#endif
  HDRSHOT_CHECK(QDir().mkpath(folder));
  HDRSHOT_CHECK(QDir::isAbsolutePath(folder));
  Fixture f;
  QWidget baseline; baseline.setWindowTitle("Save feedback focus baseline"); baseline.resize(500, 250);
  QWidget source; source.setWindowTitle("SeriousShot 保存提示实屏验收"); source.resize(450,80);
  int index=0;
  for(auto *screen:QGuiApplication::screens()) for(bool dark:{false,true}) {
    source.move(screen->availableGeometry().center()-QPoint(225,40));
    QPalette palette=source.palette();palette.setColor(QPalette::Window,dark?QColor(32,34,37):QColor(246,246,246));source.setPalette(palette);
    source.show(); wait(100);
    const auto anchor=SaveSuccessFeedback::capture(&source);
    const auto name=QStringLiteral("/保存成功验收_%1_中文完整路径及实际输出_abcdefghijklmnopqrstuvwxyz_0123456789.png").arg(++index);
    f.dialog.path=folder+name;
    HDRSHOT_CHECK(f.submit(UiCommand::save_as,folder,anchor)); source.hide(); wait(50);
#ifdef __APPLE__
    baseline.show(); baseline.raise(); baseline.activateWindow();
    if(!test::native_make_key(baseline)) { std::cerr << "[INCOMPLETE] Cannot establish valid native foreground/key-window baseline\n"; return 77; }
    wait(100);
    const auto focus_before=test::native_focus();
    HDRSHOT_CHECK(focus_before.process != 0 && focus_before.key_window != 0);
#endif
#ifdef _WIN32
    const auto foreground_before=GetForegroundWindow();
#endif
    f.executor.finish(); wait(200);
    auto *label=feedback();HDRSHOT_CHECK(label && label->isVisible());
    HDRSHOT_CHECK(QFileInfo(f.dialog.path).isAbsolute() && !QImage(f.dialog.path).isNull());
    HDRSHOT_CHECK(label->text().contains(f.dialog.path));
#ifdef __APPLE__
    HDRSHOT_CHECK(test::native_focus()==focus_before);
    std::cout << "native-focus process=" << focus_before.process << " key=" << focus_before.key_window << " unchanged-on-show=1\n";
#endif
    HDRSHOT_CHECK(screen->availableGeometry().contains(label->window()->frameGeometry()));
    std::cout<<"native-feedback screen="<<screen->name().toStdString()<<" dpr="<<screen->devicePixelRatio()<<" file="<<f.dialog.path.toStdString()<<'\n';
#ifdef _WIN32
    std::cout<<"foreground-before="<<foreground_before<<" after="<<GetForegroundWindow()<<'\n';
    HDRSHOT_CHECK(GetForegroundWindow()==foreground_before);
#endif
    const auto rect=label->window()->frameGeometry().adjusted(-8,-8,8,8).translated(-screen->geometry().topLeft());
    HDRSHOT_CHECK(screen->grabWindow(0,rect.x(),rect.y(),rect.width(),rect.height()).save(folder+QString("/feedback_%1.png").arg(index)));
    const auto visible_pixels=screen->grabWindow(0,rect.x(),rect.y(),rect.width(),rect.height()).toImage();
    HDRSHOT_CHECK(!visible_pixels.isNull());
#ifdef __APPLE__
    if(index==1) {
      // Restore every advertised clipboard format after the real context-menu sample.
      auto saved=std::make_unique<QMimeData>();
      const auto *clipboard=QApplication::clipboard()->mimeData();
      if(clipboard) for(const auto &format:clipboard->formats()) saved->setData(format,clipboard->data(format));
      struct Restore { std::unique_ptr<QMimeData> data; ~Restore(){QApplication::clipboard()->setMimeData(data.release());} } restore{std::move(saved)};
      bool sampled=false;
      QTimer::singleShot(1700,label,[&] {
        auto *menu=qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if(!menu || !menu->isVisible() || !label->isVisible()) { if(menu) menu->close(); return; }
        sampled=true;
        const auto point=menu->actionGeometry(menu->actions().front()).center();
        // Keep failures outside the native callback, so clipboard RAII can run.
        try { test::native_mouse(*menu,point,true); test::native_mouse(*menu,point,false); }
        catch(...) { sampled=false; menu->close(); }
      });
      test::native_mouse(*label,label->rect().center(),true,true);
      test::native_mouse(*label,label->rect().center(),false,true);
      HDRSHOT_CHECK(sampled && QApplication::clipboard()->text()==f.dialog.path);
      HDRSHOT_CHECK(label->isVisible());
      std::cout << "native-context-menu held=1700ms copied-full-path=1 clipboard-restored=1\n";
      wait(1600);
    } else
#endif
    { wait(1100); HDRSHOT_CHECK(label->isVisible()); wait(350); }
    HDRSHOT_CHECK(!label->isVisible());
    const auto hidden_pixels=screen->grabWindow(0,rect.x(),rect.y(),rect.width(),rect.height()).toImage();
    HDRSHOT_CHECK(!hidden_pixels.isNull() && hidden_pixels!=visible_pixels);
#ifdef __APPLE__
    HDRSHOT_CHECK(test::native_focus()==focus_before);
    std::cout << "native-focus unchanged-on-hide=1\n";
#endif
  }
  return 0;
}
}
int main(int argc,char **argv) {
  QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
#ifdef __APPLE__
  if(argc==2 && QString::fromLocal8Bit(argv[1])=="--native-window-contract") {
    try { return native_window_contract(); }
    catch(const std::exception &error) { std::cerr << "[FAIL] native window contract: " << error.what() << '\n'; return 1; }
  }
  if(argc==3 && QString::fromLocal8Bit(argv[1])=="--native-background-demo") {
    try { return native_background_demo(QString::fromLocal8Bit(argv[2])); }
    catch(const std::exception &error) { std::cerr << "[FAIL] native background feedback: " << error.what() << '\n'; return 1; }
  }
#endif
  if(argc==3 && QString::fromLocal8Bit(argv[1])=="--native-demo") {
    try { return native_demo(QString::fromLocal8Bit(argv[2])); }
    catch(const std::exception &error) { std::cerr << "[FAIL] native feedback: " << error.what() << '\n'; return 1; }
  }
  return test::run({{"real save completion survives source destruction and queues safely",pipeline_and_lifecycle},
                    {"cancel failure and clipboard produce no save success",cancelled_failed_and_clipboard}});
}
