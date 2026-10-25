#pragma once

#include "domain/analysis/types.hpp"
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>

class QWidget;
namespace hdrshot {
class AnalyzerSourceView;

struct SourcePresentationOptions {
  analysis::Settings settings;
  bool false_color{};
  analysis::Mask mask;
  double mask_outside_factor{0.25};
  bool include_operation_overlay{};
};

// One controller per Source surface. This layer knows only Qt viewport and
// presentation lifetime; analysis and native display/color state stay outside.
class SourcePresentationController final : public QObject {
public:
  using OptionsProvider = std::function<SourcePresentationOptions()>;
  // False means no consumer was ready. Keep dirty without spinning a timer.
  using Submit = std::function<bool(analysis::SourceView, std::uintptr_t)>;
  using Ready = std::function<bool()>;
  SourcePresentationController(QWidget &owner, AnalyzerSourceView &source,
                               OptionsProvider options, Submit submit,
                               Ready ready = {});
  ~SourcePresentationController() override;
  void request_present();
  // Only call after an accepted close or during destruction, never on Close.
  void stop();
  // The report/compatibility path deliberately excludes operation_overlay.
  [[nodiscard]] analysis::SourceView view() const;

protected:
  bool eventFilter(QObject *, QEvent *) override;

private:
  void dispatch();
  bool visible() const;
  QPointer<QWidget> owner_;
  QPointer<AnalyzerSourceView> source_;
  QPointer<QWidget> surface_;
  OptionsProvider options_;
  Submit submit_;
  Ready ready_;
  QTimer timer_;
  bool dirty_{}, stopped_{}, native_surface_unavailable_{}, owner_surface_unavailable_{};
  qint64 operation_overlay_key_{};
  std::shared_ptr<const analysis::UiImage> operation_overlay_;
};
} // namespace hdrshot
