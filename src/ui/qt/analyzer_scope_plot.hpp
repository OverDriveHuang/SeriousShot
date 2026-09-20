#pragma once
#include "domain/analysis/types.hpp"
#include <QImage>
#include <QPainterPath>
#include <QWidget>
#include <functional>

namespace hdrshot {
// Empty when the target itself is outside the viewport; only the text is
// constrained. This must never move the calibrated target or its trace.
QRectF analyzer_vector_target_label_bounds(QRectF plot, QPointF target,
                                           QSizeF text_size);
// Label collision policy only; statistical bins and calibrated tick values
// remain unchanged. Major landmarks win over supplementary zoom ticks.
std::vector<analysis::AxisTick> analyzer_axis_label_ticks(
    std::vector<analysis::AxisTick> ticks, double span, double label_height);
struct AnalyzerReferenceLine {
  double value{};
  double domain{};
  bool enabled{true};
  bool intensity{};
  QString label;
};

class AnalyzerScopePlot final : public QWidget {
public:
  enum class Kind { waveform, histogram, vectorscope };
  AnalyzerScopePlot(Kind kind, QWidget *parent = nullptr);
  void set_result(analysis::ResultRef result, bool rebuild_density = true);
  void set_options(analysis::ScopeOptions options);
  void set_references(std::vector<AnalyzerReferenceLine> references);
  void set_transient_hidden(bool hidden);
  void set_mask_enabled(bool enabled);
  void set_pending(bool pending);
  void set_display_gain(double gain);
  [[nodiscard]] double display_gain() const { return display_gain_; }
  [[nodiscard]] bool is_pending() const { return pending_; }
  void fit();
  bool cancel_gesture();
  [[nodiscard]] QRectF plot_rect() const;
  [[nodiscard]] Kind kind() const { return kind_; }
  std::function<void(const analysis::ScopeOptions &)> options_changed;
  std::function<void()> surface_resized;

protected:
  bool event(QEvent *) override;
  void paintEvent(QPaintEvent *) override;
  void resizeEvent(QResizeEvent *) override;
  void wheelEvent(QWheelEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void leaveEvent(QEvent *) override;

private:
  void draw_wave(QPainter &);
  void draw_histogram(QPainter &);
  void draw_vector(QPainter &);
  void draw_markers(QPainter &);
  void draw_trace_layers(QPainter &, bool highlight = false);
  void draw_distribution(QPainter &, const analysis::SampleResult &);
  void rebuild_highlight_images();
  void notify_options();
  void rebuild_density_images(bool base = true, bool detail = true);
  void zoom_at(QPointF position, double factor);
  void pan_by(QPointF pixels);
  [[nodiscard]] analysis::VectorCalibration view_calibration() const;
  Kind kind_;
  analysis::ResultRef result_;
  analysis::ScopeOptions options_;
  std::array<QImage, 4> wave_images_;
  std::array<QImage, 4> wave_detail_images_;
  QImage vector_image_, vector_detail_image_;
  // Same statistics/normalization as the trace, colored at 4 * display_gain_.
  // Lazily regenerated on density/color/gain changes, never per hover sample.
  std::array<QImage, 4> highlight_wave_, highlight_wave_detail_;
  QImage highlight_vector_, highlight_vector_detail_;
  bool highlight_dirty_{true};
  // Domain-space path survives hover, mask, gain and viewport changes.
  QPainterPath gamut_path_;
  std::optional<analysis::WorkingSpace> gamut_space_;
  std::vector<AnalyzerReferenceLine> references_;
  bool transient_hidden_{};
  bool mask_enabled_{};
  bool pending_{true};
  bool dragging_{};
  double display_gain_{1.0};
  QPointF drag_start_;
  analysis::ScopeOptions old_options_;
};
} // namespace hdrshot
