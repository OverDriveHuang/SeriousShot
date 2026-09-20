#pragma once

#include "domain/annotation/annotation_document.hpp"
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
[[nodiscard]] bool shape_fits(const AnnotationObject& object, PixelRect limits);
[[nodiscard]] std::optional<AnnotationAdjustment> shape_adjustment_at(
    const AnnotationObject& object, ShapePoint point,
    double resize_tolerance_px, double rotation_band_px);
[[nodiscard]] AnnotationObject move_shape(
    const AnnotationObject& origin, ShapePoint delta, PixelRect limits);
[[nodiscard]] AnnotationObject resize_shape(
    const AnnotationObject& origin, AnnotationAdjustment handle,
    ShapePoint anchor, ShapePoint pointer, PixelRect limits, int minimum_extent);
[[nodiscard]] AnnotationObject rotate_shape_until_boundary(
    const AnnotationObject& origin, double delta_radians, PixelRect limits);

class ShapeRotationDrag {
 public:
  ShapeRotationDrag(const AnnotationObject& origin, ShapePoint pointer);
  [[nodiscard]] const AnnotationObject& update(ShapePoint pointer, PixelRect limits);
  [[nodiscard]] const AnnotationObject& preview() const { return preview_; }
 private:
  AnnotationObject preview_;
  double pointer_angle_{};
};
} // namespace hdrshot
