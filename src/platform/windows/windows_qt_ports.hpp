#pragma once
#include "ports/export_ports.hpp"
#include "ui/qt/qt_input_platform_adapter.hpp"
#include "ui/qt/qt_overlay_window_port.hpp"
#include "ui/qt/qt_window_activation_port.hpp"
#include "platform/windows/windows_capture.hpp"
#include <QAbstractNativeEventFilter>

namespace hdrshot {
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
  void configure(QWidget& host,bool order_front) override;
  void resize(QWidget& host) override;
  void set_system_dialog_active(QWidget& host,bool active,bool restore_focus) override;
  void* native_surface() const override { return surface_; }
 protected:
  bool eventFilter(QObject* object,QEvent* event) override;
 private:
  struct NativeInput;
  std::unique_ptr<NativeInput> input_;
  WindowsDisplayInfo display_;
  void* surface_{};
  void* host_handle_{};
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
  Result<ChooseSavePathOutcome,Error> choose_save_path(const ChooseSavePathRequest&) override;
};
class WindowsClockPort final : public ClockPort {
 public:
  LocalDateTime now_local() const override;
};
}  // namespace hdrshot
