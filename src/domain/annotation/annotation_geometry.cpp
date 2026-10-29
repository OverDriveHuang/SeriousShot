#include "domain/annotation/annotation_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

namespace hdrshot {
namespace {
constexpr double pi = std::numbers::pi;
double wrap(double angle) { return std::remainder(angle, 2 * pi); }
bool finite(ShapePoint p) { return std::isfinite(p.x) && std::isfinite(p.y); }
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
  if (!std::isfinite(b.left) || !std::isfinite(b.top) ||
      !std::isfinite(b.right) || !std::isfinite(b.bottom) ||
      std::floor(b.left) < std::numeric_limits<int>::min() ||
      std::floor(b.top) < std::numeric_limits<int>::min() ||
      std::ceil(b.right) > std::numeric_limits<int>::max() ||
      std::ceil(b.bottom) > std::numeric_limits<int>::max() ||
      std::ceil(b.right) - std::floor(b.left) >
          std::numeric_limits<int>::max() ||
      std::ceil(b.bottom) - std::floor(b.top) > std::numeric_limits<int>::max())
    return {};
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
bool shape_fits(const AnnotationObject &object, PixelRect limits,
                AnnotationScale scale) {
  if (!valid_shape_transform(object) || object.bounds.empty() ||
      !valid_annotation_clip(limits))
    return false;
  const auto b = shape_bounds(object, stroke_margin(object, scale));
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
AnnotationObject move_shape(const AnnotationObject &origin, ShapePoint delta,
                            PixelRect limits, AnnotationScale scale) {
  if (!finite(delta) || !is_rotatable(origin) || !valid_annotation_clip(limits))
    return origin;
  auto result = origin;
  const auto b = shape_bounds(origin, origin.transform == ShapeTransform{}
                                          ? 0
                                          : stroke_margin(origin, scale));
  // Do not make an already clipped legacy stroke jump on a zero-distance press.
  const auto lo_x = std::min(0.0, limits.x-b.left), hi_x = std::max(0.0, limits.right()-b.right);
  const auto lo_y = std::min(0.0, limits.y-b.top), hi_y = std::max(0.0, limits.bottom()-b.bottom);
  const auto center = shape_center(origin);
  set_center(result, {center.x+std::clamp(delta.x,lo_x,hi_x),
                      center.y+std::clamp(delta.y,lo_y,hi_y)});
  return result;
}
AnnotationObject resize_shape(const AnnotationObject &origin,
                              AnnotationAdjustment handle, ShapePoint anchor,
                              ShapePoint pointer, PixelRect limits,
                              int minimum_extent, AnnotationScale scale) {
  if (!valid_annotation_clip(limits) || !is_rotatable(origin) ||
      !finite(anchor) || !finite(pointer))
    return origin;
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
  if (shape_fits(result, limits, scale))
    return result;
  if (!shape_fits(origin, limits, scale))
    return origin;
  double low=0,high=1;
  for (int i=0;i<40;++i) {
    const auto middle=(low+high)*0.5;
    if (shape_fits(candidate(middle), limits, scale))
      low = middle;
    else
      high = middle;
  }
  return candidate(low);
}
AnnotationObject rotate_shape_until_boundary(const AnnotationObject &origin,
                                             double delta, PixelRect limits,
                                             AnnotationScale scale) {
  if (!is_rotatable(origin) || !std::isfinite(delta) ||
      !shape_fits(origin, limits, scale))
    return origin;
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
    if (!shape_fits(candidate(f), limits, scale)) {
      double low=previous,high=f;
      for(int i=0;i<45;++i) {
        const auto middle=(low+high)*0.5;
        if (shape_fits(candidate(middle), limits, scale))
          low = middle;
        else
          high = middle;
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
const AnnotationObject &ShapeRotationDrag::update(ShapePoint pointer,
                                                  PixelRect limits,
                                                  AnnotationScale scale) {
  const auto center=shape_center(preview_);
  if (!finite(pointer) || std::hypot(pointer.x-center.x,pointer.y-center.y)<1e-6) return preview_;
  const auto angle=std::atan2(pointer.y-center.y,pointer.x-center.x);
  preview_ = rotate_shape_until_boundary(preview_, wrap(angle - pointer_angle_),
                                         limits, scale);
  pointer_angle_=angle;
  return preview_;
}
} // namespace hdrshot

namespace hdrshot {
std::vector<LineSegmentGeometry>
annotation_line_segments(const AnnotationObject &o, AnnotationScale scale) {
  if (!o.arrow_geometry)
    return {};
  auto g = *o.arrow_geometry;
  StrokePoint a{double(g.start.x), double(g.start.y)},
      b{double(g.end.x), double(g.end.y)};
  std::vector<LineSegmentGeometry> segments{{a, b}};
  if (o.kind == AnnotationKind::arrow) {
    auto style = std::get_if<ShapeStyle>(&o.style);
    if (!style)
      return {};
    double angle = std::atan2(a.y - b.y, a.x - b.x),
           head = std::max(12., 4. * style->line_width_dip) * scale.value();
    for (double offset : {std::numbers::pi / 6, -std::numbers::pi / 6})
      segments.push_back({b,
                          {b.x + std::cos(angle + offset) * head,
                           b.y + std::sin(angle + offset) * head}});
  }
  return segments;
}
bool line_hit_test(const AnnotationObject &o, StrokePoint p, double tolerance,
                   AnnotationScale scale) {
  for (auto segment : annotation_line_segments(o, scale)) {
    auto a = segment.start, b = segment.end;
    double dx = b.x - a.x, dy = b.y - a.y, d = dx * dx + dy * dy;
    double t =
        d > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / d, 0., 1.)
              : 0;
    if (std::hypot(p.x - a.x - t * dx, p.y - a.y - t * dy) <=
        tolerance + stroke_margin(o, scale))
      return true;
  }
  return false;
}
Result<double, Error> stroke_width_px(const AnnotationObject &o,
                                      AnnotationScale scale) {
  if (auto s = std::get_if<ShapeStyle>(&o.style))
    return to_capture_px(s->line_width_dip, scale);
  if (auto s = std::get_if<BrushStyle>(&o.style))
    return to_capture_px(s->diameter_dip, scale);
  return Result<double, Error>::failure(
      {ErrorCode::invalid_input, "AnnotationGeometry"});
}
double stroke_margin(const AnnotationObject &o, AnnotationScale scale) {
  auto width = stroke_width_px(o, scale);
  return width ? width.value() * 0.5 + 0.5 : 0;
}
BrushPath::BrushPath(std::vector<StrokePoint> input)
    : points(std::move(input)) {
  local_bounds = {INFINITY, INFINITY, -INFINITY, -INFINITY};
  valid = !points.empty();
  for (auto p : points) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || std::abs(p.x) > 1e9 ||
        std::abs(p.y) > 1e9) {
      valid = false;
      return;
    }
    local_bounds[0] = std::min(local_bounds[0], p.x);
    local_bounds[1] = std::min(local_bounds[1], p.y);
    local_bounds[2] = std::max(local_bounds[2], p.x);
    local_bounds[3] = std::max(local_bounds[3], p.y);
  }
  for (std::size_t i = 0; i < points.size(); ++i) {
    auto a = points[i ? i - 1 : 0], b = points[i];
    int l = int(std::floor(std::min(a.x, b.x) / 64)),
        r = int(std::floor(std::max(a.x, b.x) / 64));
    int t = int(std::floor(std::min(a.y, b.y) / 64)),
        bot = int(std::floor(std::max(a.y, b.y) / 64));
    if (std::int64_t(r - l + 1) * (bot - t + 1) > 4096) {
      broad_segments.push_back(i);
      continue;
    }
    for (int y = t; y <= bot; ++y)
      for (int x = l; x <= r; ++x)
        segment_buckets[{x, y}].push_back(i);
  }
}
ShapeBounds brush_centerline_bounds(const BrushGeometry &g) {
  if (!g.path || !g.path->is_valid())
    return {INFINITY, INFINITY, -INFINITY, -INFINITY};
  auto b = g.path->bounds();
  return {g.origin_x + b[0] * g.scale_x, g.origin_y + b[1] * g.scale_y,
          g.origin_x + b[2] * g.scale_x, g.origin_y + b[3] * g.scale_y};
}
Result<PixelRect, Error> brush_geometry_bounds(const BrushGeometry &g) {
  const auto error = [] {
    return Result<PixelRect, Error>::failure(
        {ErrorCode::invalid_input, "BrushGeometry"});
  };
  if (!g.path || !g.path->is_valid() || !std::isfinite(g.origin_x) ||
      !std::isfinite(g.origin_y) || !std::isfinite(g.scale_x) ||
      !std::isfinite(g.scale_y) || g.scale_x <= 0 || g.scale_y <= 0)
    return error();
  if ((g.path->bounds()[0] == g.path->bounds()[2] && g.scale_x != 1) ||
      (g.path->bounds()[1] == g.path->bounds()[3] && g.scale_y != 1))
    return error();
  auto b = brush_centerline_bounds(g);
  constexpr double low = std::numeric_limits<int>::min(),
                   high = std::numeric_limits<int>::max() - 1.;
  if (!std::isfinite(b.left) || !std::isfinite(b.top) ||
      !std::isfinite(b.right) || !std::isfinite(b.bottom) || b.left < low ||
      b.top < low || b.right > high || b.bottom > high)
    return error();
  auto l = std::floor(b.left), t = std::floor(b.top), r = std::ceil(b.right),
       bot = std::ceil(b.bottom);
  if (r - l > high || bot - t > high)
    return error();
  return Result<PixelRect, Error>::success(
      {int(l), int(t), std::max(1, int(r - l)), std::max(1, int(bot - t))});
}
bool brush_centerline_inside(const BrushGeometry &g, PixelRect clip) {
  if (!brush_geometry_bounds(g) || !valid_annotation_clip(clip))
    return false;
  auto b = brush_centerline_bounds(g);
  return b.left >= clip.x && b.top >= clip.y && b.right <= clip.right() &&
         b.bottom <= clip.bottom();
}
PixelRect brush_paint_bounds(const AnnotationObject &o, AnnotationScale scale,
                             PixelRect clip) {
  if (!valid_annotation_clip(clip) || !o.brush_geometry ||
      !brush_geometry_bounds(*o.brush_geometry))
    return {};
  auto b = brush_centerline_bounds(*o.brush_geometry);
  auto r = stroke_margin(o, scale);
  auto l =
      std::clamp(std::floor(b.left - r), double(clip.x), double(clip.right()));
  auto t =
      std::clamp(std::floor(b.top - r), double(clip.y), double(clip.bottom()));
  auto right =
      std::clamp(std::ceil(b.right + r), double(clip.x), double(clip.right()));
  auto bottom = std::clamp(std::ceil(b.bottom + r), double(clip.y),
                           double(clip.bottom()));
  return {int(l), int(t), int(right - l), int(bottom - t)};
}
bool brush_hit_test(const AnnotationObject &o, StrokePoint p, double tolerance,
                    AnnotationScale scale) {
  if (!o.brush_geometry || !brush_geometry_bounds(*o.brush_geometry))
    return false;
  const auto &g = *o.brush_geometry;
  auto b = brush_centerline_bounds(g);
  double radius = stroke_margin(o, scale) + tolerance;
  if (p.x < b.left - radius || p.x > b.right + radius || p.y < b.top - radius ||
      p.y > b.bottom + radius)
    return false;
  std::vector<std::size_t> candidates = g.path->broad();
  const double lx = (p.x - g.origin_x) / g.scale_x,
               ly = (p.y - g.origin_y) / g.scale_y;
  const double query_left = std::floor((lx - radius / g.scale_x) / 64),
               query_right = std::floor((lx + radius / g.scale_x) / 64);
  const double query_top = std::floor((ly - radius / g.scale_y) / 64),
               query_bottom = std::floor((ly + radius / g.scale_y) / 64);
  const bool bounded =
      std::isfinite(query_left) && std::isfinite(query_right) &&
      std::isfinite(query_top) && std::isfinite(query_bottom) &&
      query_left >= std::numeric_limits<int>::min() &&
      query_right < std::numeric_limits<int>::max() &&
      query_top >= std::numeric_limits<int>::min() &&
      query_bottom < std::numeric_limits<int>::max() &&
      (query_right - query_left + 1) * (query_bottom - query_top + 1) <= 4096;
  if (!bounded) {
    candidates.resize(g.path->points.size());
    for (std::size_t i = 0; i < candidates.size(); ++i)
      candidates[i] = i;
  } else
    for (int y = int(query_top); y <= int(query_bottom); ++y)
      for (int x = int(query_left); x <= int(query_right); ++x) {
        auto it = g.path->buckets().find({x, y});
        if (it != g.path->buckets().end())
          candidates.insert(candidates.end(), it->second.begin(),
                            it->second.end());
      }
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()),
                   candidates.end());
  for (auto i : candidates) {
    auto a = brush_world_point(g, g.path->points[i ? i - 1 : 0]),
         z = brush_world_point(g, g.path->points[i]);
    double dx = z.x - a.x, dy = z.y - a.y, d = dx * dx + dy * dy;
    double position =
        d > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / d, 0., 1.)
              : 0;
    if (std::hypot(p.x - a.x - position * dx, p.y - a.y - position * dy) <=
        radius)
      return true;
  }
  return false;
}
AnnotationObject resize_brush(const AnnotationObject &o, AnnotationAdjustment h,
                              StrokePoint anchor, StrokePoint p,
                              PixelRect limits, AnnotationScale) {
  if (!valid_annotation_clip(limits) || !o.brush_geometry ||
      !std::isfinite(p.x) || !std::isfinite(p.y))
    return o;
  auto result = o;
  auto &g = *result.brush_geometry;
  auto b = brush_centerline_bounds(g);
  if (h == AnnotationAdjustment::move) {
    g.origin_x += std::clamp(p.x - anchor.x, double(limits.x) - b.left,
                             double(limits.right()) - b.right);
    g.origin_y += std::clamp(p.y - anchor.y, double(limits.y) - b.top,
                             double(limits.bottom()) - b.bottom);
  } else {
    const bool l = h == AnnotationAdjustment::west ||
                   h == AnnotationAdjustment::north_west ||
                   h == AnnotationAdjustment::south_west;
    const bool r = h == AnnotationAdjustment::east ||
                   h == AnnotationAdjustment::north_east ||
                   h == AnnotationAdjustment::south_east;
    const bool t = h == AnnotationAdjustment::north ||
                   h == AnnotationAdjustment::north_west ||
                   h == AnnotationAdjustment::north_east;
    const bool bot = h == AnnotationAdjustment::south ||
                     h == AnnotationAdjustment::south_west ||
                     h == AnnotationAdjustment::south_east;
    if ((l || r) && b.right > b.left) {
      double left = l ? std::clamp(b.left + p.x - anchor.x, double(limits.x),
                                   b.right - 1e-6)
                      : b.left;
      double right = r ? std::clamp(b.right + p.x - anchor.x, b.left + 1e-6,
                                    double(limits.right()))
                       : b.right;
      double factor = (right - left) / (b.right - b.left);
      g.scale_x *= factor;
      g.origin_x = left + (g.origin_x - b.left) * factor;
    }
    if ((t || bot) && b.bottom > b.top) {
      double top = t ? std::clamp(b.top + p.y - anchor.y, double(limits.y),
                                  b.bottom - 1e-6)
                     : b.top;
      double bottom = bot ? std::clamp(b.bottom + p.y - anchor.y, b.top + 1e-6,
                                       double(limits.bottom()))
                          : b.bottom;
      double factor = (bottom - top) / (b.bottom - b.top);
      g.scale_y *= factor;
      g.origin_y = top + (g.origin_y - b.top) * factor;
    }
  }
  auto bounds = brush_geometry_bounds(g);
  if (!bounds)
    return o;
  result.bounds = bounds.value();
  return result;
}
} // namespace hdrshot
