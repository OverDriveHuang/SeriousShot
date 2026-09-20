#pragma once
#include <QWidget>
#include <functional>
namespace hdrshot {
// Only native surface/window actions. Shared host owns editor and session flow.
class QtOverlayWindowPort {
 public:
  virtual ~QtOverlayWindowPort() = default;
  virtual void prepare(QWidget& host) = 0;
  virtual void configure(QWidget& host, bool order_front) = 0;
  virtual void resize(QWidget& host) = 0;
  virtual void set_system_dialog_active(QWidget& host, bool active, bool restore_focus) = 0;
  // Optional native edge notifications, in addition to Qt application inactive.
  // Called on the UI thread only. Internal control/overlay focus transfers are
  // not interruptions. Clearing the callback disconnects the client.
  virtual void set_input_interrupted(std::function<void()> callback) { (void)callback; }
  virtual void restore_input_focus(QWidget& host) { host.activateWindow(); host.setFocus(); }
  [[nodiscard]] virtual void* native_surface() const = 0;
};
} // namespace hdrshot
