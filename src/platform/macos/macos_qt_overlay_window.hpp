#pragma once
#include "ui/qt/qt_overlay_window_port.hpp"
#include <memory>
namespace hdrshot {
class MacQtOverlayWindow final : public QtOverlayWindowPort {
 public:
  explicit MacQtOverlayWindow(void* native_screen);
  ~MacQtOverlayWindow() override;
  void prepare(QWidget&) override;
  void configure(QWidget&, bool order_front) override;
  void resize(QWidget&) override;
  void set_system_dialog_active(QWidget&, bool active, bool restore_focus) override;
  void set_input_interrupted(std::function<void()>) override;
  void restore_input_focus(QWidget&) override;
  [[nodiscard]] void* native_surface() const override;
  [[nodiscard]] std::optional<Error> preparation_error() const override;
  [[nodiscard]] void* magnifier_surface() const override;
  void move_magnifier(QWidget&, const QRect&) override;
  void show_magnifier() override;
  void hide_magnifier() override;
 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
} // namespace hdrshot
