#pragma once
#include <QWidget>
namespace hdrshot {
// UI adapter boundary: business/View code never calls a native window API.
class QtWindowActivationPort {
 public:
  virtual ~QtWindowActivationPort() = default;
  virtual void activate(QWidget& window) = 0;
};
class QtDefaultWindowActivation final : public QtWindowActivationPort {
 public:
  void activate(QWidget& window) override {
    window.showNormal();
    window.raise();
    window.activateWindow();
  }
};
}  // namespace hdrshot
