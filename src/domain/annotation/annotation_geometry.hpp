#pragma once

#include "domain/annotation/annotation_document.hpp"
#include "domain/annotation/annotation_units.hpp"
#include <array>
#include <optional>

namespace hdrshot {

enum class AnnotationAdjustment : std::uint8_t {
  move, north, north_east, east, south_east, south, south_west, west,
  north_west, arrow_start, arrow_end, rotate,
};

struct ShapePoint {
  double x{}, y{};
};
struct ShapeBounds {
  double left{}, top{}, right{}, bottom{};
};

[[nodiscard]] bool is_rotatable(const AnnotationObject& object);
[[nodiscard]] bool valid_shape_transform(const AnnotationObject& object);
[[nodiscard]] ShapePoint shape_center(const AnnotationObject& object);
// Local coordinates are relative to the shape center, before rotation.
[[nodiscard]] ShapePoint shape_to_local(const AnnotationObject& object, ShapePoint world);
[[nodiscard]] ShapePoint shape_to_world(const AnnotationObject& object, ShapePoint local);
// NW,N,NE,E,SE,S,SW,W. Padding is in capture pixels, not logical UI units.
[[nodiscard]] std::array<ShapePoint, 8> shape_handles(
    const AnnotationObject& object, double padding = 0);
[[nodiscard]] ShapeBounds shape_bounds(const AnnotationObject& object, double margin = 0);
[[nodiscard]] PixelRect shape_pixel_bounds(const AnnotationObject& object, double margin = 0);
[[nodiscard]] bool shape_contains(const AnnotationObject& object, ShapePoint point);
[[nodiscard]] bool shape_fits(const AnnotationObject &object, PixelRect limits,
                              AnnotationScale scale);
[[nodiscard]] std::optional<AnnotationAdjustment> shape_adjustment_at(
    const AnnotationObject& object, ShapePoint point,
    double resize_tolerance_px, double rotation_band_px);
[[nodiscard]] AnnotationObject move_shape(const AnnotationObject &origin,
                                          ShapePoint delta, PixelRect limits,
                                          AnnotationScale scale);
[[nodiscard]] AnnotationObject
resize_shape(const AnnotationObject &origin, AnnotationAdjustment handle,
             ShapePoint anchor, ShapePoint pointer, PixelRect limits,
             int minimum_extent, AnnotationScale scale);
[[nodiscard]] AnnotationObject
rotate_shape_until_boundary(const AnnotationObject &origin,
                            double delta_radians, PixelRect limits,
                            AnnotationScale scale);

class ShapeRotationDrag {
 public:
  ShapeRotationDrag(const AnnotationObject& origin, ShapePoint pointer);
  [[nodiscard]] const AnnotationObject &
  update(ShapePoint pointer, PixelRect limits, AnnotationScale scale);
  [[nodiscard]] const AnnotationObject& preview() const { return preview_; }
 private:
  AnnotationObject preview_;
  double pointer_angle_{};
};
struct LineSegmentGeometry {
  StrokePoint start, end;
};
std::vector<LineSegmentGeometry>
annotation_line_segments(const AnnotationObject &, AnnotationScale);
bool line_hit_test(const AnnotationObject &, StrokePoint, double,
                   AnnotationScale);
Result<double, Error> stroke_width_px(const AnnotationObject &,
                                      AnnotationScale);
double stroke_margin(const AnnotationObject &, AnnotationScale);
Result<PixelRect, Error> brush_geometry_bounds(const BrushGeometry &);
ShapeBounds brush_centerline_bounds(const BrushGeometry &);
bool brush_centerline_inside(const BrushGeometry &, PixelRect);
PixelRect brush_paint_bounds(const AnnotationObject &, AnnotationScale,
                             PixelRect);
bool brush_hit_test(const AnnotationObject &, StrokePoint, double tolerance_px,
                    AnnotationScale);
AnnotationObject resize_brush(const AnnotationObject &, AnnotationAdjustment,
                              StrokePoint anchor, StrokePoint pointer,
                              PixelRect limits, AnnotationScale);
inline StrokePoint brush_world_point(const BrushGeometry &g, StrokePoint p) {
  return {g.origin_x + p.x * g.scale_x, g.origin_y + p.y * g.scale_y};
}
} // namespace hdrshot
