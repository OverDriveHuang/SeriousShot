#pragma once
#include "core/error.hpp"
#include <QWidget>
#include <functional>
#include <optional>
namespace hdrshot {
// Only native surface/window actions. Shared host owns editor and session flow.
class QtOverlayWindowPort {
 public:
  virtual ~QtOverlayWindowPort() = default;
  virtual void prepare(QWidget& host) = 0;
  [[nodiscard]] virtual std::optional<Error> preparation_error() const { return {}; }
  virtual void configure(QWidget& host, bool order_front) = 0;
  virtual void resize(QWidget& host) = 0;
  virtual void set_system_dialog_active(QWidget& host, bool active, bool restore_focus) = 0;
  // Optional native edge notifications, in addition to Qt application inactive.
  // Called on the UI thread only. Internal control/overlay focus transfers are
  // not interruptions. Clearing the callback disconnects the client.
  virtual void set_input_interrupted(std::function<void()> callback) { (void)callback; }
  virtual void restore_input_focus(QWidget& host) { host.activateWindow(); host.setFocus(); }
  [[nodiscard]] virtual void* native_surface() const = 0;
  [[nodiscard]] virtual void* magnifier_surface() const { return nullptr; }
  virtual void move_magnifier(QWidget&, const QRect&) {}
  virtual void show_magnifier() {}
  virtual void hide_magnifier() {}
};
} // namespace hdrshot
