#pragma once
#include "domain/analysis/types.hpp"
#include <QObject>
#include <QRect>
#include <QSize>
#include <QTimer>
#include <array>
#include <functional>
class QWidget;
class QToolButton;
class QVariantAnimation;
namespace hdrshot {
class AnalyzerSourceView;
class SourcePresentationController;
// Passive secondary Source surfaces. No image/color implementation lives here.
class PinnedScreenshotPreview final : public QObject {
public:
  PinnedScreenshotPreview(QWidget &owner, QToolButton &button, const analysis::Input &input);
  ~PinnedScreenshotPreview() override;
  void set_collapsed(bool);
  void dismiss();
  void refresh(int index);
  QWidget &host(int index) const;
  AnalyzerSourceView &source(int index) const;
  static QRect card_geometry(QRect anchor, QSize image, QRect available);
  std::function<bool(int, analysis::SourceView, std::uintptr_t)> present;
  std::function<void(QWidget &)> configure_window;
protected:
  bool eventFilter(QObject *, QEvent *) override;
private:
  void show_preview();
  QWidget &owner_;
  QToolButton &button_;
  QWidget *card_{}, *badge_{};
  std::array<AnalyzerSourceView *,2> sources_{};
  std::array<SourcePresentationController *,2> controllers_{};
  QTimer hover_;
  QVariantAnimation *appearance_{};
  QSize image_;
  QPoint target_position_;
  bool collapsed_{}, hovering_{};
};
}
