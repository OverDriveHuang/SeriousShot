#include "platform/windows/windows_qt_ports.hpp"
#include "test_support.hpp"
#include <QApplication>
#include <QFile>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QDir>
#include <QCryptographicHash>
#include <QProcess>
#include <QThread>
#include <cstring>
#include <iostream>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
using namespace hdrshot;
namespace {
QByteArray read_file(const QString& path) {
  QFile file(path);HDRSHOT_CHECK(file.open(QIODevice::ReadOnly));return file.readAll();
}
QString check_clipboard_original(const QString& source) {
  const auto expected=read_file(source);
  const auto format=RegisterClipboardFormatW(source.endsWith(QStringLiteral(".png"),Qt::CaseInsensitive)?L"PNG":L"JFIF");
  bool opened=false;
  for(int attempt=0;attempt<20 && !opened;++attempt) {
    opened=OpenClipboard(nullptr)!=FALSE;if(!opened) QThread::msleep(25);
  }
  HDRSHOT_CHECK(opened);
  struct Close {~Close(){CloseClipboard();}} close;
  const auto raw=GetClipboardData(format);HDRSHOT_CHECK(raw);
  HDRSHOT_CHECK(GlobalSize(raw)>=static_cast<SIZE_T>(expected.size()));
  const auto* bytes=GlobalLock(raw);HDRSHOT_CHECK(bytes);
  const bool equal=std::memcmp(bytes,expected.data(),static_cast<std::size_t>(expected.size()))==0;
  GlobalUnlock(raw);HDRSHOT_CHECK(equal);
  HDRSHOT_CHECK(!IsClipboardFormatAvailable(CF_DIB) && !IsClipboardFormatAvailable(CF_BITMAP));
  const auto drop=static_cast<HDROP>(GetClipboardData(CF_HDROP));HDRSHOT_CHECK(drop);
  HDRSHOT_CHECK(DragQueryFileW(drop,0xffffffff,nullptr,0)==1);
  const auto count=DragQueryFileW(drop,0,nullptr,0);
  std::wstring filename(count+1,L'\0');
  HDRSHOT_CHECK(DragQueryFileW(drop,0,filename.data(),count+1)==count);
  const auto path=QString::fromWCharArray(filename.data());
  HDRSHOT_CHECK(read_file(path)==expected);
  const auto effect=GetClipboardData(RegisterClipboardFormatW(L"Preferred DropEffect"));HDRSHOT_CHECK(effect);
  const auto* effect_bytes=static_cast<const DWORD*>(GlobalLock(effect));HDRSHOT_CHECK(effect_bytes);
  const auto copy_effect=*effect_bytes;GlobalUnlock(effect);HDRSHOT_CHECK(copy_effect==DROPEFFECT_COPY);
  std::cout<<"rawOriginalMatches=1 fileOriginalMatches=1 bitmapAbsent=1 sha256="
      <<QCryptographicHash::hash(expected,QCryptographicHash::Sha256).toHex().constData()
      <<" path="<<path.toUtf8().constData()<<'\n';
  return path;
}
// Explicit manual test only: ordinary CTest must not overwrite the user's clipboard.
void clipboard_file_probe(const QString& source,const QString& cache) {
  const auto bytes=read_file(source);
  const std::string mime=source.endsWith(QStringLiteral(".png"),Qt::CaseInsensitive)?"image/png":"image/jpeg";
  const ClipboardWriteRequest request{std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(bytes.constData()),static_cast<std::size_t>(bytes.size())),{mime}};
  QString first_path;
  {
    WindowsClipboardPort clipboard(cache.toStdString());
    HDRSHOT_CHECK(clipboard.write(request).has_value());
    first_path=check_clipboard_original(source);
    // A cache failure must leave the current clipboard intact.
    WindowsClipboardPort invalid_cache(source.toStdString());
    const auto sequence=GetClipboardSequenceNumber();
    HDRSHOT_CHECK(!invalid_cache.write(request));
    HDRSHOT_CHECK(GetClipboardSequenceNumber()==sequence);
    HDRSHOT_CHECK(clipboard.write(request).has_value());
    const auto second_path=check_clipboard_original(source);
    HDRSHOT_CHECK(second_path!=first_path && read_file(first_path)==bytes);
  }
  // Read from another process after the clipboard owner has been destroyed.
  QProcess reader;reader.start(QCoreApplication::applicationFilePath(),{QStringLiteral("--clipboard-file-read"),source});
  HDRSHOT_CHECK(reader.waitForFinished(10000));
  std::cout<<reader.readAllStandardOutput().constData();
  HDRSHOT_CHECK(reader.exitStatus()==QProcess::NormalExit && reader.exitCode()==0);
  HDRSHOT_CHECK(read_file(first_path)==bytes);
}
void settings_activation_restores_native_visibility() {
  QWidget window;
  WindowsQtWindowActivation activation;
  const auto check=[&] {
    activation.activate(window);
    const auto hwnd=reinterpret_cast<HWND>(window.winId());
    HDRSHOT_CHECK(window.isVisible());
    HDRSHOT_CHECK(IsWindowVisible(hwnd));
    HDRSHOT_CHECK(!window.isMinimized() && !IsIconic(hwnd));
  };
  // Keep this first in main: launching this test process with STARTUPINFO
  // SW_HIDE exercises Qt-visible/native-hidden on its very first window.
  check();
  window.close();check();
  window.showMinimized();window.hide();check();
}
void key_contract() {
  WindowsQtInputPlatformAdapter keys;
  for(const auto* text:{"Control+Shift+2","Control+Alt+S","Windows+Shift+A"}) {
    const auto sequence=keys.display_global_hotkey(text);
    HDRSHOT_CHECK(sequence.has_value());
    HDRSHOT_CHECK(keys.canonical_global_hotkey(sequence.value()).value()==text);
  }
  HDRSHOT_CHECK(!keys.display_global_hotkey("Command+Shift+2"));
  HDRSHOT_CHECK(!keys.display_global_hotkey("S"));
  QKeyEvent save(QEvent::KeyPress,Qt::Key_S,Qt::ControlModifier);
  HDRSHOT_CHECK(keys.fixed_overlay_command(save,FocusContext::overlay)==UiCommand::save_default);
  QKeyEvent save_as(QEvent::KeyPress,Qt::Key_S,Qt::ControlModifier|Qt::AltModifier);
  HDRSHOT_CHECK(keys.fixed_overlay_command(save_as,FocusContext::overlay)==UiCommand::save_as);
}
void atomic_create_collision_and_abort() {
  QTemporaryDir temporary;
  HDRSHOT_CHECK(temporary.isValid());
  const auto filename=temporary.path()+QStringLiteral("/中文路径.png");
  WindowsFileStorePort files;
  const std::vector<std::uint8_t> bytes{0x50,0x4e,0x47,0xff};
  auto sink=files.open_atomic({filename.toStdString(),false});
  HDRSHOT_CHECK(sink.has_value());
  HDRSHOT_CHECK(sink.value()->write(bytes).has_value());
  HDRSHOT_CHECK(!QFile::exists(filename));
  HDRSHOT_CHECK(sink.value()->commit().has_value());
  QFile read(filename);HDRSHOT_CHECK(read.open(QIODevice::ReadOnly));
  HDRSHOT_CHECK(read.readAll()==QByteArray(reinterpret_cast<const char*>(bytes.data()),static_cast<qsizetype>(bytes.size())));
  read.close();
  const std::vector<std::uint8_t> other{1,2,3};
  auto collision=files.write({other,filename.toStdString(),false});
  HDRSHOT_CHECK(!collision && collision.error().code==ErrorCode::path_already_exists);
  HDRSHOT_CHECK(files.write({other,filename.toStdString(),true}).has_value());
  auto aborted=files.open_atomic({(filename+QStringLiteral(".aborted")).toStdString(),false});
  HDRSHOT_CHECK(aborted.has_value());
  HDRSHOT_CHECK(aborted.value()->write(bytes).has_value());
  aborted.value().reset();
  HDRSHOT_CHECK(QDir(temporary.path()).entryList(QDir::Files).size()==1);
}
void atomic_racing_creators() {
  QTemporaryDir temporary;
  const auto filename=(temporary.path()+QStringLiteral("/race.png")).toStdString();
  WindowsFileStorePort files;
  auto a=files.open_atomic({filename,false}),b=files.open_atomic({filename,false});
  const std::vector<std::uint8_t> bytes{1};
  HDRSHOT_CHECK(a.value()->write(bytes).has_value());
  HDRSHOT_CHECK(b.value()->write(bytes).has_value());
  HDRSHOT_CHECK(a.value()->commit().has_value());
  const auto result=b.value()->commit();
  HDRSHOT_CHECK(!result && result.error().code==ErrorCode::path_already_exists);
}
void native_underlay_follows_host_lifetime() {
  WindowsDisplayInfo display;
  display.snapshot.capture_size_px={200,100};
  QWidget host;
  WindowsQtOverlayWindow window(display);
  window.prepare(host);
  auto hwnd=static_cast<HWND>(window.native_surface());
  HDRSHOT_CHECK(hwnd && IsWindow(hwnd));
  HDRSHOT_CHECK(hwnd!=reinterpret_cast<HWND>(host.winId()));
  DWORD affinity=0;
  HDRSHOT_CHECK(GetWindowDisplayAffinity(hwnd,&affinity));
  HDRSHOT_CHECK(affinity==WDA_EXCLUDEFROMCAPTURE);
}
void native_underlay_routes_to_qt_child() {
  class Receiver final:public QWidget {
   public:
    using QWidget::QWidget;
    int presses=0,releases=0;
    QPointF position;
    void mousePressEvent(QMouseEvent* e) override {++presses;position=e->position();}
    void mouseReleaseEvent(QMouseEvent*) override {++releases;}
  };
  WindowsDisplayInfo display;display.snapshot.capture_size_px={200,100};
  QWidget host;host.resize(200,100);
  Receiver child(&host);child.setGeometry(20,10,100,60);child.show();
  WindowsQtOverlayWindow window(display);window.prepare(host);window.configure(host,true);
  const auto hwnd=static_cast<HWND>(window.native_surface());
  const auto scale=host.devicePixelRatioF();
  const auto point=MAKELPARAM(static_cast<int>(40*scale),static_cast<int>(30*scale));
  SendMessageW(hwnd,WM_LBUTTONDOWN,MK_LBUTTON,point);
  SendMessageW(hwnd,WM_LBUTTONUP,0,point);
  HDRSHOT_CHECK(child.presses==1 && child.releases==1);
  HDRSHOT_CHECK_NEAR(child.position.x(),20,0.01);
  HDRSHOT_CHECK_NEAR(child.position.y(),20,0.01);
  for(const auto& [shape,system_cursor]:std::vector<std::pair<Qt::CursorShape,LPCWSTR>>{
      {Qt::ArrowCursor,IDC_ARROW},{Qt::CrossCursor,IDC_CROSS},{Qt::IBeamCursor,IDC_IBEAM},
      {Qt::SizeVerCursor,IDC_SIZENS},{Qt::SizeHorCursor,IDC_SIZEWE},
      {Qt::SizeFDiagCursor,IDC_SIZENWSE},{Qt::SizeBDiagCursor,IDC_SIZENESW},
      {Qt::SizeAllCursor,IDC_SIZEALL},{Qt::PointingHandCursor,IDC_HAND}}) {
    child.setCursor(shape);
    SendMessageW(hwnd,WM_MOUSEMOVE,0,point);
    HDRSHOT_CHECK(GetCursor()==LoadCursorW(nullptr,system_cursor));
  }
  child.unsetCursor();SendMessageW(hwnd,WM_MOUSEMOVE,0,point);
  HDRSHOT_CHECK(GetCursor()==LoadCursorW(nullptr,IDC_ARROW));
  host.hide();HDRSHOT_CHECK(!IsWindowVisible(hwnd));
}
}
int main(int argc,char** argv) {
  QApplication app(argc,argv);
  if(argc==3 && std::string(argv[1])=="--clipboard-file-read")
    return hdrshot::test::run({{"independent clipboard original reader",[&]{(void)check_clipboard_original(QString::fromLocal8Bit(argv[2]));}}});
  if(argc==4 && std::string(argv[1])=="--clipboard-file-probe")
    return hdrshot::test::run({{"original file clipboard lifetime",[&]{clipboard_file_probe(QString::fromLocal8Bit(argv[2]),QString::fromLocal8Bit(argv[3]));}}});
  return hdrshot::test::run({{"settings native visibility on first open and reopen",settings_activation_restores_native_visibility},
      {"Windows modifier contract",key_contract},
      {"atomic Unicode save collision and abort",atomic_create_collision_and_abort},
      {"two creators never overwrite",atomic_racing_creators},
      {"separate HDR underlay",native_underlay_follows_host_lifetime},
      {"native transparent-area input reaches Qt child",native_underlay_routes_to_qt_child}});
}
