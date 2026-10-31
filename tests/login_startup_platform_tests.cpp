#include "platform/qt/qt_gui_instance_guard.hpp"
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
 {"instance lock I/O failure is distinct from running",[]{QTemporaryDir dir;put(dir.filePath("file"),"x");QtGuiInstanceGuard lock(dir.filePath("file/gui.lock"));HDRSHOT_CHECK(lock.status()==GuiInstanceStatus::io_error);}}
 });}
