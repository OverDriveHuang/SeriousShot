#pragma once
#include "ports/export_ports.hpp"
#include "ui/qt/qt_input_platform_adapter.hpp"
#include "ui/qt/qt_overlay_window_port.hpp"
#include "ui/qt/qt_window_activation_port.hpp"
#include "platform/windows/windows_capture.hpp"
#include <QAbstractNativeEventFilter>
#include <QPointer>

namespace hdrshot {
class WindowsExitCoordinator;
class WindowsQtWindowActivation final : public QtWindowActivationPort {
 public:
  void activate(QWidget& window) override;
};
class WindowsQtInputPlatformAdapter final : public QtInputPlatformAdapter {
 public:
  Result<QKeySequence,Error> display_global_hotkey(const std::string& hotkey) const override;
  Result<std::string,Error> canonical_global_hotkey(const QKeySequence& sequence) const override;
  QString fixed_shortcut_label(UiCommand command) const override;
  std::optional<UiCommand> fixed_overlay_command(const QKeyEvent&,FocusContext,CompletionBindings={}) const override;
};
class WindowsGlobalHotkeyPort final : public GlobalHotkeyPort, public QAbstractNativeEventFilter {
 public:
  WindowsGlobalHotkeyPort();
  ~WindowsGlobalHotkeyPort() override;
  void set_trigger_handler(HotkeyTriggerHandler handler) override;
  Result<HotkeyReceipt,Error> register_hotkey(std::string hotkey) override;
  Result<HotkeyReceipt,Error> replace_hotkey(std::string hotkey) override;
  Result<HotkeyReceipt,Error> unregister_hotkey() override;
  bool nativeEventFilter(const QByteArray&,void*,qintptr*) override;
 private:
  HotkeyTriggerHandler handler_;
  std::string current_;
  int active_id_{};
};
class WindowsQtOverlayWindow final : public QObject, public QtOverlayWindowPort {
 public:
  explicit WindowsQtOverlayWindow(WindowsDisplayInfo display);
  ~WindowsQtOverlayWindow() override;
  void prepare(QWidget& host) override;
  std::optional<Error> preparation_error() const override { return preparation_error_; }
  void configure(QWidget& host,bool order_front) override;
  void resize(QWidget& host) override;
  void set_system_dialog_active(QWidget& host,bool active,bool restore_focus) override;
  void set_input_interrupted(std::function<void()> callback) override;
  void restore_input_focus(QWidget& host) override;
  void* native_surface() const override { return surface_; }
  void* magnifier_surface() const override { return magnifier_; }
  void move_magnifier(QWidget& host, const QRect& rect) override;
  void show_magnifier() override;
  void hide_magnifier() override;
 protected:
  bool eventFilter(QObject* object,QEvent* event) override;
 private:
  struct NativeInput;
  std::unique_ptr<NativeInput> input_;
  WindowsDisplayInfo display_;
  void* surface_{};
  void* magnifier_{};
  QRect magnifier_rect_;
  std::optional<Error> preparation_error_;
  void* host_handle_{};
  bool presented_{}, system_dialog_active_{};
  std::function<void()> input_interrupted_;
};
class WindowsFileStorePort final : public FileStorePort {
 public:
  Result<bool,Error> prepare_directory(const std::string& folder) override;
  Result<FileReceipt,Error> write(const WriteFileRequest& request) override;
  Result<std::unique_ptr<AtomicFileSink>,Error> open_atomic(const OpenAtomicFileRequest& request) override;
};
class WindowsClipboardPort final : public ClipboardPort {
 public:
  explicit WindowsClipboardPort(std::string cache_directory={});
  ~WindowsClipboardPort() override;
  Result<ClipboardReceipt,Error> write(const ClipboardWriteRequest& request) override;
 private:
  std::string cache_directory_;
  void* owner_{};
};
class WindowsFileDialogPort final : public FileDialogPort {
 public:
  explicit WindowsFileDialogPort(WindowsExitCoordinator *exit=nullptr);
  Result<ChooseSavePathOutcome,Error> choose_save_path(const ChooseSavePathRequest&) override;
 private:
  QPointer<WindowsExitCoordinator> exit_;
};
class WindowsClockPort final : public ClockPort {
 public:
  LocalDateTime now_local() const override;
};
}  // namespace hdrshot
