#pragma once
#include <QComboBox>

namespace hdrshot {
class AnnotationSizePopup;
// Keeps QComboBox model/data/input/signals; closed and popup surfaces are shared Qt painting.
class AnnotationSizeCombo final : public QComboBox {
 public:
  explicit AnnotationSizeCombo(QWidget* parent = nullptr);
  ~AnnotationSizeCombo() override;
  void showPopup() override;
  void hidePopup() override;
  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;
 protected:
  void paintEvent(QPaintEvent* event) override;
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  void changeEvent(QEvent* event) override;
 private:
  friend class AnnotationSizePopup;
  void commit_row(int row);
  void popup_dismissed();
  AnnotationSizePopup* popup_;
  bool hovered_{};
  bool pressed_{};
};
}  // namespace hdrshot
