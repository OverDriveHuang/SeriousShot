#pragma once

#include "application/annotation_edit_service.hpp"
#include "application/input_mapper.hpp"
#include "domain/annotation/annotation_document.hpp"
#include "domain/annotation/annotation_geometry.hpp"
#include "domain/annotation/annotation_render_plan.hpp"
#include "domain/annotation/brush_stroke_builder.hpp"
#include "domain/geometry/selection_model.hpp"
#include "domain/geometry/window_selection.hpp"
#include "ports/text_rasterizer_port.hpp"
#include "ui/qt/qt_input_platform_adapter.hpp"

#include <QImage>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class QComboBox;
class QFrame;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPainter;
class QString;
class QToolButton;

namespace hdrshot {

enum class EditorTool : std::uint8_t {
  select,
  rectangle,
  ellipse,
  line,
  arrow,
  text,
  brush
};

class OverlayEditorWidget final : public QWidget {
 public:
  void set_magnifier_exclusion(QRect region) {if(magnifier_exclusion_!=region){magnifier_exclusion_=region;update();}}
  [[nodiscard]] bool magnifier_applicable() const {return !interaction_locked_ && (selection_.desktop_rect.empty() || dragging_selection_ || selection_adjustment_.has_value());}
  void set_clean_content_presented(bool enabled) { clean_content_presented_ = enabled; update(); }

  // Enabled by the shell after its native presentation path is validated.
  void set_annotation_preview_coalescing(bool enabled) {
    coalesce_annotation_moves_ = enabled;
  }
  [[nodiscard]] std::uint64_t annotation_preview_epoch() const {
    return annotation_preview_epoch_;
  }

  using PreviewChanged =
      std::function<void(
          const SelectionSnapshot&,
          const AnnotationDocumentSnapshot&,
          std::shared_ptr<const AnnotationRenderPlan>)>;
  using ActionRequested = std::function<void(UiCommand)>;
  using InteractionStarted = std::function<bool()>;
  using InitialGestureChanged = std::function<bool(bool)>;

  OverlayEditorWidget(
      PixelSize capture_size_px,
      double point_pixel_scale,
      TextRasterizerPort* text_rasterizer,
      std::string text_editor_font_family,
      QtInputPlatformAdapter& input_platform,
      SelectionSnapshot initial_selection,
      QWidget* parent = nullptr);

  void set_preview_changed(PreviewChanged callback);
  void set_action_requested(ActionRequested callback);
  void set_analysis_available(bool available);
  void set_pin_available(bool available);
  void set_interaction_started(InteractionStarted callback);
  void set_interaction_locked(bool locked);
  void set_completion_bindings(CompletionBindings bindings);
  void set_window_candidates(WindowSnapshotRef snapshot, DisplayId display);
  void set_initial_gesture_changed(InitialGestureChanged callback);
  void refresh_window_candidate(QPointF position);
  void clear_window_candidate();
  void interrupt_pointer_gesture();
  void restore_interaction_focus(QPointF position);
  [[nodiscard]] PreviewHighlight initial_highlight() const;
  void show_status(const QString& message, bool is_error);

  [[nodiscard]] const SelectionSnapshot& selection() const noexcept { return selection_; }
  [[nodiscard]] const AnnotationDocumentSnapshot& annotations() const noexcept {
    return document_.snapshot();
  }
  [[nodiscard]] std::shared_ptr<const AnnotationRenderPlan> annotation_render_plan() const {
    return annotation_render_plan_;
  }

 protected:
  bool event(QEvent* event) override;
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void enterEvent(QEnterEvent* event) override;

 private:
   AnnotationScale annotation_scale() const {
     return AnnotationScale::from(point_pixel_scale_).value();
   }
   AnnotationEditContext edit_context() {
     return {selection_, annotation_scale(), text_rasterizer_,
             &annotation_coverage_cache_};
   }
   StrokePoint to_brush_point(QPointF) const;
   bool begin_brush_stroke(QPointF);
   void flush_pending_brush_preview();
   bool finish_brush_stroke(StrokePoint);
   bool finish_pending_pointer_edit();
   void update_brush_transform(QPointF);
   BrushStrokeBuilder brush_builder_;
   bool brush_active_{}, brush_preview_scheduled_{};
   std::vector<StrokePoint> pending_brush_points_;
   StrokePoint last_brush_point_{}, brush_drag_anchor_{};
   std::optional<QPointF> brush_cursor_position_;
   std::optional<ReusableAnnotationLayer> pending_reuse_;
   std::uint16_t brush_diameter_dip_{default_brush_size_dip};
   void flush_pending_annotation_move();
   bool coalesce_annotation_moves_{false};
   bool processing_annotation_move_{false};
   bool annotation_move_scheduled_{false};
   std::optional<QPointF> pending_annotation_move_;
   std::uint64_t annotation_preview_epoch_{};
   AnnotationCoverageCache annotation_coverage_cache_;
   QRect magnifier_exclusion_{};
   bool clean_content_presented_{};
   QImage painted_coverage_;
   std::shared_ptr<const AnnotationRenderPlan> painted_plan_;
   std::optional<AnnotationObject> draft_annotation_;
   void update_new_annotation_preview(PixelPoint current);

   void build_controls();
   void apply_theme();
   void choose_tool(EditorTool tool);
   void choose_color(std::uint32_t color_srgb_rgb);
   void choose_size(std::uint16_t value);
   bool apply_document_command(const AnnotationCommand &command);
   void request_action(UiCommand command);
   [[nodiscard]] bool rebuild_render_plan();
   [[nodiscard]] bool apply_selection_candidate(SelectionSnapshot candidate);
   void draw_coverage_layer(QPainter &painter,
                            const AnnotationCoverageLayer &layer) const;
   void notify_preview_changed();
   void layout_controls();
   void update_property_controls();
   void update_pending_text_editor_style();
   bool finish_pending_text_edit();
   void finish_annotation(PixelPoint end);
   [[nodiscard]] PixelRect
   normalize_text_bounds(PixelRect bounds, const std::string &text = {},
                         std::optional<TextStyle> style = std::nullopt) const;
   [[nodiscard]] int
   text_editor_font_pixel_size(std::uint16_t font_size_dip) const;
   void begin_text_edit(PixelRect bounds);
   void begin_existing_text_edit(const AnnotationObject &object);
   bool commit_text_edit();
   void cancel_text_edit();
   [[nodiscard]] std::optional<ObjectId> hit_test(PixelPoint point) const;
   [[nodiscard]] const AnnotationObject *selected_object() const;
   [[nodiscard]] PixelRect annotation_bounds() const;
   [[nodiscard]] std::optional<AnnotationAdjustment>
   annotation_adjustment_at(QPointF point) const;
   [[nodiscard]] AnnotationObject
   adjusted_annotation(const AnnotationObject &origin,
                       AnnotationAdjustment adjustment, PixelPoint anchor,
                       PixelPoint current) const;
   void update_annotation_drag_preview(PixelPoint current);
   void update_pointer_cursor(QPointF point);
   void layout_annotation_controls();
   [[nodiscard]] PixelPoint to_capture_point(QPointF point) const;
   [[nodiscard]] ShapePoint to_shape_point(QPointF point) const;
   [[nodiscard]] QPointF shape_widget_point(ShapePoint point) const;
   [[nodiscard]] SelectionPointer selection_pointer(QPointF point) const;
   void finish_initial_gesture();
   [[nodiscard]] PixelPoint to_drawing_point(QPointF point) const;
   [[nodiscard]] PixelPoint output_to_capture_point(PixelPoint point) const;
   [[nodiscard]] bool capture_point_in_selection(PixelPoint point) const;
   [[nodiscard]] QPointF to_widget_point(PixelPoint point) const;
   [[nodiscard]] QRectF to_widget_rect(PixelRect rect) const;
   [[nodiscard]] QRectF to_widget_output_rect(PixelRect rect) const;
   [[nodiscard]] PixelRect active_drawing_bounds() const;
   [[nodiscard]] std::optional<SelectionAdjustment>
   selection_adjustment_at(QPointF point) const;

   PixelSize capture_size_px_{};
   double point_pixel_scale_{1.0};
   TextRasterizerPort *text_rasterizer_{};
   std::string text_editor_font_family_;
   QtInputPlatformAdapter &input_platform_;
   CompletionBindings completion_bindings_{};
   SelectionSnapshot selection_{};
   AnnotationDocument document_{AnnotationDocument::empty()};
   std::shared_ptr<const AnnotationRenderPlan> annotation_render_plan_;
   EditorTool tool_{EditorTool::select};
   std::uint32_t color_srgb_rgb_{0xFF4D67};
   std::uint16_t line_width_dip_{default_shape_size_dip};
   std::uint16_t font_size_dip_{default_text_size_dip};
   ObjectId next_object_id_{1};
   PixelPoint drag_anchor_{};
   SelectionSnapshot selection_drag_origin_{};
   std::optional<SelectionAdjustment> selection_adjustment_;
   std::optional<AnnotationAdjustment> annotation_adjustment_;
   std::optional<AnnotationObject> annotation_drag_origin_;
   std::optional<AnnotationObject> annotation_drag_preview_;
   std::optional<ShapeRotationDrag> rotation_drag_;
   std::shared_ptr<const AnnotationRenderPlan>
       transient_annotation_render_plan_;
   std::optional<PixelPoint> drag_current_;
   bool dragging_selection_{};
   bool dragging_annotation_{};
   bool dragging_annotation_transform_{};
   bool suppress_parent_text_key_{};
   PreviewChanged preview_changed_;
   ActionRequested action_requested_;
   InteractionStarted interaction_started_;
   InitialGestureChanged initial_gesture_changed_;
   std::unique_ptr<WindowSelectionGesture> window_selection_;
   bool last_press_had_selection_{true};
   bool interaction_locked_{};

   QFrame *toolbar_{};
   QFrame *property_bar_{};
   QLabel *status_label_{};
   QHBoxLayout *toolbar_layout_{};
   QHBoxLayout *property_layout_{};
   QComboBox *size_combo_{};
   QToolButton *tool_buttons_[7]{};
   QToolButton *color_buttons_[7]{};
   QToolButton *delete_annotation_button_{};
   QLineEdit *text_editor_{};
   std::optional<ObjectId> editing_text_object_id_;
   std::optional<PixelRect> pending_text_bounds_;
   std::optional<TextStyle> pending_text_style_;
};

}  // namespace hdrshot
