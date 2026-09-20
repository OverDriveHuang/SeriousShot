#include "domain/annotation/annotation_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace hdrshot {
namespace {
constexpr double pi = std::numbers::pi;
double wrap(double angle) { return std::remainder(angle, 2 * pi); }
bool finite(ShapePoint p) { return std::isfinite(p.x) && std::isfinite(p.y); }
double stroke_margin(const AnnotationObject& object) {
  const auto* style = std::get_if<ShapeStyle>(&object.style);
  return style ? style->line_width_px * 0.5 + 0.5 : 0;
}
void set_center(AnnotationObject& object, ShapePoint center) {
  const auto x = center.x - object.bounds.width * 0.5;
  const auto y = center.y - object.bounds.height * 0.5;
  object.bounds.x = static_cast<std::int32_t>(std::floor(x));
  object.bounds.y = static_cast<std::int32_t>(std::floor(y));
  object.transform.offset_x = x - object.bounds.x;
  object.transform.offset_y = y - object.bounds.y;
}
} // namespace

bool is_rotatable(const AnnotationObject& object) {
  return object.kind == AnnotationKind::rectangle || object.kind == AnnotationKind::ellipse;
}
bool valid_shape_transform(const AnnotationObject& object) {
  const auto t = object.transform;
  return std::isfinite(t.rotation_radians) && std::abs(t.rotation_radians) <= pi &&
      std::isfinite(t.offset_x) && std::isfinite(t.offset_y) &&
      std::abs(t.offset_x) < 1 && std::abs(t.offset_y) < 1 &&
      (is_rotatable(object) || t == ShapeTransform{});
}
ShapePoint shape_center(const AnnotationObject& object) {
  return {object.bounds.x + object.bounds.width * 0.5 + object.transform.offset_x,
          object.bounds.y + object.bounds.height * 0.5 + object.transform.offset_y};
}
ShapePoint shape_to_local(const AnnotationObject& object, ShapePoint world) {
  const auto center = shape_center(object);
  const auto c = std::cos(object.transform.rotation_radians);
  const auto s = std::sin(object.transform.rotation_radians);
  const auto x = world.x - center.x, y = world.y - center.y;
  return {c * x + s * y, -s * x + c * y};
}
ShapePoint shape_to_world(const AnnotationObject& object, ShapePoint local) {
  const auto center = shape_center(object);
  const auto c = std::cos(object.transform.rotation_radians);
  const auto s = std::sin(object.transform.rotation_radians);
  return {center.x + c * local.x - s * local.y,
          center.y + s * local.x + c * local.y};
}
std::array<ShapePoint, 8> shape_handles(const AnnotationObject& object, double padding) {
  const auto a = object.bounds.width * 0.5 + padding;
  const auto b = object.bounds.height * 0.5 + padding;
  std::array<ShapePoint, 8> points{{{-a,-b},{0,-b},{a,-b},{a,0},{a,b},{0,b},{-a,b},{-a,0}}};
  for (auto& point : points) point = shape_to_world(object, point);
  return points;
}
ShapeBounds shape_bounds(const AnnotationObject& object, double margin) {
  const auto center = shape_center(object);
  const auto a = object.bounds.width * 0.5, b = object.bounds.height * 0.5;
  const auto c = std::abs(std::cos(object.transform.rotation_radians));
  const auto s = std::abs(std::sin(object.transform.rotation_radians));
  const auto ex = (object.kind == AnnotationKind::ellipse ? std::hypot(a*c,b*s) : a*c+b*s) + margin;
  const auto ey = (object.kind == AnnotationKind::ellipse ? std::hypot(a*s,b*c) : a*s+b*c) + margin;
  return {center.x-ex, center.y-ey, center.x+ex, center.y+ey};
}
PixelRect shape_pixel_bounds(const AnnotationObject& object, double margin) {
  const auto b = shape_bounds(object, margin);
  const auto left = static_cast<std::int32_t>(std::floor(b.left));
  const auto top = static_cast<std::int32_t>(std::floor(b.top));
  return {left, top, static_cast<std::int32_t>(std::ceil(b.right))-left,
          static_cast<std::int32_t>(std::ceil(b.bottom))-top};
}
bool shape_contains(const AnnotationObject& object, ShapePoint point) {
  const auto p = shape_to_local(object, point);
  return std::abs(p.x) <= object.bounds.width * 0.5 &&
      std::abs(p.y) <= object.bounds.height * 0.5;
}
bool shape_fits(const AnnotationObject& object, PixelRect limits) {
  if (!valid_shape_transform(object) || object.bounds.empty() || limits.empty()) return false;
  const auto b = shape_bounds(object, stroke_margin(object));
  constexpr double epsilon = 1e-8;
  return b.left >= limits.x-epsilon && b.top >= limits.y-epsilon &&
      b.right <= limits.right()+epsilon && b.bottom <= limits.bottom()+epsilon;
}
std::optional<AnnotationAdjustment> shape_adjustment_at(
    const AnnotationObject& object, ShapePoint point, double tolerance, double band) {
  if (!is_rotatable(object) || !finite(point) || tolerance < 0 || band < 0) return std::nullopt;
  const auto p = shape_to_local(object, point);
  const auto a = object.bounds.width * 0.5, b = object.bounds.height * 0.5;
  const auto outside = std::max(std::abs(p.x)-a, std::abs(p.y)-b);
  if (outside > tolerance) {
    return outside <= tolerance+band ? std::optional{AnnotationAdjustment::rotate} : std::nullopt;
  }
  const bool left = std::abs(p.x+a) <= tolerance, right = std::abs(p.x-a) <= tolerance;
  const bool top = std::abs(p.y+b) <= tolerance, bottom = std::abs(p.y-b) <= tolerance;
  if (left && top) return AnnotationAdjustment::north_west;
  if (right && top) return AnnotationAdjustment::north_east;
  if (right && bottom) return AnnotationAdjustment::south_east;
  if (left && bottom) return AnnotationAdjustment::south_west;
  if (top) return AnnotationAdjustment::north;
  if (right) return AnnotationAdjustment::east;
  if (bottom) return AnnotationAdjustment::south;
  if (left) return AnnotationAdjustment::west;
  return AnnotationAdjustment::move;
}
AnnotationObject move_shape(const AnnotationObject& origin, ShapePoint delta, PixelRect limits) {
  if (!finite(delta) || !is_rotatable(origin) || limits.empty()) return origin;
  auto result = origin;
  const auto b = shape_bounds(origin,
      origin.transform == ShapeTransform{} ? 0 : stroke_margin(origin));
  // Do not make an already clipped legacy stroke jump on a zero-distance press.
  const auto lo_x = std::min(0.0, limits.x-b.left), hi_x = std::max(0.0, limits.right()-b.right);
  const auto lo_y = std::min(0.0, limits.y-b.top), hi_y = std::max(0.0, limits.bottom()-b.bottom);
  const auto center = shape_center(origin);
  set_center(result, {center.x+std::clamp(delta.x,lo_x,hi_x),
                      center.y+std::clamp(delta.y,lo_y,hi_y)});
  return result;
}
AnnotationObject resize_shape(const AnnotationObject& origin, AnnotationAdjustment handle,
    ShapePoint anchor, ShapePoint pointer, PixelRect limits, int minimum_extent) {
  if (!is_rotatable(origin) || !finite(anchor) || !finite(pointer)) return origin;
  const bool l=handle==AnnotationAdjustment::west || handle==AnnotationAdjustment::north_west || handle==AnnotationAdjustment::south_west;
  const bool r=handle==AnnotationAdjustment::east || handle==AnnotationAdjustment::north_east || handle==AnnotationAdjustment::south_east;
  const bool t=handle==AnnotationAdjustment::north || handle==AnnotationAdjustment::north_west || handle==AnnotationAdjustment::north_east;
  const bool b=handle==AnnotationAdjustment::south || handle==AnnotationAdjustment::south_west || handle==AnnotationAdjustment::south_east;
  if (!(l || r || t || b)) return origin;
  const auto first=shape_to_local(origin,anchor), last=shape_to_local(origin,pointer);
  const auto candidate = [&](double fraction) {
    auto result=origin;
    const auto dx=std::lround((last.x-first.x)*fraction);
    const auto dy=std::lround((last.y-first.y)*fraction);
    result.bounds.width=std::max(minimum_extent,origin.bounds.width+static_cast<int>((r?dx:0)-(l?dx:0)));
    result.bounds.height=std::max(minimum_extent,origin.bounds.height+static_cast<int>((b?dy:0)-(t?dy:0)));
    const auto shift_x=(result.bounds.width-origin.bounds.width)*0.5*(l?-1:1);
    const auto shift_y=(result.bounds.height-origin.bounds.height)*0.5*(t?-1:1);
    set_center(result,shape_to_world(origin,{shift_x,shift_y}));
    return result;
  };
  auto result=candidate(1);
  if (shape_fits(result,limits)) return result;
  if (!shape_fits(origin,limits)) return origin;
  double low=0,high=1;
  for (int i=0;i<40;++i) {
    const auto middle=(low+high)*0.5;
    if (shape_fits(candidate(middle),limits)) low=middle; else high=middle;
  }
  return candidate(low);
}
AnnotationObject rotate_shape_until_boundary(
    const AnnotationObject& origin, double delta, PixelRect limits) {
  if (!is_rotatable(origin) || !std::isfinite(delta) || !shape_fits(origin,limits)) return origin;
  // A full revolution repeats all constraints; input gestures use <= pi steps.
  delta=std::clamp(delta,-2*pi,2*pi);
  if (delta==0) return origin;
  const auto start=origin.transform.rotation_radians;
  const auto finish=start+delta;
  const auto a=origin.bounds.width*0.5, b=origin.bounds.height*0.5;
  std::vector<double> fractions{0,1};
  // Between support-function extrema each edge constraint is monotone. Checking
  // these boundaries also catches valid final angles beyond an invalid arc.
  for (int quadrant=-6;quadrant<=6;++quadrant) {
    const auto base=quadrant*pi*0.5;
    for (const auto offset : {0.0,std::atan2(b,a),std::atan2(a,b)}) {
      const auto f=(base+offset-start)/delta;
      if (f>0 && f<1) fractions.push_back(f);
    }
  }
  std::sort(fractions.begin(),fractions.end());
  const auto candidate=[&](double fraction) {
    auto value=origin; value.transform.rotation_radians=wrap(start+delta*fraction); return value;
  };
  double previous=0;
  for (auto f : fractions) {
    if (!shape_fits(candidate(f),limits)) {
      double low=previous,high=f;
      for(int i=0;i<45;++i) {
        const auto middle=(low+high)*0.5;
        if(shape_fits(candidate(middle),limits)) low=middle; else high=middle;
      }
      return candidate(low);
    }
    previous=f;
  }
  return candidate((finish-start)/delta);
}
ShapeRotationDrag::ShapeRotationDrag(const AnnotationObject& origin, ShapePoint pointer)
    : preview_(origin) {
  const auto center=shape_center(origin);
  pointer_angle_=std::atan2(pointer.y-center.y,pointer.x-center.x);
}
const AnnotationObject& ShapeRotationDrag::update(ShapePoint pointer, PixelRect limits) {
  const auto center=shape_center(preview_);
  if (!finite(pointer) || std::hypot(pointer.x-center.x,pointer.y-center.y)<1e-6) return preview_;
  const auto angle=std::atan2(pointer.y-center.y,pointer.x-center.x);
  preview_=rotate_shape_until_boundary(preview_,wrap(angle-pointer_angle_),limits);
  pointer_angle_=angle;
  return preview_;
}
} // namespace hdrshot
