#include "platform/qt/update/native_update_backend.hpp"
#include "platform/qt/update/update_catalog.hpp"
#include "platform/windows/update/windows_update_native.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <cstdio>
#include <windows.h>
using namespace hdrshot::update;
// Opt-in native fixture. It runs only inside the controller's isolated managed
// installation; no settings, tray, network or user application are touched.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const auto args = app.arguments();
  if(args.size()==3 && args[1]=="--move-probe") {
    const auto root=QDir::fromNativeSeparators(args[2]);
    const auto from=root+"/SeriousShot.exe",to=root+"/.seriousshot-backup-"+QString(36,'0')+"/SeriousShot.exe";
    if(!QDir().mkpath(QFileInfo(to).absolutePath()))return 18;
    QFile fixture(from);if(!fixture.open(QIODevice::WriteOnly))return 19;
    fixture.write("long path fixture");fixture.close();
    if(windows_extended_path("//server/share/file")!=QStringLiteral("\\\\?\\UNC\\server\\share\\file"))return 20;
    const auto extended=windows_extended_path(from);
    if(windows_extended_path(extended)!=extended || !windows_move(from,to) || !windows_move(to,from))return 21;
    const auto missing=windows_move(root+"/missing",to);
    if(missing || !missing.error().safe_context.contains("native_error"))return 22;
    std::printf("long native move PASS destination_length=%lld native_error=%s\n",
      static_cast<long long>(to.size()),missing.error().safe_context.at("native_error").c_str());
    return 0;
  }
  if(args.size()==3 && args[1]=="--startup") {
    const bool refused=qEnvironmentVariableIsSet("SERIOUSSHOT_UPDATE_STARTUP_REFUSE");
    const auto recovered=windows_begin_startup_recovery(args[2]);
    const bool expected=refused?(!recovered && recovered.error().code==hdrshot::ErrorCode::operation_cancelled):
        (recovered && recovered.value());
    if(!expected)return 16;
    (void)save_json(args[2]+"/startup-caller.json",{{"armed",!refused},
      {"pid",QString::number(app.applicationPid())},{"creation",process_identity(app.applicationPid())},
      {"exe",windows_executable()}});
    while(!QFileInfo::exists(args[2]+"/release-startup"))QThread::msleep(50);
    return 0;
  }
  if(args.size()==1 && qEnvironmentVariableIsSet("SERIOUSSHOT_STARTUP_TEST_SLOT")) {
    const auto root=qEnvironmentVariable("SERIOUSSHOT_STARTUP_TEST_SLOT");
    const auto recovered=windows_begin_startup_recovery(root);
    if(!recovered)return 17;
    if(recovered.value())return 0;
    (void)save_json(root+"/restarted.json",{{"pid",QString::number(app.applicationPid())},
      {"creation",process_identity(app.applicationPid())},{"exe",windows_executable()}});
    while(!QFileInfo::exists(root+"/stop-restarted"))QThread::msleep(50);
    return 0;
  }
  if (args.size()==3 && args[1]=="--cleanup") return cleanup_managed(QDir::fromNativeSeparators(args[2]))?0:11;
  if (args.contains("--update-receipt")) {
    if (qEnvironmentVariableIsSet("SERIOUSSHOT_TEST_NO_RECEIPT")) return 10;
    windows_write_start_receipt();
    const auto stop = QCoreApplication::applicationDirPath() + "/stop";
    for (int n=0; n<1800 && !QFileInfo::exists(stop); ++n) QThread::msleep(100);
    return 0;
  }
  if ((args.size()==3 && args[1]=="--hold") || (args.size()==4 && args[1]=="--hold-library")) {
    // Qt6Network is a real mapped DLL dependency in this process. The helper
    // must wait for this same-installation process before moving any DLL.
    const auto library=args[1]=="--hold-library" ? args[2] : QCoreApplication::applicationDirPath()+"/Qt6Network.dll";
    const auto release=args.last();
    const auto loaded=LoadLibraryW(reinterpret_cast<LPCWSTR>(library.utf16()));
    if (!loaded) return 12;
    wchar_t mapped[32768];
    const auto length=GetModuleFileNameW(loaded,mapped,32768);
    if (!length) return 14;
    (void)save_json(release+".ready", {{"pid",QString::number(app.applicationPid())},
      {"mapped_library",QDir::fromNativeSeparators(QString::fromWCharArray(mapped,static_cast<int>(length)))}});
    while (!QFileInfo::exists(release)) QThread::msleep(50);
    if (args[1]=="--hold-library") {
      const auto function=reinterpret_cast<int(*)()>(GetProcAddress(loaded,"seriousshot_update_lock_fixture"));
      if (!function || function()!=1) return 15;
    }
    FreeLibrary(loaded);
    return 0;
  }
  if (args.size()!=6 || args[1]!="--install") return 2;
  QFile file(args[2]);
  if (!file.open(QIODevice::ReadOnly)) return 3;
  const auto envelope=file.readAll();
  const auto catalog=verify_catalog(envelope,compiled_trust());
  if (!catalog || catalog.value().targets.size()!=1) return 4;
  const auto c=catalog.value().targets.front();
  Slot slot(QDir::fromNativeSeparators(args[4]),native_installer()->install_identity(),"testing");
  if (!slot.reserve(c) || !QFile::copy(args[3],slot.payload())) return 5;
  if (!QFile::rename(slot.payload(),slot.root+"/payload.zip")) return 13;
  slot.record["suffix"]=".zip";
  slot.record["phase"]="ready";
  if (!slot.save()) return 6;
  auto native=native_installer();
  if (native->manual()) return 7;
  const bool reject=args[5]=="reject";
  auto result=native->install(slot,c,envelope,[&]{return !reject;});
  if (reject) {
    const bool safe=!result && slot.record["phase"]=="ready" && QFileInfo::exists(slot.payload());
    // Exit normally after refusal: an unarmed helper must not apply later.
    return safe?0:8;
  }
  return result?0:9;
}
