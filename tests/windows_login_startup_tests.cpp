#include "platform/windows/windows_login_startup_port.hpp"
#include "application/login_startup_workflow.hpp"
#include "test_support.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <windows.h>
using namespace hdrshot;
static bool exists(const QString& p){return GetFileAttributesW(reinterpret_cast<LPCWSTR>(p.utf16()))!=INVALID_FILE_ATTRIBUTES;}
static QByteArray bytes(const QString& p){QFile f(p);HDRSHOT_CHECK(f.open(QIODevice::ReadOnly));return f.readAll();}
int run(int argc,char** argv) {
  QCoreApplication app(argc,argv);
  QTemporaryDir root(QDir::tempPath()+"/T72-XXXXXX");HDRSHOT_CHECK(root.isValid());
  std::cout<<"temporary_root="<<root.path().toStdString()<<"\n";
  const auto executable=root.path()+QStringLiteral("/安装 with 空格/SeriousShot.exe");
  HDRSHOT_CHECK(QDir().mkpath(QFileInfo(executable).absolutePath()));
  HDRSHOT_CHECK(QFile::copy(app.applicationFilePath(),executable));
  const auto startup=root.path()+"/Startup";HDRSHOT_CHECK(QDir().mkpath(startup));
  WindowsStartupApproval approval=WindowsStartupApproval::missing_no_override;
  WindowsLoginStartupContext context;
  context.startup_directory=[&]{return Result<QString,Error>::success(startup);};
  context.current_executable=[&]{return executable;};
  context.approval=[&]{return WindowsStartupApprovalRead{approval,{}};};
  WindowsLoginStartupPort port(context);LoginStartupWorkflow workflow(port);
  const auto item=startup+"/SeriousShot.lnk";
  HDRSHOT_CHECK(port.query().value().state==LoginStartupState::disabled);
  HDRSHOT_CHECK(port.query().value().can_enable && !exists(item));
  const auto created=workflow.change_enabled(true);
  HDRSHOT_CHECK(!created.operation_error && exists(item));
  HDRSHOT_CHECK(created.snapshot.state==LoginStartupState::enabled);
  auto original=bytes(item);
  HDRSHOT_CHECK(port.set_enabled(true) && bytes(item)==original);
  HDRSHOT_CHECK(!workflow.change_enabled(false).operation_error && !exists(item));
  for(int i=0;i<3;++i)HDRSHOT_CHECK(port.query().value().state==LoginStartupState::disabled && !exists(item));
  HDRSHOT_CHECK(!workflow.change_enabled(true).operation_error && port.query().value().state==LoginStartupState::enabled);
  original=bytes(item);
  approval=WindowsStartupApproval::unknown;
  HDRSHOT_CHECK(port.query().value().state==LoginStartupState::unknown && !port.set_enabled(true));
  approval=WindowsStartupApproval::enabled;
  auto current=port.query().value();
  HDRSHOT_CHECK(current.state==LoginStartupState::enabled);
  HDRSHOT_CHECK(current.system_registered_target_path==executable.toStdString());
  HDRSHOT_CHECK(port.set_enabled(true) && bytes(item)==original);
  WindowsLoginStartupContext foreign_context=context;
  foreign_context.ownership_marker="another owner";
  WindowsLoginStartupPort foreign_port(foreign_context);
  HDRSHOT_CHECK(!foreign_port.query().value().can_enable && !foreign_port.query().value().can_disable);
  HDRSHOT_CHECK(!foreign_port.set_enabled(true) && !foreign_port.set_enabled(false) && bytes(item)==original);
  const auto alias=root.path()+"/hardlink/SeriousShot.exe";
  HDRSHOT_CHECK(QDir().mkpath(QFileInfo(alias).absolutePath()));
  HDRSHOT_CHECK(CreateHardLinkW(reinterpret_cast<LPCWSTR>(alias.utf16()),reinterpret_cast<LPCWSTR>(executable.utf16()),nullptr));
  auto alias_context=context;alias_context.current_executable=[&]{return alias;};
  WindowsLoginStartupPort alias_port(alias_context);
  HDRSHOT_CHECK(alias_port.query().value().state==LoginStartupState::invalid_target);
  HDRSHOT_CHECK(bytes(item)==original);
  // B1: OS approval survives removal. Queries never re-create the shortcut.
  HDRSHOT_CHECK(!workflow.change_enabled(false).operation_error);
  for(int i=0;i<3;++i)HDRSHOT_CHECK(port.query().value().state==LoginStartupState::disabled && !exists(item));
  HDRSHOT_CHECK(!workflow.change_enabled(true).operation_error && port.query().value().state==LoginStartupState::enabled);
  // Native locked replace preserves old bytes; stale target repair is explicit.
  const auto moved=executable+".old";HDRSHOT_CHECK(QFile::rename(executable,moved));
  HDRSHOT_CHECK(QFile::copy(app.applicationFilePath(),executable));
  // Different directory forces an actual stale shortcut rather than same-path replacement.
  const auto new_exe=root.path()+"/stable/SeriousShot.exe";HDRSHOT_CHECK(QDir().mkpath(QFileInfo(new_exe).absolutePath()));
  HDRSHOT_CHECK(QFile::copy(app.applicationFilePath(),new_exe));
  context.current_executable=[&]{return new_exe;};WindowsLoginStartupPort relocated(context);
  HDRSHOT_CHECK(relocated.query().value().state==LoginStartupState::invalid_target && relocated.query().value().can_enable);
  const auto before_lock=bytes(item);
  HANDLE lock=CreateFileW(reinterpret_cast<LPCWSTR>(item.utf16()),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
  HDRSHOT_CHECK(lock!=INVALID_HANDLE_VALUE);
  const auto failed=relocated.set_enabled(true);CloseHandle(lock);
  HDRSHOT_CHECK(!failed && bytes(item)==before_lock);
  approval=WindowsStartupApproval::missing_no_override;
  HDRSHOT_CHECK(relocated.query().value().can_enable);
  HDRSHOT_CHECK(relocated.set_enabled(true).has_value());
  HDRSHOT_CHECK(relocated.query().value().state==LoginStartupState::enabled);
  HDRSHOT_CHECK(relocated.set_enabled(false).has_value());
  for(const auto blocked:{WindowsStartupApproval::blocked,WindowsStartupApproval::unknown}) {
    approval=blocked;HDRSHOT_CHECK(!relocated.query().value().can_enable);
    if(blocked==WindowsStartupApproval::blocked) HDRSHOT_CHECK(relocated.query().value().approval_message=="请在 Windows 设置的“启动”设置中，将 SeriousShot 的启动设置为“开”。");
    HDRSHOT_CHECK(!relocated.set_enabled(true) && !exists(item));
  }
  approval=WindowsStartupApproval::enabled;
  {QFile foreign(item);HDRSHOT_CHECK(foreign.open(QIODevice::WriteOnly));foreign.write("foreign corrupt item");}
  const auto foreign=bytes(item);
  const auto foreign_state=relocated.query().value();
  std::cout<<"foreign state="<<static_cast<int>(foreign_state.state)<<" enable="<<foreign_state.can_enable<<" disable="<<foreign_state.can_disable<<" exists="<<exists(item)<<" diagnostic="<<foreign_state.diagnostic<<"\n";
  HDRSHOT_CHECK(!foreign_state.can_disable && !foreign_state.can_enable);
  HDRSHOT_CHECK(!relocated.set_enabled(true) && !relocated.set_enabled(false) && bytes(item)==foreign);
  HDRSHOT_CHECK(QFile::remove(item));
  for(const auto changed:{WindowsStartupApproval::blocked,WindowsStartupApproval::unknown}) {
    auto racing_context=context;int reads=0;
    racing_context.approval=[&]{return WindowsStartupApprovalRead{++reads==1?WindowsStartupApproval::missing_no_override:changed,{}};};
    WindowsLoginStartupPort racing(racing_context);
    HDRSHOT_CHECK(!racing.set_enabled(true) && reads>=2 && !exists(item));
  }
  context.current_executable=[&]{return QString(270,'x')+".exe";};WindowsLoginStartupPort long_target(context);
  HDRSHOT_CHECK(long_target.query().value().state==LoginStartupState::invalid_target && !long_target.set_enabled(true));
  HDRSHOT_CHECK(classify_windows_startup_approval_read(ERROR_FILE_NOT_FOUND,0,{})==WindowsStartupApproval::missing_no_override);
  for(const auto error:{ERROR_ACCESS_DENIED,ERROR_PATH_NOT_FOUND,ERROR_MORE_DATA})
    HDRSHOT_CHECK(classify_windows_startup_approval_read(error,REG_BINARY,QByteArray(12,0))==WindowsStartupApproval::unknown);
  HDRSHOT_CHECK(classify_windows_startup_approval_read(ERROR_SUCCESS,REG_BINARY,{})==WindowsStartupApproval::unknown);
  HDRSHOT_CHECK(decode_windows_startup_approval(REG_BINARY,QByteArray(12,0))==WindowsStartupApproval::enabled);
  auto blob=QByteArray(12,0);blob[0]=1;blob[11]=42;
  HDRSHOT_CHECK(decode_windows_startup_approval(REG_BINARY,blob)==WindowsStartupApproval::blocked);
  for(char state:{2,3,4}){blob[0]=state;HDRSHOT_CHECK(decode_windows_startup_approval(REG_BINARY,blob)==WindowsStartupApproval::unknown);}
  blob=QByteArray(12,0);blob[11]=1;HDRSHOT_CHECK(decode_windows_startup_approval(REG_BINARY,blob)==WindowsStartupApproval::unknown);
  HDRSHOT_CHECK(decode_windows_startup_approval(REG_SZ,QByteArray(12,0))==WindowsStartupApproval::unknown);
  HDRSHOT_CHECK(decode_windows_startup_approval(REG_BINARY,QByteArray(11,0))==WindowsStartupApproval::unknown);
  HDRSHOT_CHECK(decode_windows_startup_approval(REG_BINARY,{})==WindowsStartupApproval::unknown);
  auto staging_context=context;staging_context.current_executable=[&]{return app.applicationFilePath();};
  WindowsLoginStartupPort staging(staging_context);
  // The real test executable is inside the project build directory: location
  // alone must not prevent explicit registration into our temporary Startup.
  HDRSHOT_CHECK(staging.query().value().state==LoginStartupState::disabled && staging.query().value().can_enable);
  HDRSHOT_CHECK(staging.set_enabled(true).has_value());
  HDRSHOT_CHECK(staging.query().value().state==LoginStartupState::enabled);
  HDRSHOT_CHECK(staging.set_enabled(false).has_value());
  HDRSHOT_CHECK(QDir(startup).entryList(QDir::Files).empty());
  std::cout<<"Windows login startup native temporary-dir transaction and policy PASS\n";
  return 0;
}
int main(int argc,char**argv){try{return run(argc,argv);}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
