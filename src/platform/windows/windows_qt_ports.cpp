#include "platform/windows/windows_qt_ports.hpp"
#include <windows.h>
#include <shlobj.h>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QScreen>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <QWindow>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sstream>

namespace hdrshot {
void WindowsQtWindowActivation::activate(QWidget& window) {
  window.showNormal();
  const auto hwnd=reinterpret_cast<HWND>(window.winId());
  // STARTUPINFO/SW_HIDE can override the first ShowWindow call while Qt already
  // considers its widget visible. Reconcile the actual HWND on every reopen.
  if(!IsWindowVisible(hwnd)) ShowWindow(hwnd,SW_SHOW);
  if(IsIconic(hwnd)) ShowWindow(hwnd,SW_RESTORE);
  window.raise();
  window.activateWindow();
}
namespace {
Error port_error(const char* module,const char* reason,ErrorCode code=ErrorCode::invalid_input) {
  return {code,module,Retryability::after_user_action,{{"reason",reason}}};
}
Error file_error(DWORD code) {
  const auto type=code==ERROR_FILE_EXISTS || code==ERROR_ALREADY_EXISTS ? ErrorCode::path_already_exists :
      (code==ERROR_DISK_FULL || code==ERROR_HANDLE_DISK_FULL ? ErrorCode::storage_full : ErrorCode::path_not_writable);
  return {type,"WindowsFileStore",Retryability::after_user_action,{{"win32",std::to_string(code)}}};
}
std::wstring wide(const std::string& value) { return QString::fromUtf8(value).toStdWString(); }
class WindowsAtomicSink final : public AtomicFileSink {
 public:
  WindowsAtomicSink(HANDLE handle,std::wstring pending,std::string path,bool overwrite)
      :handle_(handle),pending_(std::move(pending)),path_(std::move(path)),overwrite_(overwrite){}
  ~WindowsAtomicSink() override { abort(); }
  Result<std::size_t,Error> write(std::span<const std::uint8_t> bytes) override {
    if(handle_==INVALID_HANDLE_VALUE) return Result<std::size_t,Error>::failure(file_error(ERROR_INVALID_HANDLE));
    if(bytes.empty()) return Result<std::size_t,Error>::success(0);
    const DWORD requested=static_cast<DWORD>(std::min<std::size_t>(bytes.size(),std::numeric_limits<DWORD>::max()));
    DWORD written=0;
    if(!WriteFile(handle_,bytes.data(),requested,&written,nullptr) || written==0) return Result<std::size_t,Error>::failure(file_error(GetLastError()));
    bytes_+=written;
    return Result<std::size_t,Error>::success(written);
  }
  Result<FileReceipt,Error> commit() override {
    if(handle_==INVALID_HANDLE_VALUE || bytes_==0) return Result<FileReceipt,Error>::failure(file_error(ERROR_INVALID_HANDLE));
    if(!FlushFileBuffers(handle_)) { auto e=file_error(GetLastError()); abort(); return Result<FileReceipt,Error>::failure(e); }
    CloseHandle(handle_); handle_=INVALID_HANDLE_VALUE;
    if(!MoveFileExW(pending_.c_str(),wide(path_).c_str(),MOVEFILE_WRITE_THROUGH | (overwrite_?MOVEFILE_REPLACE_EXISTING:0))) {
      auto e=file_error(GetLastError()); abort(); return Result<FileReceipt,Error>::failure(e);
    }
    pending_.clear();
    return Result<FileReceipt,Error>::success({path_,bytes_});
  }
  void abort() noexcept override {
    if(handle_!=INVALID_HANDLE_VALUE) { CloseHandle(handle_); handle_=INVALID_HANDLE_VALUE; }
    if(!pending_.empty()) { DeleteFileW(pending_.c_str()); pending_.clear(); }
  }
 private:
  HANDLE handle_{INVALID_HANDLE_VALUE};
  std::wstring pending_;
  std::string path_;
  bool overwrite_{};
  std::size_t bytes_{};
};
}
Result<QKeySequence,Error> WindowsQtInputPlatformAdapter::display_global_hotkey(const std::string& hotkey) const {
  Qt::KeyboardModifiers mods{}; Qt::Key key=Qt::Key_unknown;
  std::stringstream stream(hotkey); std::string token;
  while(std::getline(stream,token,'+')) {
    if(token=="Control") mods|=Qt::ControlModifier;
    else if(token=="Alt") mods|=Qt::AltModifier;
    else if(token=="Shift") mods|=Qt::ShiftModifier;
    else if(token=="Windows") mods|=Qt::MetaModifier;
    else if(token.size()==1 && key==Qt::Key_unknown &&
        ((token[0]>='A' && token[0]<='Z') || (token[0]>='0' && token[0]<='9'))) key=static_cast<Qt::Key>(token[0]);
    else return Result<QKeySequence,Error>::failure(port_error("WindowsInput","invalid_hotkey"));
  }
  if(mods==Qt::NoModifier || key==Qt::Key_unknown) return Result<QKeySequence,Error>::failure(port_error("WindowsInput","modifier_and_key_required"));
  return Result<QKeySequence,Error>::success(QKeySequence(QKeyCombination(mods,key)));
}
Result<std::string,Error> WindowsQtInputPlatformAdapter::canonical_global_hotkey(const QKeySequence& sequence) const {
  if(sequence.count()!=1) return Result<std::string,Error>::failure(port_error("WindowsInput","one_key_combination_required"));
  const auto combo=sequence[0]; const auto key=combo.key(); const auto mods=combo.keyboardModifiers();
  if(!((key>=Qt::Key_A && key<=Qt::Key_Z) || (key>=Qt::Key_0 && key<=Qt::Key_9)) || mods==Qt::NoModifier)
    return Result<std::string,Error>::failure(port_error("WindowsInput","unsupported_hotkey"));
  std::string result;
  if(mods.testFlag(Qt::MetaModifier)) result+="Windows+";
  if(mods.testFlag(Qt::ControlModifier)) result+="Control+";
  if(mods.testFlag(Qt::AltModifier)) result+="Alt+";
  if(mods.testFlag(Qt::ShiftModifier)) result+="Shift+";
  result+=static_cast<char>(key);
  return Result<std::string,Error>::success(std::move(result));
}
QString WindowsQtInputPlatformAdapter::fixed_shortcut_label(UiCommand command) const {
  switch(command) {
    case UiCommand::cancel_capture:return QStringLiteral("Esc");
    case UiCommand::copy_and_close:return QStringLiteral("Enter / 双击选区");
    case UiCommand::save_default:return QStringLiteral("Ctrl+S");
    case UiCommand::save_as:return QStringLiteral("Ctrl+Alt+S");
    case UiCommand::undo:return QStringLiteral("Ctrl+Z");
    case UiCommand::redo:return QStringLiteral("Ctrl+Y");
    default:return {};
  }
}
std::optional<UiCommand> WindowsQtInputPlatformAdapter::fixed_overlay_command(const QKeyEvent& event,FocusContext focus,CompletionBindings bindings) const {
  Key key=Key::unknown;
  switch(event.key()) {
    case Qt::Key_Escape:key=Key::escape;break;
    case Qt::Key_Enter:case Qt::Key_Return:key=Key::enter;break;
    case Qt::Key_S:key=Key::s;break;
    case Qt::Key_Y:key=Key::y;break;
    case Qt::Key_Z:key=Key::z;break;
    default:break;
  }
  const auto mods=event.modifiers();
  if(mods.testFlag(Qt::MetaModifier)) return std::nullopt;
  return InputMapper::map({focus,key,{mods.testFlag(Qt::ControlModifier),mods.testFlag(Qt::AltModifier),mods.testFlag(Qt::ShiftModifier)}},bindings);
}
WindowsGlobalHotkeyPort::WindowsGlobalHotkeyPort() { QCoreApplication::instance()->installNativeEventFilter(this); }
WindowsGlobalHotkeyPort::~WindowsGlobalHotkeyPort() { (void)unregister_hotkey(); QCoreApplication::instance()->removeNativeEventFilter(this); }
void WindowsGlobalHotkeyPort::set_trigger_handler(HotkeyTriggerHandler handler) { handler_=std::move(handler); }
Result<HotkeyReceipt,Error> WindowsGlobalHotkeyPort::register_hotkey(std::string hotkey) { return replace_hotkey(std::move(hotkey)); }
Result<HotkeyReceipt,Error> WindowsGlobalHotkeyPort::replace_hotkey(std::string hotkey) {
  if(hotkey==current_) return Result<HotkeyReceipt,Error>::success({current_});
  const auto parsed=WindowsQtInputPlatformAdapter{}.display_global_hotkey(hotkey);
  if(!parsed) return Result<HotkeyReceipt,Error>::failure(parsed.error());
  const auto combo=parsed.value()[0]; const auto mods=combo.keyboardModifiers();
  UINT native=MOD_NOREPEAT;
  if(mods.testFlag(Qt::ControlModifier)) native|=MOD_CONTROL;
  if(mods.testFlag(Qt::AltModifier)) native|=MOD_ALT;
  if(mods.testFlag(Qt::ShiftModifier)) native|=MOD_SHIFT;
  if(mods.testFlag(Qt::MetaModifier)) native|=MOD_WIN;
  const int candidate=active_id_==0x5301 ? 0x5302:0x5301;
  if(!RegisterHotKey(nullptr,candidate,native,static_cast<UINT>(combo.key())))
    return Result<HotkeyReceipt,Error>::failure(port_error("WindowsHotkey","registration_failed",ErrorCode::hotkey_conflict));
  if(active_id_) UnregisterHotKey(nullptr,active_id_);
  active_id_=candidate; current_=std::move(hotkey);
  return Result<HotkeyReceipt,Error>::success({current_});
}
Result<HotkeyReceipt,Error> WindowsGlobalHotkeyPort::unregister_hotkey() {
  if(active_id_) UnregisterHotKey(nullptr,active_id_);
  active_id_=0; current_.clear(); return Result<HotkeyReceipt,Error>::success({});
}
bool WindowsGlobalHotkeyPort::nativeEventFilter(const QByteArray&,void* message,qintptr* result) {
  const auto* msg=static_cast<MSG*>(message);
  if(msg->message==WM_HOTKEY && msg->wParam==static_cast<WPARAM>(active_id_) && active_id_) {
    if(handler_) handler_(); if(result) *result=0; return true;
  }
  return false;
}
struct WindowsQtOverlayWindow::NativeInput {
  QPointer<QWidget> host;
  QPointer<QWidget> grabbed;
  void update_cursor(QWidget* target) const {
    const auto* override_cursor=QApplication::overrideCursor();
    const auto shape=override_cursor ? override_cursor->shape() : target->cursor().shape();
    LPCWSTR resource=IDC_ARROW;
    switch(shape) {
      case Qt::CrossCursor: resource=IDC_CROSS;break;
      case Qt::IBeamCursor: resource=IDC_IBEAM;break;
      case Qt::SizeVerCursor: resource=IDC_SIZENS;break;
      case Qt::SizeHorCursor: resource=IDC_SIZEWE;break;
      case Qt::SizeFDiagCursor: resource=IDC_SIZENWSE;break;
      case Qt::SizeBDiagCursor: resource=IDC_SIZENESW;break;
      case Qt::SizeAllCursor: case Qt::OpenHandCursor: case Qt::ClosedHandCursor: resource=IDC_SIZEALL;break;
      case Qt::PointingHandCursor: resource=IDC_HAND;break;
      case Qt::ForbiddenCursor: resource=IDC_NO;break;
      case Qt::WaitCursor: resource=IDC_WAIT;break;
      case Qt::BusyCursor: resource=IDC_APPSTARTING;break;
      case Qt::WhatsThisCursor: resource=IDC_HELP;break;
      case Qt::BlankCursor: SetCursor(nullptr);return;
      default: break;
    }
    SetCursor(LoadCursorW(nullptr,resource));
  }
  static LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
    if(message==WM_NCCREATE) {
      const auto* create=reinterpret_cast<CREATESTRUCTW*>(lparam);
      SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self=reinterpret_cast<NativeInput*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(self && self->host && message==WM_SETCURSOR && LOWORD(lparam)==HTCLIENT) {
      POINT point{};GetCursorPos(&point);ScreenToClient(hwnd,&point);
      const auto scale=self->host->devicePixelRatioF();
      auto* target=self->grabbed ? self->grabbed.data() : self->host->childAt(QPoint(
          qRound(point.x/scale),qRound(point.y/scale)));
      self->update_cursor(target ? target:self->host.data());
      return TRUE;
    }
    if(self && self->host && (message==WM_MOUSEMOVE || message==WM_LBUTTONDOWN ||
        message==WM_LBUTTONUP || message==WM_LBUTTONDBLCLK || message==WM_RBUTTONDOWN || message==WM_RBUTTONUP)) {
      // Windows skips alpha-zero parts of the Qt layered HWND. The opaque
      // FP16 underlay receives those mouse events and routes them to the same
      // Qt child widgets; it contains no selection/editor policy.
      auto* host=self->host.data();
      const auto scale=host->devicePixelRatioF();
      const QPointF local(static_cast<short>(LOWORD(lparam))/scale,static_cast<short>(HIWORD(lparam))/scale);
      auto* target=self->grabbed ? self->grabbed.data() : host->childAt(local.toPoint());
      if(!target) target=host;
      const bool down=message==WM_LBUTTONDOWN || message==WM_RBUTTONDOWN || message==WM_LBUTTONDBLCLK;
      const bool up=message==WM_LBUTTONUP || message==WM_RBUTTONUP;
      const auto button=message==WM_MOUSEMOVE ? Qt::NoButton :
          (message==WM_RBUTTONDOWN || message==WM_RBUTTONUP ? Qt::RightButton:Qt::LeftButton);
      Qt::MouseButtons buttons;
      if(wparam&MK_LBUTTON) buttons|=Qt::LeftButton;
      if(wparam&MK_RBUTTON) buttons|=Qt::RightButton;
      Qt::KeyboardModifiers modifiers;
      if(wparam&MK_SHIFT) modifiers|=Qt::ShiftModifier;
      if(wparam&MK_CONTROL) modifiers|=Qt::ControlModifier;
      if(GetKeyState(VK_MENU)&0x8000) modifiers|=Qt::AltModifier;
      if(down) {self->grabbed=target;SetCapture(hwnd);host->activateWindow();target->setFocus(Qt::MouseFocusReason);}
      const auto type=message==WM_LBUTTONDBLCLK ? QEvent::MouseButtonDblClick :
          (down ? QEvent::MouseButtonPress : (up ? QEvent::MouseButtonRelease:QEvent::MouseMove));
      const auto global=QPointF(host->mapToGlobal(local.toPoint()));
      QMouseEvent event(type,QPointF(target->mapFromGlobal(global.toPoint())),global,button,buttons,modifiers);
      QApplication::sendEvent(target,&event);
      self->update_cursor(target);
      if(up && buttons==Qt::NoButton) {self->grabbed=nullptr;ReleaseCapture();}
      return 0;
    }
    return DefWindowProcW(hwnd,message,wparam,lparam);
  }
};
WindowsQtOverlayWindow::WindowsQtOverlayWindow(WindowsDisplayInfo display)
    :input_(std::make_unique<NativeInput>()),display_(std::move(display)){}
void WindowsQtOverlayWindow::prepare(QWidget& host) {
  // A separate native underlay keeps Qt's SDR backing store from repainting
  // the authoritative HDR image. Qt retains all editor/input business logic.
  host.setAttribute(Qt::WA_TranslucentBackground);
  host.setAttribute(Qt::WA_OpaquePaintEvent,false);
  host_handle_=reinterpret_cast<void*>(host.winId());
  input_->host=&host;
  WNDCLASSW klass{};klass.hInstance=GetModuleHandleW(nullptr);klass.lpfnWndProc=NativeInput::procedure;
  klass.lpszClassName=L"SeriousShotHdrUnderlay";klass.style=CS_DBLCLKS;klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);
  RegisterClassW(&klass);
  surface_=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOREDIRECTIONBITMAP,
      klass.lpszClassName,L"SeriousShot HDR surface",WS_POPUP,display_.physical_x,display_.physical_y,
      display_.snapshot.capture_size_px.width,display_.snapshot.capture_size_px.height,
      nullptr,nullptr,GetModuleHandleW(nullptr),input_.get());
  if(!surface_) return;
  SetWindowLongPtrW(static_cast<HWND>(host_handle_),GWLP_HWNDPARENT,reinterpret_cast<LONG_PTR>(surface_));
  SetWindowDisplayAffinity(static_cast<HWND>(surface_),WDA_EXCLUDEFROMCAPTURE);
  SetWindowDisplayAffinity(static_cast<HWND>(host_handle_),WDA_EXCLUDEFROMCAPTURE);
  host.installEventFilter(this);
  configure(host,false);
}
void WindowsQtOverlayWindow::configure(QWidget& host,bool order_front) {
  if(!surface_) return;
  SetWindowPos(static_cast<HWND>(surface_),HWND_TOPMOST,display_.physical_x,display_.physical_y,
      display_.snapshot.capture_size_px.width,display_.snapshot.capture_size_px.height,
      SWP_NOACTIVATE | (order_front?SWP_SHOWWINDOW:0));
  if(order_front) {
    host.show();
    SetWindowPos(static_cast<HWND>(host_handle_),HWND_TOPMOST,display_.physical_x,display_.physical_y,
        display_.snapshot.capture_size_px.width,display_.snapshot.capture_size_px.height,SWP_SHOWWINDOW);
    host.raise(); host.activateWindow(); SetForegroundWindow(static_cast<HWND>(host_handle_));
  }
}
void WindowsQtOverlayWindow::resize(QWidget&) {}
WindowsQtOverlayWindow::~WindowsQtOverlayWindow() { if(surface_) DestroyWindow(static_cast<HWND>(surface_)); }
bool WindowsQtOverlayWindow::eventFilter(QObject*,QEvent* event) {
  if(surface_ && event->type()==QEvent::Hide) ShowWindow(static_cast<HWND>(surface_),SW_HIDE);
  return false;
}
void WindowsQtOverlayWindow::set_system_dialog_active(QWidget& host,bool active,bool restore_focus) {
  if(!surface_) return;
  SetWindowPos(static_cast<HWND>(surface_),active?HWND_NOTOPMOST:HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
  SetWindowPos(static_cast<HWND>(host_handle_),active?HWND_NOTOPMOST:HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
  if(restore_focus) { host.raise(); host.activateWindow(); SetForegroundWindow(static_cast<HWND>(host_handle_)); }
}
Result<bool,Error> WindowsFileStorePort::prepare_directory(const std::string& folder) {
  if(folder.empty()) return Result<bool,Error>::failure(file_error(ERROR_PATH_NOT_FOUND));
  std::error_code ec;
  const auto path=std::filesystem::u8path(folder);
  std::filesystem::create_directories(path,ec);
  if(ec || !std::filesystem::is_directory(path,ec)) return Result<bool,Error>::failure(file_error(ERROR_PATH_NOT_FOUND));
  return Result<bool,Error>::success(true);
}
Result<std::unique_ptr<AtomicFileSink>,Error> WindowsFileStorePort::open_atomic(const OpenAtomicFileRequest& request) {
  if(request.exact_path.empty()) return Result<std::unique_ptr<AtomicFileSink>,Error>::failure(file_error(ERROR_PATH_NOT_FOUND));
  const auto pending=wide(request.exact_path)+L"."+QUuid::createUuid().toString(QUuid::Id128).toStdWString()+L".pending";
  const auto handle=CreateFileW(pending.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(handle==INVALID_HANDLE_VALUE) return Result<std::unique_ptr<AtomicFileSink>,Error>::failure(file_error(GetLastError()));
  return Result<std::unique_ptr<AtomicFileSink>,Error>::success(std::make_unique<WindowsAtomicSink>(handle,pending,request.exact_path,request.overwrite));
}
Result<FileReceipt,Error> WindowsFileStorePort::write(const WriteFileRequest& request) {
  if(request.bytes.empty()) return Result<FileReceipt,Error>::failure(file_error(ERROR_INVALID_DATA));
  auto sink=open_atomic({request.exact_path,request.overwrite});
  if(!sink) return Result<FileReceipt,Error>::failure(sink.error());
  auto remaining=request.bytes;
  while(!remaining.empty()) {
    const auto written=sink.value()->write(remaining);
    if(!written) return Result<FileReceipt,Error>::failure(written.error());
    remaining=remaining.subspan(written.value());
  }
  return sink.value()->commit();
}
WindowsClipboardPort::WindowsClipboardPort(std::string cache_directory)
    :cache_directory_(std::move(cache_directory)) {}
WindowsClipboardPort::~WindowsClipboardPort() { if(owner_) DestroyWindow(static_cast<HWND>(owner_)); }
Result<ClipboardReceipt,Error> WindowsClipboardPort::write(const ClipboardWriteRequest& request) {
  const bool png=std::find(request.mime_types.begin(),request.mime_types.end(),"image/png")!=request.mime_types.end();
  const bool jpeg=std::find(request.mime_types.begin(),request.mime_types.end(),"image/jpeg")!=request.mime_types.end();
  if(request.bytes.empty() || png==jpeg) return Result<ClipboardReceipt,Error>::failure(port_error("WindowsClipboard","unsupported_mime",ErrorCode::unsupported_encoding));
  if(!owner_) owner_=CreateWindowExW(0,L"STATIC",L"SeriousShot Clipboard",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
  if(!owner_) return Result<ClipboardReceipt,Error>::failure(port_error("WindowsClipboard","owner_failed",ErrorCode::clipboard_rejected));
  // File-aware consumers receive the same encoded original as image-aware
  // consumers. Never decode/re-encode pixels or publish an SDR bitmap here.
  // Each clipboard file is immutable and survives app exit / later copies:
  // clipboard history and consumers may still reference an earlier file.
  const auto root=cache_directory_.empty()
      ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation)+QStringLiteral("/clipboard")
      : QString::fromStdString(cache_directory_);
  if(!QDir().mkpath(root)) return Result<ClipboardReceipt,Error>::failure(port_error("WindowsClipboard","cache_directory_failed",ErrorCode::clipboard_rejected));
  const auto path=QDir(root).absoluteFilePath(QStringLiteral("SeriousShot_")+
      QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss_"))+
      QUuid::createUuid().toString(QUuid::WithoutBraces)+(png?QStringLiteral(".png"):QStringLiteral(".jpg")));
  QSaveFile file(path);
  if(request.bytes.size()>static_cast<std::size_t>(std::numeric_limits<qint64>::max()) ||
      !file.open(QIODevice::WriteOnly) ||
      file.write(reinterpret_cast<const char*>(request.bytes.data()),static_cast<qint64>(request.bytes.size()))!=static_cast<qint64>(request.bytes.size()) || !file.commit())
    return Result<ClipboardReceipt,Error>::failure(port_error("WindowsClipboard","cache_write_failed",ErrorCode::clipboard_rejected));
  const auto failure=[&](const char* reason) {
    QFile::remove(path);
    return Result<ClipboardReceipt,Error>::failure(port_error("WindowsClipboard",reason,ErrorCode::clipboard_rejected));
  };
  auto native_path=QDir::toNativeSeparators(path).toStdWString();
  native_path.append(2,L'\0');
  DROPFILES header{};header.pFiles=sizeof(header);header.fWide=TRUE;
  std::vector<std::uint8_t> drop(sizeof(header)+native_path.size()*sizeof(wchar_t));
  std::memcpy(drop.data(),&header,sizeof(header));
  std::memcpy(drop.data()+sizeof(header),native_path.data(),native_path.size()*sizeof(wchar_t));
  const auto allocate=[](const void* bytes,std::size_t length) {
    std::unique_ptr<void,decltype(&GlobalFree)> memory(GlobalAlloc(GMEM_MOVEABLE,length),&GlobalFree);
    if(memory) {
      if(auto* data=GlobalLock(memory.get())) {
        std::memcpy(data,bytes,length);GlobalUnlock(memory.get());
      } else memory.reset();
    }
    return memory;
  };
  const auto format=RegisterClipboardFormatW(png?L"PNG":L"JFIF");
  const auto effect_format=RegisterClipboardFormatW(L"Preferred DropEffect");
  const DWORD effect=DROPEFFECT_COPY;
  auto original=allocate(request.bytes.data(),request.bytes.size());
  auto file_drop=allocate(drop.data(),drop.size());
  auto copy_effect=allocate(&effect,sizeof(effect));
  if(!format || !effect_format || !original || !file_drop || !copy_effect) return failure("allocation_failed");
  if(!OpenClipboard(static_cast<HWND>(owner_))) return failure("busy");
  bool published=EmptyClipboard() && SetClipboardData(format,original.get())!=nullptr;
  bool file_published=false;
  if(published) {
    (void)original.release();
    file_published=SetClipboardData(CF_HDROP,file_drop.get())!=nullptr;
    published=file_published;
    if(file_published) {
      (void)file_drop.release();
      published=SetClipboardData(effect_format,copy_effect.get())!=nullptr;
      if(published) (void)copy_effect.release();
    }
  }
  CloseClipboard();
  if(!published) {
    if(!file_published) return failure("publish_failed");
    // CF_HDROP already references the file: do not invalidate it on a later failure.
    return Result<ClipboardReceipt,Error>::failure(port_error("WindowsClipboard","copy_effect_failed",ErrorCode::clipboard_rejected));
  }
  return Result<ClipboardReceipt,Error>::success({request.bytes.size(),{png?"image/png":"image/jpeg","text/uri-list"}});
}
Result<ChooseSavePathOutcome,Error> WindowsFileDialogPort::choose_save_path(const ChooseSavePathRequest& request) {
  QFileDialog dialog(QApplication::activeWindow(),QStringLiteral("保存截图"),QString::fromUtf8(request.initial_folder));
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  const bool png=request.save_format==SaveFormat::png_display_p3_dual_range;
  dialog.setNameFilter(png?QStringLiteral("PNG 图片 (*.png)"):QStringLiteral("JPEG 图片 (*.jpg)"));
  dialog.setDefaultSuffix(png?QStringLiteral("png"):QStringLiteral("jpg"));
  dialog.selectFile(QString::fromUtf8(request.suggested_name));
  if(dialog.exec()!=QDialog::Accepted || dialog.selectedFiles().isEmpty()) return Result<ChooseSavePathOutcome,Error>::success(UserCancelled{});
  return Result<ChooseSavePathOutcome,Error>::success(ChosenPath{dialog.selectedFiles().front().toUtf8().toStdString()});
}
LocalDateTime WindowsClockPort::now_local() const {
  const auto now=QDateTime::currentDateTime(); const auto d=now.date(); const auto t=now.time();
  return {d.year(),static_cast<std::uint8_t>(d.month()),static_cast<std::uint8_t>(d.day()),
      static_cast<std::uint8_t>(t.hour()),static_cast<std::uint8_t>(t.minute()),static_cast<std::uint8_t>(t.second()),static_cast<std::int16_t>(now.offsetFromUtc()/60)};
}
}  // namespace hdrshot
