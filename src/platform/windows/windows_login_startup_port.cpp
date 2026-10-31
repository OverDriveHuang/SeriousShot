#include "platform/windows/windows_login_startup_port.hpp"
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <array>
namespace hdrshot {
namespace {
using Microsoft::WRL::ComPtr;
const wchar_t* wide(const QString& s) { return reinterpret_cast<const wchar_t*>(s.utf16()); }
Error native_error(const char* operation, unsigned long code, const char* domain="Win32",std::source_location origin=std::source_location::current()) {
  wchar_t* message=nullptr;
  const auto length=FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr,code,0,reinterpret_cast<wchar_t*>(&message),0,nullptr);
  const auto description=length?QString::fromWCharArray(message,static_cast<int>(length)).trimmed().toStdString():std::string{};
  if(message)LocalFree(message);
  const auto error_code=(code==ERROR_ACCESS_DENIED || code==static_cast<unsigned long>(E_ACCESSDENIED))?ErrorCode::permission_denied:ErrorCode::precondition_failed;
  return {error_code,"windows_login_startup",Retryability::after_user_action,
      {{"operation",operation},{"domain",domain},{"native_code",std::to_string(code)},
       {"description",description},{"reason","登录启动操作未完成，请检查启动设置或应用位置后再次勾选。"}},origin};
}
Error problem(std::string reason) {
  return {ErrorCode::precondition_failed,"windows_login_startup",Retryability::after_user_action,{{"reason",std::move(reason)}}};
}
struct ComScope {
  HRESULT status=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
  ~ComScope(){if(SUCCEEDED(status))CoUninitialize();}
  bool usable() const {return SUCCEEDED(status)||status==RPC_E_CHANGED_MODE;}
};
bool item_exists(const QString& path) {return GetFileAttributesW(wide(path))!=INVALID_FILE_ATTRIBUTES;}
struct Link { QString target,args,workdir,marker; friend bool operator==(const Link&,const Link&)=default; };
Result<Link,Error> read_link(const QString& path) {
  ComScope scope;
  if(!scope.usable())return Result<Link,Error>::failure(native_error("CoInitializeEx",static_cast<unsigned long>(scope.status),"HRESULT"));
  ComPtr<IShellLinkW> link;
  auto hr=CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link));
  if(FAILED(hr))return Result<Link,Error>::failure(native_error("CoCreateInstance",static_cast<unsigned long>(hr),"HRESULT"));
  ComPtr<IPersistFile> file;hr=link.As(&file);
  if(FAILED(hr))return Result<Link,Error>::failure(native_error("QueryInterface(IPersistFile)",static_cast<unsigned long>(hr),"HRESULT"));
  hr=file->Load(wide(path),STGM_READ);
  if(FAILED(hr))return Result<Link,Error>::failure(native_error("IPersistFile::Load",static_cast<unsigned long>(hr),"HRESULT"));
  std::array<wchar_t,MAX_PATH> target{},args{},workdir{},marker{};
  // GetPath's documented reliable bound is MAX_PATH. Never treat a full
  // buffer as a complete registered target, nor ask Resolve to repair it.
  hr=link->GetPath(target.data(),MAX_PATH,nullptr,SLGP_RAWPATH);
  if(FAILED(hr))return Result<Link,Error>::failure(native_error("IShellLinkW::GetPath",static_cast<unsigned long>(hr),"HRESULT"));
  hr=link->GetArguments(args.data(),MAX_PATH);
  if(FAILED(hr))return Result<Link,Error>::failure(native_error("IShellLinkW::GetArguments",static_cast<unsigned long>(hr),"HRESULT"));
  hr=link->GetWorkingDirectory(workdir.data(),MAX_PATH);
  if(FAILED(hr))return Result<Link,Error>::failure(native_error("IShellLinkW::GetWorkingDirectory",static_cast<unsigned long>(hr),"HRESULT"));
  hr=link->GetDescription(marker.data(),MAX_PATH);
  if(FAILED(hr))return Result<Link,Error>::failure(native_error("IShellLinkW::GetDescription",static_cast<unsigned long>(hr),"HRESULT"));
  if(wcslen(target.data())>=MAX_PATH-1 || wcslen(args.data())>=MAX_PATH-1 ||
      wcslen(workdir.data())>=MAX_PATH-1 || wcslen(marker.data())>=MAX_PATH-1)
    return Result<Link,Error>::failure(problem("启动项字段超过可靠回读长度，请使用较短的稳定安装路径。"));
  return Result<Link,Error>::success({QDir::fromNativeSeparators(QString::fromWCharArray(target.data())),
      QString::fromWCharArray(args.data()),QDir::fromNativeSeparators(QString::fromWCharArray(workdir.data())),QString::fromWCharArray(marker.data())});
}
Result<bool,Error> write_link(const QString& path,const Link& value) {
  ComScope scope;
  if(!scope.usable())return Result<bool,Error>::failure(native_error("CoInitializeEx",static_cast<unsigned long>(scope.status),"HRESULT"));
  ComPtr<IShellLinkW> link;
  auto hr=CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link));
  if(FAILED(hr))return Result<bool,Error>::failure(native_error("CoCreateInstance",static_cast<unsigned long>(hr),"HRESULT"));
  const auto check=[](HRESULT result,const char* operation,std::source_location origin=std::source_location::current())->Result<bool,Error> {
    if(FAILED(result))return Result<bool,Error>::failure(native_error(operation,static_cast<unsigned long>(result),"HRESULT",origin));
    return Result<bool,Error>::success(true);
  };
  auto status=check(link->SetPath(wide(value.target)),"IShellLinkW::SetPath");if(!status)return status;
  status=check(link->SetArguments(wide(value.args)),"IShellLinkW::SetArguments");if(!status)return status;
  status=check(link->SetWorkingDirectory(wide(value.workdir)),"IShellLinkW::SetWorkingDirectory");if(!status)return status;
  status=check(link->SetDescription(wide(value.marker)),"IShellLinkW::SetDescription");if(!status)return status;
  ComPtr<IPersistFile> file;status=check(link.As(&file),"QueryInterface(IPersistFile)");if(!status)return status;
  return check(file->Save(wide(path),TRUE),"IPersistFile::Save");
}
bool reparse_path(QString path) {
  for(int n=0;n<MAX_PATH && !path.isEmpty();++n) {
    const auto attrs=GetFileAttributesW(wide(QDir::toNativeSeparators(path)));
    if(attrs!=INVALID_FILE_ATTRIBUTES && (attrs&FILE_ATTRIBUTE_REPARSE_POINT))return true;
    QDir parent(QFileInfo(path).absolutePath());
    if(parent.absolutePath()==path)break;
    path=parent.absolutePath();
  }
  return false;
}
bool same_file(const QString& a,const QString& b) {
  const auto open=[](const QString& p){return CreateFileW(wide(QDir::toNativeSeparators(p)),0,
      FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);};
  const auto first=open(a),second=open(b);
  BY_HANDLE_FILE_INFORMATION ai{},bi{};
  const bool equal=first!=INVALID_HANDLE_VALUE && second!=INVALID_HANDLE_VALUE &&
      GetFileInformationByHandle(first,&ai) && GetFileInformationByHandle(second,&bi) &&
      ai.dwVolumeSerialNumber==bi.dwVolumeSerialNumber && ai.nFileIndexHigh==bi.nFileIndexHigh && ai.nFileIndexLow==bi.nFileIndexLow;
  if(first!=INVALID_HANDLE_VALUE)CloseHandle(first);
  if(second!=INVALID_HANDLE_VALUE)CloseHandle(second);
  return equal;
}
bool current_target(const QString& registered,const QString& current) {
  const auto a=QFileInfo(registered).canonicalFilePath(),b=QFileInfo(current).canonicalFilePath();
  return !a.isEmpty() && !b.isEmpty() && a.compare(b,Qt::CaseInsensitive)==0 && same_file(registered,current);
}
bool owned(const Link& l,const WindowsLoginStartupContext& context) {
  return l.marker==context.ownership_marker && l.args=="--login-startup" &&
      QDir::isAbsolutePath(l.target) && l.target.endsWith(".exe",Qt::CaseInsensitive) &&
      QDir::cleanPath(l.target)==l.target &&
      QDir::cleanPath(l.workdir)==QFileInfo(l.target).absolutePath() && !reparse_path(l.target);
}
QByteArray read_bounded(const QString& path) {
  QFile file(path);if(!file.open(QIODevice::ReadOnly)||file.size()>1024*1024)return {};
  return file.readAll();
}
std::string target_problem(const QString& exe) {
  if(exe.size()+1>MAX_PATH)return "应用路径超过 Windows 启动项可靠回读长度，请将完整应用复制到较短的稳定目录后再次勾选。";
  if(!QDir::isAbsolutePath(exe)||!QFileInfo(exe).isFile()||reparse_path(exe))return "无法确认当前应用的实际位置，请在稳定目录运行完整应用后再次勾选。";
  return {};
}
Result<QString,Error> startup_directory() {
  PWSTR path=nullptr;
  const auto hr=SHGetKnownFolderPath(FOLDERID_Startup,KF_FLAG_DONT_VERIFY,nullptr,&path);
  if(FAILED(hr))return Result<QString,Error>::failure(native_error("SHGetKnownFolderPath",static_cast<unsigned long>(hr),"HRESULT"));
  const auto result=QDir::fromNativeSeparators(QString::fromWCharArray(path));CoTaskMemFree(path);
  return Result<QString,Error>::success(result);
}
WindowsStartupApprovalRead read_approval(const QString& basename) {
  HKEY key=nullptr;
  auto status=RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder",0,KEY_QUERY_VALUE,&key);
  if(status==ERROR_FILE_NOT_FOUND)return {classify_windows_startup_approval_read(status,0,{}),{}};
  if(status!=ERROR_SUCCESS)return {classify_windows_startup_approval_read(status,0,{}),"RegOpenKeyExW Win32 "+std::to_string(status)};
  struct Close {HKEY key;~Close(){RegCloseKey(key);}} close{key};
  std::array<char,12> bytes{};DWORD type=0,length=12;
  status=RegQueryValueExW(key,wide(basename),nullptr,&type,reinterpret_cast<BYTE*>(bytes.data()),&length);
  if(status==ERROR_FILE_NOT_FOUND)return {classify_windows_startup_approval_read(status,0,{}),{}};
  if(status!=ERROR_SUCCESS)return {classify_windows_startup_approval_read(status,0,{}),"RegQueryValueExW Win32 "+std::to_string(status)};
  return {classify_windows_startup_approval_read(status,type,QByteArray(bytes.data(),static_cast<qsizetype>(length))),{}};
}
}
WindowsStartupApproval decode_windows_startup_approval(unsigned long type,const QByteArray& b) {
  if(type!=REG_BINARY || b.size()!=12)return WindowsStartupApproval::unknown;
  if(b==QByteArray(12,'\0'))return WindowsStartupApproval::enabled;
  if(static_cast<unsigned char>(b[0])==1 && b[1]==0 && b[2]==0 && b[3]==0)return WindowsStartupApproval::blocked;
  return WindowsStartupApproval::unknown;
}
WindowsStartupApproval classify_windows_startup_approval_read(unsigned long status,unsigned long type,const QByteArray& bytes) {
  if(status==ERROR_FILE_NOT_FOUND)return WindowsStartupApproval::missing_no_override;
  if(status!=ERROR_SUCCESS)return WindowsStartupApproval::unknown;
  return decode_windows_startup_approval(type,bytes);
}
WindowsLoginStartupPort::WindowsLoginStartupPort(WindowsLoginStartupContext context):context_(std::move(context)) {
  if(!context_.startup_directory)context_.startup_directory=startup_directory;
  if(!context_.current_executable)context_.current_executable=[] {return QCoreApplication::applicationFilePath();};
  if(!context_.approval)context_.approval=[basename=context_.item_basename] {return read_approval(basename);};
}
Result<LoginStartupSnapshot,Error> WindowsLoginStartupPort::query() {
  LoginStartupSnapshot result;
  result.current_bundle_path=context_.current_executable().toStdString();
  result.can_open_system_settings=true;
  result.approval_message="请在 Windows 设置的“启动”设置中，将 SeriousShot 的启动设置为“开”。";
  const auto directory=context_.startup_directory();if(!directory)return Result<LoginStartupSnapshot,Error>::failure(directory.error());
  if(context_.item_basename.contains('/')||context_.item_basename.contains('\\')||!context_.item_basename.endsWith(".lnk")||context_.ownership_marker.size()>=MAX_PATH-1)
    return Result<LoginStartupSnapshot,Error>::failure(problem("登录启动身份无效。"));
  const auto path=directory.value()+"/"+context_.item_basename;
  const auto approval=context_.approval();
  const auto issue=target_problem(QString::fromStdString(result.current_bundle_path));
  const auto attrs=GetFileAttributesW(wide(path));
  const auto path_error=attrs==INVALID_FILE_ATTRIBUTES?GetLastError():ERROR_SUCCESS;
  if(attrs==INVALID_FILE_ATTRIBUTES && path_error!=ERROR_FILE_NOT_FOUND && path_error!=ERROR_PATH_NOT_FOUND)
    return Result<LoginStartupSnapshot,Error>::failure(native_error("GetFileAttributesW(startup item)",path_error));
  const bool exists=attrs!=INVALID_FILE_ATTRIBUTES;
  if(reparse_path(directory.value()))return Result<LoginStartupSnapshot,Error>::failure(problem("启动目录含重解析路径，无法安全管理启动项。"));
  if(exists) {
    const auto link=read_link(path);
    if(!link || reparse_path(path) || !owned(link.value(),context_)) {
      result.diagnostic="同名启动项不属于本应用或已损坏，请在 Windows 启动设置中检查；本应用不会覆盖或删除它。";
      if(!link)for(const auto& [key,value]:link.error().safe_context)result.diagnostic+=" "+key+"="+value;
      return Result<LoginStartupSnapshot,Error>::success(std::move(result));
    }
    result.system_registered_target_path=link.value().target.toStdString();result.can_disable=true;
    if(approval.state==WindowsStartupApproval::blocked) {
      result.state=LoginStartupState::blocked;result.diagnostic="Windows 已禁用此启动项，请在 Windows 设置的“启动”设置中将 SeriousShot 设为“开”。";
    } else if(!current_target(link.value().target,QString::fromStdString(result.current_bundle_path))) {
      result.state=LoginStartupState::invalid_target;result.diagnostic="启动项仍指向旧位置；请在本次稳定应用中明确勾选修复。";
      result.can_enable=issue.empty() && (approval.state==WindowsStartupApproval::enabled || approval.state==WindowsStartupApproval::missing_no_override);
    } else if(!issue.empty()) {result.state=LoginStartupState::invalid_target;result.diagnostic=issue;}
    else if(approval.state==WindowsStartupApproval::enabled || approval.state==WindowsStartupApproval::missing_no_override) result.state=LoginStartupState::enabled;
    else {result.diagnostic="无法确认 Windows 启动覆盖记录，请检查系统启动设置后再次勾选。";}
  } else if(approval.state==WindowsStartupApproval::blocked) {
    result.state=LoginStartupState::blocked;result.diagnostic="Windows 已禁用此启动项，请在 Windows 设置的“启动”设置中将 SeriousShot 设为“开”。";
  } else if(!issue.empty()) {result.state=LoginStartupState::invalid_target;result.diagnostic=issue;}
  else if(approval.state==WindowsStartupApproval::missing_no_override || approval.state==WindowsStartupApproval::enabled) {
    result.state=LoginStartupState::disabled;result.can_enable=true;
  } else result.diagnostic="无法确认 Windows 启动状态，请检查启动设置后再次勾选。";
  if(!approval.diagnostic.empty())result.diagnostic+=" "+approval.diagnostic;
  return Result<LoginStartupSnapshot,Error>::success(std::move(result));
}
Result<bool,Error> WindowsLoginStartupPort::set_enabled(bool enabled) {
  const auto state=query();if(!state)return Result<bool,Error>::failure(state.error());
  if((enabled&&state.value().state==LoginStartupState::enabled)||(!enabled&&state.value().state==LoginStartupState::disabled))return Result<bool,Error>::success(true);
  if(!(enabled?state.value().can_enable:state.value().can_disable))return Result<bool,Error>::failure(problem(state.value().diagnostic));
  const auto directory=context_.startup_directory();if(!directory)return Result<bool,Error>::failure(directory.error());
  const auto path=directory.value()+"/"+context_.item_basename;
  const bool existed=item_exists(path);
  std::optional<Link> original;
  QByteArray original_bytes;
  if(existed) {
    const auto link=read_link(path);if(!link)return Result<bool,Error>::failure(link.error());
    if(reparse_path(path)||!owned(link.value(),context_))return Result<bool,Error>::failure(problem("启动项已改变，未修改；请重新检查。"));
    original=link.value();original_bytes=read_bounded(path);
  }
  const auto unchanged=[&] {
    if(!existed)return !item_exists(path);
    const auto check=read_link(path);
    return check && !reparse_path(path) && owned(check.value(),context_) && check.value()==*original && read_bounded(path)==original_bytes;
  };
  if(!enabled) {
    if(!unchanged())return Result<bool,Error>::failure(problem("启动项已改变，未删除；请重新检查。"));
    if(!DeleteFileW(wide(path))) {const auto code=GetLastError();return Result<bool,Error>::failure(native_error("DeleteFileW",code));}
    return Result<bool,Error>::success(true);
  }
  const auto exe=context_.current_executable();
  const auto issue=target_problem(exe);if(!issue.empty())return Result<bool,Error>::failure(problem(issue));
  const Link expected{exe,"--login-startup",QFileInfo(exe).absolutePath(),context_.ownership_marker};
  const auto temporary=directory.value()+"/SeriousShot-"+QUuid::createUuid().toString(QUuid::WithoutBraces)+".tmp";
  struct Cleanup {QString path;~Cleanup(){if(item_exists(path))DeleteFileW(wide(path));}} cleanup{temporary};
  const auto failed=[&](Error error) {
    if(item_exists(temporary) && !DeleteFileW(wide(temporary))) {
      const auto cleanup_code=GetLastError();
      error.safe_context["cleanup_operation"]="DeleteFileW(temporary)";
      error.safe_context["cleanup_native_code"]=std::to_string(cleanup_code);
    }
    return Result<bool,Error>::failure(std::move(error));
  };
  const auto written=write_link(temporary,expected);if(!written)return failed(written.error());
  const auto verified=read_link(temporary);
  if(!verified)return failed(verified.error());
  if(verified.value()!=expected || !current_target(verified.value().target,exe))return failed(problem("临时启动项回读不一致，未替换现有项。"));
  const auto approval=context_.approval();
  if((approval.state!=WindowsStartupApproval::enabled && approval.state!=WindowsStartupApproval::missing_no_override) || !unchanged())
    return failed(problem("系统状态或启动项已改变，未替换；请重新检查。"));
  if(!MoveFileExW(wide(temporary),wide(path),existed?MOVEFILE_REPLACE_EXISTING:0)) {
    const auto code=GetLastError();return failed(native_error("MoveFileExW",code));
  }
  return Result<bool,Error>::success(true);
}
Result<bool,Error> WindowsLoginStartupPort::open_system_settings() {
  const auto value=reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",L"ms-settings:startupapps",nullptr,nullptr,SW_SHOWNORMAL));
  if(value<=32)return Result<bool,Error>::failure(native_error("ShellExecuteW",static_cast<unsigned long>(value),"ShellExecute"));
  return Result<bool,Error>::success(true);
}
}
