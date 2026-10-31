#include "platform/qt/qt_gui_instance_guard.hpp"
#include "platform/macos/macos_login_startup_paths.hpp"
#include "test_support.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QProcess>
#include <QTemporaryDir>
using namespace hdrshot;
static void put(const QString& path,const QByteArray& data={}) {QDir().mkpath(QFileInfo(path).absolutePath());QFile f(path);HDRSHOT_CHECK(f.open(QIODevice::WriteOnly));HDRSHOT_CHECK(f.write(data)==data.size());}
int main(int argc,char**argv){QCoreApplication app(argc,argv);
 if(app.arguments().value(1)=="--hold-lock") {QtGuiInstanceGuard guard(app.arguments().value(2));if(guard.status()!=GuiInstanceStatus::owned)return 3;put(app.arguments().value(3),"ready");for(;;)QThread::msleep(20);}
 return test::run({
 {"live process owns lock and dead process stale lock is recovered",[]{QTemporaryDir dir;const auto lock=dir.filePath("gui.lock"),ready=dir.filePath("ready");QProcess child;child.start(QCoreApplication::applicationFilePath(),{"--hold-lock",lock,ready});HDRSHOT_CHECK(child.waitForStarted());for(int n=0;n<200&&!QFileInfo::exists(ready);++n)QThread::msleep(10);HDRSHOT_CHECK(QFileInfo::exists(ready));{QtGuiInstanceGuard second(lock);HDRSHOT_CHECK(second.status()==GuiInstanceStatus::already_running);}child.kill();HDRSHOT_CHECK(child.waitForFinished());QtGuiInstanceGuard recovered(lock);HDRSHOT_CHECK(recovered.status()==GuiInstanceStatus::owned);}},
 {"instance lock I/O failure is distinct from running",[]{QTemporaryDir dir;put(dir.filePath("file"),"x");QtGuiInstanceGuard lock(dir.filePath("file/gui.lock"));HDRSHOT_CHECK(lock.status()==GuiInstanceStatus::io_error);}},
 {"project build cache evidence is required and canonical",[]{QTemporaryDir dir;const auto source=dir.filePath("source"),build=dir.filePath("build"),bundle=build+"/SeriousShot.app";QDir().mkpath(bundle);HDRSHOT_CHECK(!macos_login_target_is_project_staging(bundle));put(source+"/CMakeLists.txt","project(HDRShot VERSION 0.1.9)");put(source+"/src/ui/qt/settings_window.cpp");put(source+"/tools/macos_qt_overlay_poc.mm");put(build+"/CMakeCache.txt",("CMAKE_PROJECT_NAME:STATIC=HDRShot\nCMAKE_HOME_DIRECTORY:INTERNAL="+source+"\n").toUtf8());HDRSHOT_CHECK(macos_login_target_is_project_staging(bundle));HDRSHOT_CHECK(QFile::link(bundle,dir.filePath("alias.app")));HDRSHOT_CHECK(macos_login_target_is_project_staging(dir.filePath("alias.app")));QDir().mkpath(dir.filePath("build-copy/SeriousShot.app"));HDRSHOT_CHECK(!macos_login_target_is_project_staging(dir.filePath("build-copy/SeriousShot.app")));}},
 {"source staging requires real structure, plain same-name directories allowed",[]{QTemporaryDir dir;const auto source=dir.filePath("source");const auto bundle=source+"/.agent/tmp/SeriousShot.app";QDir().mkpath(bundle);HDRSHOT_CHECK(!macos_login_target_is_project_staging(bundle));put(source+"/CMakeLists.txt","project(HDRShot LANGUAGES CXX)");put(source+"/src/ui/qt/settings_window.cpp");put(source+"/tools/macos_qt_overlay_poc.mm");HDRSHOT_CHECK(macos_login_target_is_project_staging(bundle));QDir().mkpath(source+"/artifacts/review/SeriousShot.app");HDRSHOT_CHECK(macos_login_target_is_project_staging(source+"/artifacts/review/SeriousShot.app"));QDir().mkpath(source+"/Applications/SeriousShot.app");HDRSHOT_CHECK(!macos_login_target_is_project_staging(source+"/Applications/SeriousShot.app"));}}
 });}
