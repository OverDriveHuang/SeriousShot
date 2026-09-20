#pragma once
#include "ui/qt/qt_window_activation_port.hpp"
namespace hdrshot {
class MacQtWindowActivation final : public QtWindowActivationPort {
 public:
  void activate(QWidget& window) override;
};
}  // namespace hdrshot
