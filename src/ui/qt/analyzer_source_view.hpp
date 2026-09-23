#pragma once

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace hdrshot {

struct AnalyzerSourceTransform {
  bool fit{true};
  double scale{1.};
  QPointF pan;
};

struct AnalyzerSourceMask {
  QRectF bounds;
  bool ellipse{};
};

struct AnalyzerSourcePin {
  std::uint64_t id{};
  QPointF source;
  int sample_size{1};
};

class AnalyzerSourceView final : public QWidget {
public:
  explicit AnalyzerSourceView(QWidget *parent = nullptr);
  ~AnalyzerSourceView() override;

  void set_source_size(QSize physical_pixels);
  void set_tools(bool picker, bool mask, bool ellipse);
  void set_sample_size(int pixels);
  void set_mask(std::optional<AnalyzerSourceMask> mask);
  void set_pins(std::vector<AnalyzerSourcePin> pins);
  void set_transform(AnalyzerSourceTransform transform);
  [[nodiscard]] AnalyzerSourceTransform transform() const { return transform_; }
  [[nodiscard]] double effective_scale() const;
  [[nodiscard]] QRectF image_rect() const;
  [[nodiscard]] QPointF source_at(QPointF local) const;
  [[nodiscard]] QPointF local_at(QPointF source) const;
  [[nodiscard]] QRectF sample_bounds(QPointF source, int size) const;
  [[nodiscard]] QWidget *presentation_surface();
  [[nodiscard]] bool dragging() const { return pressed_; }
  [[nodiscard]] bool transient_visible() const;

  void fit();
  void zoom_at(QPointF local, double factor);
  bool cancel_gesture();
  void set_report_mode(bool active);
  [[nodiscard]] QImage retained_overlay(double pixel_ratio);
  [[nodiscard]] QImage presentation_overlay(double pixel_ratio);
  // Explicit test injection. Production uses presentation_surface(), never this
  // SDR bitmap for measurement, HDR display, or final report composition.
  void set_fixture_image(QImage image);

  std::function<void()> view_changed;
  std::function<void()> overlay_changed;
  std::function<void(std::optional<QPointF>)> hover_changed;
  std::function<void(std::optional<AnalyzerSourceMask>)> mask_changed;
  std::function<void(QPointF, int)> pin_requested;
  std::function<void(bool)> gesture_changed;

protected:
  bool event(QEvent *) override;
  bool eventFilter(QObject *, QEvent *) override;
  void paintEvent(QPaintEvent *) override;
  void resizeEvent(QResizeEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void leaveEvent(QEvent *) override;
  void wheelEvent(QWheelEvent *) override;

private:
  void draw_overlay(QPainter &);
  void update_hover(QPointF local);
  void finish_gesture(QPointF local, bool cancel);
  void begin_multi_gesture(QPointF local);
  void finish_multi_gesture(QPointF local, bool cancel);
  bool valid(QPointF source) const;
  QPointF bounded(QPointF source) const;
  void notify_view();
  void invalidate_overlay();
  QWidget *surface_{};
  QWidget *overlay_{};
  QSize source_size_{1, 1};
  AnalyzerSourceTransform transform_;
  std::optional<AnalyzerSourceMask> mask_;
  std::optional<AnalyzerSourceMask> old_mask_;
  std::vector<AnalyzerSourcePin> pins_;
  QImage fixture_image_;
  QImage presentation_overlay_;
  bool overlay_dirty_{true};
  bool native_presentation_{};
  std::optional<QPointF> hover_;
  bool picker_{};
  bool mask_armed_{};
  bool ellipse_{};
  int sample_size_{1};
  bool report_mode_{};
  bool pressed_{};
  bool moved_{};
  bool multi_gesture_{};
  bool multi_touch_active_{};
  QPointF press_point_;
  QPointF press_source_;
  QPointF last_point_;
  AnalyzerSourceTransform old_transform_;
  QPointF touch_center_;
  double touch_distance_{};
};

} // namespace hdrshot
