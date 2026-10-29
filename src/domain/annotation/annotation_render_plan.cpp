#include "domain/annotation/annotation_allocation_checkpoint.hpp"
#include "domain/annotation/annotation_render_plan.hpp"
#include "domain/annotation/annotation_geometry.hpp"
#include "domain/annotation/brush_coverage_kernel.hpp"
#include "domain/color/extended_p3_mapper.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

namespace hdrshot {
struct AnnotationRenderPlan::Provenance {
  DocumentRevision revision;
  std::vector<AnnotationObject> objects;
  PixelSize output;
  std::vector<AnnotationCoverageLayer> layers;
  PixelRect selection;
  AnnotationScale scale;
};
void AnnotationRenderPlan::bind_source(
    const AnnotationDocumentSnapshot &document) {
  provenance_ = std::make_shared<const Provenance>(
      Provenance{document.revision, document.objects, output_size_px,
                 ordered_layers, source_selection_rect_px, scale});
}
bool AnnotationRenderPlan::matches_source(
    const AnnotationDocumentSnapshot &document) const {
  if (!provenance_ || provenance_->revision != document.revision ||
      source_document_revision != document.revision ||
      provenance_->output != output_size_px ||
      provenance_->selection != source_selection_rect_px ||
      provenance_->scale != scale ||
      provenance_->objects.size() != document.objects.size() ||
      provenance_->layers.size() != ordered_layers.size())
    return false;
  for (std::size_t i = 0; i < document.objects.size(); ++i)
    if (!same_annotation_source(provenance_->objects[i], document.objects[i]))
      return false;
  for (std::size_t i = 0; i < ordered_layers.size(); ++i)
    if (!same_coverage_source(provenance_->layers[i], ordered_layers[i]))
      return false;
  return true;
}
// Defined only in this translation unit. There is no public raw-plan signer.
class AnnotationPlanUpdater {
public:
  static Result<AnnotationRenderPlan, Error>
  update(const AnnotationDocumentSnapshot &, const AnnotationDocumentSnapshot &,
         const AnnotationRenderPlan *, PixelRect, AnnotationScale,
         TextRasterizerPort *, AnnotationCoverageCache *,
         const ReusableAnnotationLayer *, bool);
};

namespace {

Error plan_error(const ErrorCode code, const char* reason, const ObjectId object_id = {}) {
  auto context = std::map<std::string, std::string>{{"reason", reason}};
  if (object_id.value != 0U) {
    context.emplace("objectId", std::to_string(object_id.value));
  }
  return Error{code, "AnnotationRenderPlanner", Retryability::never, std::move(context)};
}

bool inside_output(const PixelRect bounds, const PixelSize output) {
  return valid_annotation_clip(bounds) && bounds.x >= 0 && bounds.y >= 0 &&
         bounds.right() <= output.width && bounds.bottom() <= output.height;
}

bool inside_output(const PixelPoint point, const PixelSize output) {
  return point.x >= 0 && point.y >= 0 && point.x <= output.width && point.y <= output.height;
}

bool inside_selection(const PixelRect bounds, const PixelRect selection) {
  return valid_annotation_clip(bounds) && valid_annotation_clip(selection) &&
         bounds.x >= selection.x && bounds.y >= selection.y &&
         bounds.right() <= selection.right() &&
         bounds.bottom() <= selection.bottom();
}

AnnotationObject selection_local_object(
    AnnotationObject object,
    const PixelRect selection) {
  object.bounds.x -= selection.x;
  object.bounds.y -= selection.y;
  if (object.arrow_geometry.has_value()) {
    object.arrow_geometry->start.x -= selection.x;
    object.arrow_geometry->start.y -= selection.y;
    object.arrow_geometry->end.x -= selection.x;
    object.arrow_geometry->end.y -= selection.y;
  }
  if (object.brush_geometry) {
    object.brush_geometry->origin_x -= selection.x;
    object.brush_geometry->origin_y -= selection.y;
  }
  return object;
}

PixelRect intersect_output(const PixelRect bounds, const PixelSize output) {
  const auto left = std::clamp(bounds.x, 0, output.width);
  const auto top = std::clamp(bounds.y, 0, output.height);
  const auto right = std::clamp(bounds.right(), 0, output.width);
  const auto bottom = std::clamp(bounds.bottom(), 0, output.height);
  return PixelRect{left, top, std::max(0, right - left), std::max(0, bottom - top)};
}

double distance_to_segment(
    const double x,
    const double y,
    const double start_x,
    const double start_y,
    const double end_x,
    const double end_y) {
  const auto dx = end_x - start_x;
  const auto dy = end_y - start_y;
  const auto length_squared = dx * dx + dy * dy;
  if (length_squared <= std::numeric_limits<double>::epsilon()) {
    return std::hypot(x - start_x, y - start_y);
  }
  const auto position = std::clamp(
      ((x - start_x) * dx + (y - start_y) * dy) / length_squared, 0.0, 1.0);
  return std::hypot(
      x - (start_x + position * dx),
      y - (start_y + position * dy));
}

double distance_to_ellipse_boundary(
    const double x,
    const double y,
    const PixelRect bounds) {
  const auto radius_x = static_cast<double>(bounds.width) / 2.0;
  const auto radius_y = static_cast<double>(bounds.height) / 2.0;
  const auto center_x = static_cast<double>(bounds.x) + radius_x;
  const auto center_y = static_cast<double>(bounds.y) + radius_y;
  const auto point_x = std::abs(x - center_x);
  const auto point_y = std::abs(y - center_y);
  if (point_x <= std::numeric_limits<double>::epsilon() &&
      point_y <= std::numeric_limits<double>::epsilon()) {
    return std::min(radius_x, radius_y);
  }

  auto parameter = std::atan2(point_y * radius_x, point_x * radius_y);
  for (int iteration = 0; iteration < 12; ++iteration) {
    const auto sine = std::sin(parameter);
    const auto cosine = std::cos(parameter);
    const auto derivative =
        (radius_y * radius_y - radius_x * radius_x) * sine * cosine +
        radius_x * point_x * sine - radius_y * point_y * cosine;
    const auto second_derivative =
        (radius_y * radius_y - radius_x * radius_x) *
            (cosine * cosine - sine * sine) +
        radius_x * point_x * cosine + radius_y * point_y * sine;
    if (std::abs(second_derivative) <= 1e-12) {
      break;
    }
    const auto next_parameter = std::clamp(
        parameter - derivative / second_derivative,
        0.0,
        std::numbers::pi / 2.0);
    // Stop in physical-pixel units, far below one coverage_u8 step. Retain
    // the 12-iteration cap and endpoint checks for flat/degenerate cases.
    const auto converged = std::abs(next_parameter - parameter) *
        std::max(radius_x, radius_y) < 1e-8;
    parameter = next_parameter;
    if (converged) {
      break;
    }
  }

  const auto distance_at = [&](const double candidate) {
    return std::hypot(
        radius_x * std::cos(candidate) - point_x,
        radius_y * std::sin(candidate) - point_y);
  };
  return std::min({distance_at(parameter), distance_at(0.0),
                   distance_at(std::numbers::pi / 2.0)});
}

struct ArrowSegments {
  double start_x{};
  double start_y{};
  double end_x{};
  double end_y{};
  double first_x{};
  double first_y{};
  double second_x{};
  double second_y{};
};

ArrowSegments arrow_segments(const AnnotationObject &object,
                             AnnotationScale scale) {
  const auto segments = annotation_line_segments(object, scale);
  const auto a = segments.at(0), b = segments.size() > 1 ? segments.at(1) : a,
             c = segments.size() > 2 ? segments.at(2) : a;
  return {a.start.x, a.start.y, a.end.x, a.end.y,
          b.end.x,   b.end.y,   c.end.x, c.end.y};
}

double distance_to_shape(const AnnotationObject &object, const double x,
                         const double y, AnnotationScale scale) {
  const auto& bounds = object.bounds;
  if (object.kind == AnnotationKind::rectangle) {
    return std::min({
        distance_to_segment(x, y, bounds.x, bounds.y, bounds.right(), bounds.y),
        distance_to_segment(x, y, bounds.right(), bounds.y, bounds.right(), bounds.bottom()),
        distance_to_segment(x, y, bounds.right(), bounds.bottom(), bounds.x, bounds.bottom()),
        distance_to_segment(x, y, bounds.x, bounds.bottom(), bounds.x, bounds.y),
    });
  }
  if (object.kind == AnnotationKind::ellipse) {
    return distance_to_ellipse_boundary(x, y, bounds);
  }
  const auto segments = arrow_segments(object, scale);
  return std::min({
      distance_to_segment(
          x, y, segments.start_x, segments.start_y, segments.end_x, segments.end_y),
      distance_to_segment(
          x, y, segments.end_x, segments.end_y, segments.first_x, segments.first_y),
      distance_to_segment(
          x, y, segments.end_x, segments.end_y, segments.second_x, segments.second_y),
  });
}

PixelRect shape_scan_bounds(const AnnotationObject &object,
                            const PixelSize output, AnnotationScale scale) {
  const auto style = std::get<ShapeStyle>(object.style);
  const double margin =
      std::floor(style.line_width_dip * scale.value() / 2) + 2;
  double left = object.bounds.x, top = object.bounds.y,
         right = object.bounds.right(), bottom = object.bounds.bottom();
  if (is_rotatable(object) && object.transform != ShapeTransform{}) {
    auto bounds = shape_bounds(object, margin);
    const auto l =
        std::clamp(std::floor(bounds.left), 0., double(output.width));
    const auto t =
        std::clamp(std::floor(bounds.top), 0., double(output.height));
    const auto r =
        std::clamp(std::ceil(bounds.right), 0., double(output.width));
    const auto b =
        std::clamp(std::ceil(bounds.bottom), 0., double(output.height));
    return {int(l), int(t), int(r - l), int(b - t)};
  }
  if (object.kind == AnnotationKind::arrow) {
    const auto segments = arrow_segments(object, scale);
    left = std::min({segments.start_x, segments.end_x, segments.first_x,
                     segments.second_x});
    top = std::min({segments.start_y, segments.end_y, segments.first_y,
                    segments.second_y});
    right = std::max({segments.start_x, segments.end_x, segments.first_x,
                      segments.second_x});
    bottom = std::max({segments.start_y, segments.end_y, segments.first_y,
                       segments.second_y});
  }
  const auto l =
      std::clamp(std::floor(left) - margin, 0., double(output.width));
  const auto t =
      std::clamp(std::floor(top) - margin, 0., double(output.height));
  const auto r = std::clamp(std::floor(left) + std::ceil(right - left) + margin,
                            0., double(output.width));
  const auto b = std::clamp(std::floor(top) + std::ceil(bottom - top) + margin,
                            0., double(output.height));
  return {int(l), int(t), int(r - l), int(b - t)};
}

void append_row_spans(
    AnnotationCoverageLayer& layer,
    const std::int32_t y,
    const std::int32_t left,
    std::vector<std::uint8_t> row) {
  std::size_t cursor = 0;
  while (cursor < row.size()) {
    while (cursor < row.size() && row[cursor] == 0U) {
      ++cursor;
    }
    if (cursor == row.size()) {
      break;
    }
    const auto start = cursor;
    while (cursor < row.size() && row[cursor] != 0U) {
      ++cursor;
    }
    layer.spans.push_back(CoverageSpan{
        y,
        left + static_cast<std::int32_t>(start),
        std::vector<std::uint8_t>(row.begin() + static_cast<std::ptrdiff_t>(start),
                                  row.begin() + static_cast<std::ptrdiff_t>(cursor)),
    });
  }
}

void append_sparse_row(
    AnnotationCoverageLayer& layer,
    const std::int32_t y,
    std::vector<std::pair<std::int32_t, std::uint8_t>> samples) {
  if (samples.empty()) {
    return;
  }
  std::sort(samples.begin(), samples.end(), [](const auto& first, const auto& second) {
    return first.first < second.first;
  });
  std::size_t cursor = 0;
  while (cursor < samples.size()) {
    const auto start_x = samples[cursor].first;
    std::vector<std::uint8_t> coverage;
    coverage.push_back(samples[cursor].second);
    auto previous_x = start_x;
    ++cursor;
    while (cursor < samples.size()) {
      if (samples[cursor].first == previous_x) {
        coverage.back() = std::max(coverage.back(), samples[cursor].second);
        ++cursor;
        continue;
      }
      if (samples[cursor].first != previous_x + 1) {
        break;
      }
      previous_x = samples[cursor].first;
      coverage.push_back(samples[cursor].second);
      ++cursor;
    }
    layer.spans.push_back(CoverageSpan{y, start_x, std::move(coverage)});
  }
}

void append_ellipse_row(
    AnnotationCoverageLayer& layer,
    const AnnotationObject& object,
    const PixelRect scan_bounds,
    const std::int32_t y,
    const double half_width) {
  const auto radius_x = static_cast<double>(object.bounds.width) / 2.0;
  const auto radius_y = static_cast<double>(object.bounds.height) / 2.0;
  const auto center_x = static_cast<double>(object.bounds.x) + radius_x;
  const auto center_y = static_cast<double>(object.bounds.y) + radius_y;
  // Bound the Minkowski sum of the ellipse and the antialiased stroke disk.
  // Coverage is still evaluated with the exact geometric-distance function
  // below, so this only narrows the candidate work and cannot change pixels.
  const auto coverage_radius = half_width + 0.5;
  const auto dy = std::abs(static_cast<double>(y) + 0.5 - center_y);
  if (dy > radius_y + coverage_radius) {
    return;
  }
  const auto nearest_ellipse_y = std::max(0.0, dy - coverage_radius);
  const auto normalized_y = std::min(1.0, nearest_ellipse_y / radius_y);
  const auto outer_extent_x = radius_x *
      std::sqrt(std::max(0.0, 1.0 - normalized_y * normalized_y));
  // Do not clamp the sampled left half to the output. A clipped left stroke
  // can mirror to visible pixels on the right half (and vice versa).
  const auto start_x = static_cast<std::int32_t>(
      std::floor(center_x - outer_extent_x - coverage_radius)) - 1;
  const auto twice_center_x = object.bounds.x * 2 + object.bounds.width;
  const auto left_center_x = std::min(
      scan_bounds.right() - 1,
      static_cast<std::int32_t>(std::floor(
          (static_cast<double>(twice_center_x) - 1.0) / 2.0)));
  if (start_x > left_center_x) {
    return;
  }

  std::vector<std::pair<std::int32_t, std::uint8_t>> samples;
  samples.reserve(static_cast<std::size_t>(
      std::max(8, static_cast<int>(std::ceil(half_width * 8.0)))));
  bool saw_coverage = false;
  int empty_after_coverage = 0;
  for (auto x = start_x; x <= left_center_x; ++x) {
    const auto distance = distance_to_ellipse_boundary(
        static_cast<double>(x) + 0.5,
        static_cast<double>(y) + 0.5,
        object.bounds);
    const auto coverage = static_cast<std::uint8_t>(std::lround(
        std::clamp(half_width + 0.5 - distance, 0.0, 1.0) * 255.0));
    if (coverage == 0U) {
      if (saw_coverage && ++empty_after_coverage >= 2) {
        break;
      }
      continue;
    }
    saw_coverage = true;
    empty_after_coverage = 0;
    if (x >= scan_bounds.x && x < scan_bounds.right()) {
      samples.emplace_back(x, coverage);
    }
    const auto mirrored_x = twice_center_x - x - 1;
    if (mirrored_x != x && mirrored_x >= scan_bounds.x &&
        mirrored_x < scan_bounds.right()) {
      samples.emplace_back(mirrored_x, coverage);
    }
  }
  append_sparse_row(layer, y, std::move(samples));
}

void rasterize_transformed_shape(
    AnnotationCoverageLayer& layer, const AnnotationObject& object,
    const PixelRect scan, const double half_width) {
  // Generate conservative candidate bands around a polyline. Evaluate exact
  // inverse-transformed geometry only inside those bands, never every pixel of
  // a large rotated ellipse's interior. Chord error <= 0.05 physical pixel.
  std::vector<ShapePoint> contour;
  if (object.kind == AnnotationKind::rectangle) {
    const auto handles = shape_handles(object);
    for (int i : {0,2,4,6}) contour.push_back(handles[static_cast<std::size_t>(i)]);
  } else {
    const auto radius = std::max(object.bounds.width, object.bounds.height)*0.5;
    const auto count = std::max(16, static_cast<int>(std::ceil(
        std::numbers::pi*std::sqrt(radius/0.1))));
    contour.reserve(static_cast<std::size_t>(count));
    for (int i=0;i<count;++i) {
      const auto angle=2*std::numbers::pi*i/count;
      contour.push_back(shape_to_world(object,
          {object.bounds.width*0.5*std::cos(angle), object.bounds.height*0.5*std::sin(angle)}));
    }
  }
  using Interval=std::pair<int,int>;
  std::vector<std::vector<Interval>> rows(static_cast<std::size_t>(scan.height));
  const auto margin=half_width+0.5+0.05;
  for (std::size_t i=0;i<contour.size();++i) {
    const auto a=contour[i], b=contour[(i+1)%contour.size()];
    const auto first =
        static_cast<int>(std::clamp(std::floor(std::min(a.y, b.y) - margin),
                                    double(scan.y), double(scan.bottom())));
    const auto last =
        static_cast<int>(std::clamp(std::ceil(std::max(a.y, b.y) + margin),
                                    double(scan.y), double(scan.bottom())));
    for(int y=first;y<last;++y) {
      double start_x=a.x, end_x=b.x;
      if (std::abs(a.y-b.y)>1e-10) {
        auto t0=(y+0.5-margin-a.y)/(b.y-a.y);
        auto t1=(y+0.5+margin-a.y)/(b.y-a.y);
        if(t0>t1) std::swap(t0,t1);
        if(t1<0 || t0>1) continue;
        start_x=a.x+(b.x-a.x)*std::clamp(t0,0.0,1.0);
        end_x=a.x+(b.x-a.x)*std::clamp(t1,0.0,1.0);
      }
      const auto left = static_cast<int>(
          std::clamp(std::floor(std::min(start_x, end_x) - margin),
                     double(scan.x), double(scan.right())));
      const auto right = static_cast<int>(
          std::clamp(std::ceil(std::max(start_x, end_x) + margin),
                     double(scan.x), double(scan.right())));
      if(left<right) rows[static_cast<std::size_t>(y-scan.y)].emplace_back(left,right);
    }
  }
  const auto center=shape_center(object);
  const auto local_center=ShapePoint{object.bounds.x+object.bounds.width*0.5,
                                    object.bounds.y+object.bounds.height*0.5};
  const auto c=std::cos(object.transform.rotation_radians), s=std::sin(object.transform.rotation_radians);
  for(int y=scan.y;y<scan.bottom();++y) {
    auto& intervals=rows[static_cast<std::size_t>(y-scan.y)];
    std::sort(intervals.begin(),intervals.end());
    for(std::size_t i=0;i<intervals.size();) {
      const auto left=intervals[i].first;
      auto right=intervals[i++].second;
      while(i<intervals.size() && intervals[i].first<=right) right=std::max(right,intervals[i++].second);
      std::vector<std::uint8_t> samples(static_cast<std::size_t>(right-left));
      for(int x=left;x<right;++x) {
        const auto dx=x+0.5-center.x, dy=y+0.5-center.y;
        const auto distance = distance_to_shape(
            object, local_center.x + c * dx + s * dy,
            local_center.y - s * dx + c * dy, AnnotationScale::from(1).value());
        samples[static_cast<std::size_t>(x-left)]=static_cast<std::uint8_t>(std::lround(
            std::clamp(half_width+0.5-distance,0.0,1.0)*255.0));
      }
      append_row_spans(layer,y,left,std::move(samples));
    }
  }
}

Result<AnnotationCoverageLayer, Error>
rasterize_text(const AnnotationObject &object,
               const double capture_pixels_per_dip,
               TextRasterizerPort *const text_rasterizer) {
  if (text_rasterizer == nullptr) {
    return Result<AnnotationCoverageLayer, Error>::failure(
        plan_error(ErrorCode::unsupported_encoding, "text_rasterizer_not_connected", object.id));
  }
  const auto style = std::get<TextStyle>(object.style);
  auto mask = text_rasterizer->rasterize(TextRasterRequest{
      object.id,
      object.text,
      PixelSize{object.bounds.width, object.bounds.height},
      style.font_size_dip,
      capture_pixels_per_dip,
  });
  if (!mask) {
    return Result<AnnotationCoverageLayer, Error>::failure(mask.error());
  }
  const auto expected_size = static_cast<std::size_t>(object.bounds.width) *
      static_cast<std::size_t>(object.bounds.height);
  if (mask.value().object_id != object.id ||
      mask.value().size_px != PixelSize{object.bounds.width, object.bounds.height} ||
      mask.value().coverage_u8.size() != expected_size || mask.value().font_identity.empty()) {
    return Result<AnnotationCoverageLayer, Error>::failure(
        plan_error(ErrorCode::state_inconsistent, "invalid_text_mask_receipt", object.id));
  }
  AnnotationCoverageLayer layer{
      object.id,
      object.kind,
      object.bounds,
      style.color_srgb_rgb,
      {},
      mask.value().font_identity,
  };
  for (std::int32_t row = 0; row < object.bounds.height; ++row) {
    const auto begin = static_cast<std::size_t>(row) *
        static_cast<std::size_t>(object.bounds.width);
    append_row_spans(
        layer,
        object.bounds.y + row,
        object.bounds.x,
        std::vector<std::uint8_t>(
            mask.value().coverage_u8.begin() + static_cast<std::ptrdiff_t>(begin),
            mask.value().coverage_u8.begin() +
                static_cast<std::ptrdiff_t>(begin + static_cast<std::size_t>(object.bounds.width))));
  }
  return Result<AnnotationCoverageLayer, Error>::success(std::move(layer));
}

}  // namespace

Result<AnnotationCoverageLayer, Error>
AnnotationRenderPlanner::rasterize_shape(const AnnotationObject &object,
                                         const PixelSize output_size_px,
                                         AnnotationScale scale) {
  if (object.kind == AnnotationKind::brush) {
    if (!valid_style(object.kind, object.style) || !object.brush_geometry ||
        !brush_centerline_inside(
            *object.brush_geometry,
            {0, 0, output_size_px.width, output_size_px.height}))
      return Result<AnnotationCoverageLayer, Error>::failure(
          plan_error(ErrorCode::invalid_input, "invalid_brush", object.id));
    BrushCoverageAccumulator accumulator;
    std::vector<StrokeSegment> segments;
    const auto &g = *object.brush_geometry;
    auto previous = brush_world_point(g, g.path->points.front());
    for (auto p : g.path->points) {
      auto world = brush_world_point(g, p);
      segments.push_back({previous, world});
      previous = world;
    }
    auto width = stroke_width_px(object, scale);
    if (!width)
      return Result<AnnotationCoverageLayer, Error>::failure(width.error());
    auto accumulated = accumulate_brush_segments(
        accumulator, segments, width.value() * .5,
        {0, 0, output_size_px.width, output_size_px.height});
    if (!accumulated)
      return Result<AnnotationCoverageLayer, Error>::failure(
          accumulated.error());
    return Result<AnnotationCoverageLayer, Error>::success(
        freeze_brush_coverage(
            accumulator, object.id,
            std::get<BrushStyle>(object.style).color_srgb_rgb,
            {0, 0, output_size_px.width, output_size_px.height}));
  }
  if (output_size_px.width <= 0 || output_size_px.height <= 0 ||
      object.kind == AnnotationKind::text ||
      !std::holds_alternative<ShapeStyle>(object.style) ||
      !valid_geometry(object) || object.bounds.empty() ||
      !inside_output(is_rotatable(object) ? shape_pixel_bounds(object)
                                          : object.bounds,
                     output_size_px) ||
      ((object.kind == AnnotationKind::arrow ||
        object.kind == AnnotationKind::line) &&
       (!object.arrow_geometry.has_value() ||
        !inside_output(object.arrow_geometry->start, output_size_px) ||
        !inside_output(object.arrow_geometry->end, output_size_px)))) {
    return Result<AnnotationCoverageLayer, Error>::failure(
        plan_error(ErrorCode::invalid_input, "shape_out_of_selection", object.id));
  }
  const auto style = std::get<ShapeStyle>(object.style);
  auto physical =
      to_capture_px(std::max(12., 4. * style.line_width_dip), scale);
  if (!physical || physical.value() > std::numeric_limits<int>::max() / 8.)
    return Result<AnnotationCoverageLayer, Error>::failure(plan_error(
        ErrorCode::invalid_input, "stroke_extent_overflow", object.id));
  const auto scan_bounds = shape_scan_bounds(object, output_size_px, scale);
  AnnotationCoverageLayer layer{
      object.id,
      object.kind,
      scan_bounds,
      style.color_srgb_rgb,
      {},
      "hdrshot-shape-coverage-v1",
  };
  const auto half_width =
      static_cast<double>(style.line_width_dip) * scale.value() / 2.0;
  if (is_rotatable(object) && object.transform != ShapeTransform{}) {
    rasterize_transformed_shape(layer, object, scan_bounds, half_width);
    return Result<AnnotationCoverageLayer, Error>::success(std::move(layer));
  }

  // Conservative row intervals around segments; evaluate the original coverage
  // expression within their union. Intersections are evaluated exactly once.
  struct Segment {
    double ax, ay, bx, by;
  };
  std::vector<Segment> segments;
  if (object.kind == AnnotationKind::arrow) {
    const auto a = arrow_segments(object, scale);
    segments = {{a.start_x, a.start_y, a.end_x, a.end_y},
                {a.end_x, a.end_y, a.first_x, a.first_y},
                {a.end_x, a.end_y, a.second_x, a.second_y}};
  } else if (object.kind == AnnotationKind::line) {
    const auto g = *object.arrow_geometry;
    segments = {{double(g.start.x), double(g.start.y), double(g.end.x),
                 double(g.end.y)}};
  } else if (object.kind == AnnotationKind::rectangle) {
    const auto b = object.bounds;
    const double l = b.x, t = b.y, rr = b.right(), bb = b.bottom();
    segments = {{l, t, rr, t}, {rr, t, rr, bb}, {rr, bb, l, bb}, {l, bb, l, t}};
  }
  const double radius = half_width + 0.5;
  for (int y = scan_bounds.y; y < scan_bounds.bottom(); ++y) {
    if (object.kind == AnnotationKind::ellipse) {
      append_ellipse_row(layer, object, scan_bounds, y, half_width);
      continue;
    }
    std::vector<std::pair<int, int>> intervals;
    const double py = y + 0.5;
    for (const auto& a : segments) {
      double low = 0, high = 1;
      const double dy = a.by - a.ay;
      if (dy == 0) {
        if (std::abs(py - a.ay) > radius) continue;
      } else {
        double t0 = (py - radius - a.ay) / dy, t1 = (py + radius - a.ay) / dy;
        if (t0 > t1) std::swap(t0, t1);
        low = std::max(0.0, t0);
        high = std::min(1.0, t1);
        if (low > high) continue;
      }
      const double x0 = a.ax + (a.bx - a.ax) * low,
                   x1 = a.ax + (a.bx - a.ax) * high;
      const int left =
          int(std::clamp(std::floor(std::min(x0, x1) - radius - .5) - 1,
                         double(scan_bounds.x), double(scan_bounds.right())));
      const int right =
          int(std::clamp(std::ceil(std::max(x0, x1) + radius - .5) + 2,
                         double(scan_bounds.x), double(scan_bounds.right())));
      if (left < right) intervals.emplace_back(left, right);
    }
    std::sort(intervals.begin(), intervals.end());
    std::vector<std::pair<int, int>> merged;
    for (auto v : intervals) {
      if (!merged.empty() && v.first <= merged.back().second)
        merged.back().second = std::max(merged.back().second, v.second);
      else
        merged.push_back(v);
    }
    std::vector<std::pair<int, std::uint8_t>> samples;
    for (auto [left, right] : merged)
      for (int x = left; x < right; ++x) {
        double distance = std::numeric_limits<double>::infinity();
        for (const auto& a : segments)
          distance = std::min(distance, distance_to_segment(x + 0.5, py, a.ax,
                                                            a.ay, a.bx, a.by));
        const auto amount = std::uint8_t(
            std::lround(std::clamp(radius - distance, 0.0, 1.0) * 255.0));
        if (amount) samples.emplace_back(x, amount);
      }
    append_sparse_row(layer, y, std::move(samples));
  }
  return Result<AnnotationCoverageLayer, Error>::success(std::move(layer));
}

Result<AnnotationCoverageLayer, Error>
AnnotationCoverageCache::get(const AnnotationObject &object, PixelSize output,
                             AnnotationScale scale) {
  // Bounded editor-owned geometry cache. Only integer translations with
  // identical clipping footprints can reuse coverage; edits and edge clipping
  // miss it.
  auto& cache = entries_;
  if (auto style = std::get_if<ShapeStyle>(&object.style)) {
    auto physical =
        to_capture_px(std::max(12., 4. * style->line_width_dip), scale);
    if (!physical || physical.value() > std::numeric_limits<int>::max() / 8.)
      return Result<AnnotationCoverageLayer, Error>::failure(plan_error(
          ErrorCode::invalid_input, "stroke_extent_overflow", object.id));
  }
  const bool valid =
      output.width > 0 && output.height > 0 &&
      object.kind != AnnotationKind::text &&
      std::holds_alternative<ShapeStyle>(object.style) &&
      valid_shape_transform(object) && !object.bounds.empty() &&
      inside_output(
          is_rotatable(object) ? shape_pixel_bounds(object) : object.bounds,
          output) &&
      (object.kind != AnnotationKind::arrow ||
       (object.arrow_geometry &&
        inside_output(object.arrow_geometry->start, output) &&
        inside_output(object.arrow_geometry->end, output)));
  if (!valid)
    return AnnotationRenderPlanner::rasterize_shape(object, output, scale);
  if (object.transform != ShapeTransform{})
    return AnnotationRenderPlanner::rasterize_shape(object, output, scale);
  auto key = object;
  const int x = key.bounds.x, y = key.bounds.y;
  key.bounds.x = 0;
  key.bounds.y = 0;
  if (key.arrow_geometry) {
    key.arrow_geometry->start.x -= x;
    key.arrow_geometry->end.x -= x;
    key.arrow_geometry->start.y -= y;
    key.arrow_geometry->end.y -= y;
  }
  auto clip = shape_scan_bounds(object, output, scale);
  clip.x -= x;
  clip.y -= y;
  for (const auto& e : cache)
    if (e.key == key && e.clip == clip && e.scale == scale) {
      auto layer = e.layer;
      const int dx = x - e.x, dy = y - e.y;
      layer.bounds_px.x += dx;
      layer.bounds_px.y += dy;
      for (auto& span : layer.spans) {
        span.x += dx;
        span.y += dy;
      }
      return Result<AnnotationCoverageLayer, Error>::success(std::move(layer));
    }
  auto result = AnnotationRenderPlanner::rasterize_shape(object, output, scale);
  if (result) {
    std::size_t bytes = 0;
    for (const auto& span : result.value().spans)
      bytes += span.coverage_u8.size() + sizeof(CoverageSpan);
    if (bytes <= 1024 * 1024) {
      std::erase_if(cache, [&](const Entry& e) { return e.key.id == key.id; });
      if (cache.size() >= 8) cache.erase(cache.begin());
      cache.push_back({std::move(key), clip, result.value(), x, y, scale});
    }
  }
  return result;
}

Result<AnnotationRenderPlan, Error>
AnnotationRenderPlanner::build(const AnnotationDocumentSnapshot &annotations,
                               const PixelRect selection_rect_px,
                               const double capture_pixels_per_dip,
                               TextRasterizerPort *const text_rasterizer,
                               AnnotationCoverageCache *const coverage_cache) {
  const auto output_size_px = PixelSize{
      selection_rect_px.width, selection_rect_px.height};
  if (!valid_annotation_clip(selection_rect_px) ||
      !std::isfinite(capture_pixels_per_dip) || capture_pixels_per_dip <= 0.0) {
    return Result<AnnotationRenderPlan, Error>::failure(
        plan_error(ErrorCode::invalid_input, "invalid_plan_contract"));
  }
  AnnotationRenderPlan plan{
      annotations.revision,
      output_size_px,
      {},
      selection_rect_px,
      AnnotationScale::from(capture_pixels_per_dip).value()};
  plan.ordered_layers.reserve(annotations.objects.size());
  for (const auto& object : annotations.objects) {
    if (!valid_shape_transform(object) ||
        (object.kind == AnnotationKind::brush
             ? (!object.brush_geometry ||
                !brush_centerline_inside(*object.brush_geometry,
                                         selection_rect_px))
             : !inside_selection(is_rotatable(object)
                                     ? shape_pixel_bounds(object)
                                     : object.bounds,
                                 selection_rect_px))) {
      return Result<AnnotationRenderPlan, Error>::failure(
          plan_error(ErrorCode::invalid_input, "annotation_out_of_selection", object.id));
    }
    const auto local_object = selection_local_object(object, selection_rect_px);
    auto layer =
        local_object.kind == AnnotationKind::text
            ? rasterize_text(local_object, capture_pixels_per_dip,
                             text_rasterizer)
            : (coverage_cache
                   ? coverage_cache->get(local_object, output_size_px,
                                         plan.scale)
                   : rasterize_shape(local_object, output_size_px, plan.scale));
    if (!layer) {
      return Result<AnnotationRenderPlan, Error>::failure(layer.error());
    }
    plan.ordered_layers.push_back(std::move(layer.value()));
  }
  plan.bind_source(annotations);
  return Result<AnnotationRenderPlan, Error>::success(std::move(plan));
}

Result<AnnotationRenderPlan, Error> AnnotationRenderPlanner::rebase(
    const AnnotationRenderPlan& render_plan,
    const PixelRect selection_rect_px) {
  if (!valid_annotation_clip(render_plan.source_selection_rect_px) ||
      !valid_annotation_clip(selection_rect_px)) {
    return Result<AnnotationRenderPlan, Error>::failure(
        plan_error(ErrorCode::invalid_input, "invalid_rebase_selection"));
  }
  const PixelSize output_size_px{selection_rect_px.width,
                                 selection_rect_px.height};
  const auto delta_x = std::int64_t(render_plan.source_selection_rect_px.x) -
                       selection_rect_px.x;
  const auto delta_y = std::int64_t(render_plan.source_selection_rect_px.y) -
                       selection_rect_px.y;
  auto rebased = render_plan;
  rebased.provenance_.reset();
  rebased.output_size_px = output_size_px;
  rebased.source_selection_rect_px = selection_rect_px;
  for (auto& layer : rebased.ordered_layers) {
    const auto left =
        std::clamp(std::int64_t(layer.bounds_px.x) + delta_x, std::int64_t{0},
                   std::int64_t(output_size_px.width));
    const auto top =
        std::clamp(std::int64_t(layer.bounds_px.y) + delta_y, std::int64_t{0},
                   std::int64_t(output_size_px.height));
    const auto right = std::clamp(
        std::int64_t(layer.bounds_px.x) + layer.bounds_px.width + delta_x,
        std::int64_t{0}, std::int64_t(output_size_px.width));
    const auto bottom = std::clamp(
        std::int64_t(layer.bounds_px.y) + layer.bounds_px.height + delta_y,
        std::int64_t{0}, std::int64_t(output_size_px.height));
    layer.bounds_px = {int(left), int(top),
                       int(std::max(std::int64_t{0}, right - left)),
                       int(std::max(std::int64_t{0}, bottom - top))};
    for (auto& span : layer.spans) {
      const auto x = std::int64_t(span.x) + delta_x,
                 y = std::int64_t(span.y) + delta_y;
      if (!span.valid_view() || x < 0 || y < 0 ||
          y >= output_size_px.height ||
          span.length() > std::size_t(output_size_px.width) ||
          x > output_size_px.width - std::int64_t(span.length())) {
        return Result<AnnotationRenderPlan, Error>::failure(
            plan_error(ErrorCode::invalid_input, "rebase_excludes_annotation",
                       layer.object_id));
      }
      span.x = int(x);
      span.y = int(y);
    }
  }
  return Result<AnnotationRenderPlan, Error>::success(std::move(rebased));
}

Result<AnnotationPixelPlan, Error> AnnotationRenderPlanner::build_pixel_plan(
    const AnnotationRenderPlan& render_plan) {
  try {
  const auto size = render_plan.output_size_px;
  if (size.width <= 0 || size.height <= 0) {
    return Result<AnnotationPixelPlan, Error>::failure(
        plan_error(ErrorCode::invalid_input, "invalid_pixel_plan_size"));
  }
  const auto width = static_cast<std::size_t>(size.width);
  const auto height = static_cast<std::size_t>(size.height);
  if (width > std::numeric_limits<std::size_t>::max() / height) {
    return Result<AnnotationPixelPlan, Error>::failure(
        plan_error(ErrorCode::invalid_input, "pixel_plan_size_overflow"));
  }

  if (render_plan.ordered_layers.size() >=
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return Result<AnnotationPixelPlan, Error>::failure(
        plan_error(ErrorCode::invalid_input, "too_many_annotation_layers"));
  }
  struct RowCoverage {
    std::size_t layer_index{};
    const CoverageSpan* span{};
    int left{}, right{};
    bool opaque{};
  };
  std::vector<std::vector<RowCoverage>> spans_by_row(height);
  std::vector<std::array<float, 3>> colors;
  for (std::size_t layer_index = 0; layer_index < render_plan.ordered_layers.size(); ++layer_index) {
    const auto& layer = render_plan.ordered_layers[layer_index];
    colors.push_back(ExtendedP3Mapper::annotation_linear_display_p3(layer.color_srgb_rgb));
    int previous_y=-1, previous_end=0;
    for (const auto& span : layer.spans) {
      if (!span.valid_view() || span.y < 0 || span.y >= size.height || span.x < 0 ||
          std::int64_t(span.x) + std::int64_t(span.length()) > size.width ||
          span.y < previous_y || (span.y == previous_y && span.x < previous_end))
        return Result<AnnotationPixelPlan,Error>::failure(plan_error(
            ErrorCode::state_inconsistent,"coverage_span_out_of_bounds",layer.object_id));
      previous_y=span.y; previous_end=span.x+int(span.length());
      auto& row=spans_by_row[std::size_t(span.y)];
      if (span.is_opaque()) {
        row.push_back({layer_index,&span,span.x,previous_end,true});
      } else {
        // Legacy shape/text payload may contain zero or 255. Normalize its
        // runs once; brush interiors already arrive as constant intervals.
        std::size_t i=0;
        while(i<span.length()) {
          auto kind=span.coverage_at(i)==0?0:span.coverage_at(i)==255?2:1;
          auto start=i++;
          while(i<span.length() && (span.coverage_at(i)==0?0:span.coverage_at(i)==255?2:1)==kind) ++i;
          if(kind) row.push_back({layer_index,&span,span.x+int(start),span.x+int(i),kind==2});
        }
      }
    }
  }
  AnnotationPixelPlan pixel_plan{render_plan.source_document_revision,size,{}, {}};
  for (int y=0;y<size.height;++y) {
    const auto& row=spans_by_row[std::size_t(y)];
    if(row.empty()) { pixel_plan.source_visible_spans.push_back({y,0,size.width});continue; }
    struct RowEvent { int x{}; bool start{}; const RowCoverage* run{}; };
    std::vector<RowEvent> events;
    if(row.size()>(events.max_size()-2)/2)
      return Result<AnnotationPixelPlan,Error>::failure(plan_error(ErrorCode::invalid_input,"row_event_count_overflow"));
    annotation_allocation_checkpoint(AnnotationAllocationStage::plan_events);
    events.reserve(row.size()*2+2);
    events.push_back({0,false,nullptr});events.push_back({size.width,false,nullptr});
    // row is complete before pointers are taken; it never grows during sweep.
    for(const auto& run:row) { events.push_back({run.left,true,&run});events.push_back({run.right,false,&run}); }
    std::sort(events.begin(),events.end(),[](const auto& a,const auto& b) {
      if(a.x!=b.x) return a.x<b.x;
      return a.start<b.start; // every end at x precedes every start at x
    });
    std::vector<const RowCoverage*> active;
    annotation_allocation_checkpoint(AnnotationAllocationStage::plan_active);
    active.reserve(std::min(row.size(),render_plan.ordered_layers.size()));
    std::size_t event_index=0;
    while(event_index<events.size()) {
      const int left=events[event_index].x;
      // Resolve only after the entire same-x group has been applied.
      while(event_index<events.size()&&events[event_index].x==left) {
        const auto event=events[event_index++];
        if(!event.run) continue;
        auto where=std::lower_bound(active.begin(),active.end(),event.run->layer_index,
            [](const auto* run,std::size_t layer){return run->layer_index<layer;});
        if(event.start) active.insert(where,event.run);
        else if(where!=active.end()&&*where==event.run) active.erase(where);
      }
      if(event_index==events.size()) break;
      const int right=events[event_index].x;
      if(active.empty()) {
        auto& source=pixel_plan.source_visible_spans;
        if(!source.empty()&&source.back().y==y&&source.back().x+source.back().length==left)
          source.back().length+=right-left;
        else source.push_back({y,left,right-left});
        continue;
      }
      // row insertion preserves document order. The last opaque run erases
      // all contributions below it; partial layers above it remain samples
      // even when remaining source weight is zero.
      auto owner=active.back()->layer_index;
      const auto& layer=render_plan.ordered_layers[owner];
      bool constant=active.back()->opaque;
      auto& owned=pixel_plan.annotation_owned_spans;
      bool join=!owned.empty()&&owned.back().y==y&&owned.back().x+owned.back().length==left &&
          owned.back().object_id==layer.object_id&&owned.back().color_srgb_rgb==layer.color_srgb_rgb &&
          owned.back().edge_samples.empty()==constant;
      if(join) owned.back().length+=right-left;
      else owned.push_back(AnnotationOwnedSpan{y,left,right-left,layer.object_id,layer.color_srgb_rgb});
      if(constant) continue;
      std::size_t first=0;
      for(std::size_t j=0;j<active.size();++j) if(active[j]->opaque) first=j;
      auto& samples=owned.back().edge_samples;
      // push_back retains geometric growth across adjacent event intervals;
      // exact reserve per interval would repeatedly copy the same AA samples.
      for(int x=left;x<right;++x) {
        std::array<float,4> value{0,0,0,1};
        for(std::size_t j=first;j<active.size();++j) {
          const auto& run=*active[j];
          float alpha=run.opaque?1.F:float(run.span->coverage_at(std::size_t(x-run.span->x)))/255.F;
          const auto& color=colors[run.layer_index];
          for(int c=0;c<3;++c) value[c]=alpha*color[c]+(1.F-alpha)*value[c];
          value[3]*=1.F-alpha;
        }
        samples.push_back(value);
      }
    }
  }
  return Result<AnnotationPixelPlan, Error>::success(std::move(pixel_plan));
  } catch (const std::bad_alloc&) {
    return Result<AnnotationPixelPlan,Error>::failure(plan_error(ErrorCode::state_inconsistent,"pixel_plan_allocation_failed"));
  }
}

}  // namespace hdrshot

namespace hdrshot {
namespace {
bool valid_layer(const AnnotationCoverageLayer &layer, PixelRect clip) {
  if (!valid_annotation_clip(clip, true))
    return false;
  if (!layer.spans.empty() &&
      !inside_selection(layer.bounds_px, {0, 0, clip.width, clip.height}))
    return false;
  int y = -1, end = 0;
  for (const auto &span : layer.spans) {
    if (!span.valid_view() || span.length() == 0 ||
        span.y < 0 || span.y >= clip.height || span.x < 0 ||
        std::int64_t(span.x) + span.length() >
            std::uint64_t(clip.width) ||
        span.y < y || (span.y == y && span.x < end))
      return false;
    y = span.y;
    end = span.x + int(span.length());
  }
  return true;
}
bool fits(const AnnotationObject &o, PixelRect clip) {
  return o.kind == AnnotationKind::brush
             ? o.brush_geometry &&
                   brush_centerline_inside(*o.brush_geometry, clip)
             : inside_selection(
                   is_rotatable(o) ? shape_pixel_bounds(o) : o.bounds, clip);
}
} // namespace
bool brush_clip_changed(const AnnotationObject &o, PixelRect a, PixelRect b,
                        AnnotationScale scale) {
  return brush_paint_bounds(o, scale, a) != brush_paint_bounds(o, scale, b);
}
namespace {
std::optional<AnnotationCoverageLayer> translate_coverage_layer_if_compatible(
    const AnnotationObject &origin, const AnnotationObject &target,
    const AnnotationCoverageLayer &layer, PixelRect selection,
    AnnotationScale scale) {
  if (origin.kind != AnnotationKind::brush ||
      target.kind != AnnotationKind::brush || !origin.brush_geometry ||
      !target.brush_geometry || origin.style != target.style ||
      origin.id != target.id)
    return {};
  const auto &a = *origin.brush_geometry;
  const auto &b = *target.brush_geometry;
  if (a.path != b.path || a.scale_x != b.scale_x || a.scale_y != b.scale_y)
    return {};
  double dx = b.origin_x - a.origin_x, dy = b.origin_y - a.origin_y;
  if (!std::isfinite(dx) || !std::isfinite(dy) || dx != std::round(dx) ||
      dy != std::round(dy) || dx < std::numeric_limits<int>::min() ||
      dx > std::numeric_limits<int>::max() ||
      dy < std::numeric_limits<int>::min() ||
      dy > std::numeric_limits<int>::max())
    return {};
  auto old = brush_paint_bounds(origin, scale, selection),
       next = brush_paint_bounds(target, scale, selection);
  const auto translated_x = std::int64_t(old.x) + std::int64_t(dx);
  const auto translated_y = std::int64_t(old.y) + std::int64_t(dy);
  if (translated_x < std::numeric_limits<int>::min() ||
      translated_x > std::numeric_limits<int>::max() ||
      translated_y < std::numeric_limits<int>::min() ||
      translated_y > std::numeric_limits<int>::max())
    return {};
  old.x = int(translated_x);
  old.y = int(translated_y);
  if (old != next)
    return {};
  auto result = layer;
  result.bounds_px.x += int(dx);
  result.bounds_px.y += int(dy);
  for (auto &span : result.spans) {
    span.x += int(dx);
    span.y += int(dy);
  }
  if (!valid_layer(result, selection))
    return {};
  return result;
}
} // namespace
Result<AnnotationRenderPlan, Error> AnnotationPlanUpdater::update(
    const AnnotationDocumentSnapshot &previous,
    const AnnotationDocumentSnapshot &next, const AnnotationRenderPlan *plan,
    PixelRect clip, AnnotationScale scale, TextRasterizerPort *text,
    AnnotationCoverageCache *cache, const ReusableAnnotationLayer *reuse,
    bool committed) {
  if (!valid_annotation_clip(clip, true) ||
      (plan &&
       (!plan->matches_source(previous) ||
        !valid_annotation_clip(plan->source_selection_rect_px, true))) ||
      (reuse && !reuse->has_valid_provenance()))
    return Result<AnnotationRenderPlan, Error>::failure(plan_error(
        ErrorCode::invalid_input, "unproven_or_invalid_edit_source"));
  if (clip.empty()) {
    if (next.objects.empty() && !reuse) {
      AnnotationRenderPlan empty{
          next.revision, {clip.width, clip.height}, {}, clip, scale};
      if (committed)
        empty.bind_source(next);
      return Result<AnnotationRenderPlan, Error>::success(std::move(empty));
    }
    return Result<AnnotationRenderPlan, Error>::failure(
        plan_error(ErrorCode::invalid_input, "empty_selection"));
  }
  if (plan && (plan->source_document_revision != previous.revision ||
               plan->output_size_px !=
                   PixelSize{plan->source_selection_rect_px.width,
                             plan->source_selection_rect_px.height}))
    return Result<AnnotationRenderPlan, Error>::failure(
        plan_error(ErrorCode::invalid_input, "inconsistent_previous_plan"));
  if (plan) {
    if (plan->ordered_layers.size() != previous.objects.size())
      return Result<AnnotationRenderPlan, Error>::failure(
          plan_error(ErrorCode::invalid_input, "previous_layer_count"));
    for (std::size_t i = 0; i < previous.objects.size(); ++i)
      if (plan->ordered_layers[i].object_id != previous.objects[i].id ||
          plan->ordered_layers[i].kind != previous.objects[i].kind ||
          plan->ordered_layers[i].color_srgb_rgb !=
              std::visit([](auto style) { return style.color_srgb_rgb; },
                         previous.objects[i].style) ||
          !valid_layer(plan->ordered_layers[i], plan->source_selection_rect_px))
        return Result<AnnotationRenderPlan, Error>::failure(
            plan_error(ErrorCode::invalid_input, "previous_layer_identity"));
  }
  if (reuse && std::none_of(next.objects.begin(), next.objects.end(),
                            [&](const auto &object) {
                              return object.id == reuse->object.id;
                            }))
    return Result<AnnotationRenderPlan, Error>::failure(
        plan_error(ErrorCode::invalid_input, "unused_reuse"));
  AnnotationRenderPlan result{
      next.revision, {clip.width, clip.height}, {}, clip, scale};
  for (const auto &object : next.objects) {
    if (!valid_geometry(object) || !valid_style(object.kind, object.style) ||
        !fits(object, clip))
      return Result<AnnotationRenderPlan, Error>::failure(
          plan_error(ErrorCode::invalid_input, "invalid_object", object.id));
    if (reuse && reuse->object.id == object.id) {
      if (!same_annotation_source(reuse->object, object) ||
          reuse->clip != clip || reuse->scale != scale ||
          reuse->layer.object_id != object.id ||
          reuse->layer.kind != object.kind ||
          reuse->layer.color_srgb_rgb !=
              std::visit([](auto s) { return s.color_srgb_rgb; },
                         object.style) ||
          !valid_layer(reuse->layer, clip) ||
          (object.kind == AnnotationKind::brush &&
           reuse->trajectory_revision == 0))
        return Result<AnnotationRenderPlan, Error>::failure(
            plan_error(ErrorCode::invalid_input, "invalid_reuse", object.id));
      result.ordered_layers.push_back(reuse->layer);
      continue;
    }
    auto old = std::find_if(previous.objects.begin(), previous.objects.end(),
                            [&](const auto &o) { return o.id == object.id; });
    if (plan && plan->scale == scale && old != previous.objects.end()) {
      auto layer =
          std::find_if(plan->ordered_layers.begin(), plan->ordered_layers.end(),
                       [&](const auto &l) { return l.object_id == object.id; });
      if (layer != plan->ordered_layers.end()) {
        if (same_annotation_source(*old, object) &&
            (object.kind != AnnotationKind::brush ||
             !brush_clip_changed(object, plan->source_selection_rect_px, clip,
                                 scale))) {
          AnnotationRenderPlan single{next.revision,
                                      plan->output_size_px,
                                      {*layer},
                                      plan->source_selection_rect_px,
                                      scale};
          auto rebased = AnnotationRenderPlanner::rebase(single, clip);
          if (rebased) {
            result.ordered_layers.push_back(
                std::move(rebased.value().ordered_layers.front()));
            continue;
          }
        }
        if (plan->source_selection_rect_px == clip)
          if (auto translated = translate_coverage_layer_if_compatible(
                  *old, object, *layer, clip, scale)) {
            result.ordered_layers.push_back(std::move(*translated));
            continue;
          }
      }
    }
    auto built = AnnotationRenderPlanner::build(
        {next.revision, {object}, {}, false, false}, clip, scale.value(), text,
        cache);
    if (!built)
      return built;
    result.ordered_layers.push_back(
        std::move(built.value().ordered_layers.front()));
  }
  if (committed)
    result.bind_source(next);
  return Result<AnnotationRenderPlan, Error>::success(std::move(result));
}
Result<AnnotationRenderPlan, Error> update_annotation_render_plan(
    const AnnotationDocumentSnapshot &previous,
    const AnnotationDocumentSnapshot &next, const AnnotationRenderPlan *plan,
    PixelRect clip, AnnotationScale scale, TextRasterizerPort *text,
    AnnotationCoverageCache *cache, const ReusableAnnotationLayer *reuse) {
  return AnnotationPlanUpdater::update(previous, next, plan, clip, scale, text,
                                       cache, reuse, true);
}
Result<AnnotationRenderPlan, Error> prepare_annotation_preview_plan(
    const AnnotationDocumentSnapshot &doc, const AnnotationRenderPlan *plan,
    const AnnotationPreviewRequest &request, PixelRect clip,
    AnnotationScale scale, TextRasterizerPort *text,
    AnnotationCoverageCache *cache) {
  if (auto brush = std::get_if<BrushPreview>(&request)) {
    if (brush->clip != clip || brush->scale != scale ||
        !valid_style(AnnotationKind::brush, brush->style) ||
        brush->trajectory_revision == 0 ||
        brush->layer.object_id != brush->id ||
        brush->layer.kind != AnnotationKind::brush ||
        brush->layer.color_srgb_rgb != brush->style.color_srgb_rgb ||
        !valid_layer(brush->layer, clip) ||
        std::any_of(doc.objects.begin(), doc.objects.end(),
                    [&](auto &o) { return o.id == brush->id; }))
      return Result<AnnotationRenderPlan, Error>::failure(
          plan_error(ErrorCode::invalid_input, "invalid_brush_preview"));
    auto result = AnnotationPlanUpdater::update(doc, doc, plan, clip, scale,
                                                text, cache, nullptr, false);
    if (result)
      result.value().ordered_layers.push_back(brush->layer);
    return result;
  }
  const auto &object = std::get<ObjectPreview>(request);
  auto next = doc;
  auto found = std::find_if(next.objects.begin(), next.objects.end(),
                            [&](auto &o) { return o.id == object.object.id; });
  if ((object.intent == PreviewIntent::create) != (found == next.objects.end()))
    return Result<AnnotationRenderPlan, Error>::failure(
        plan_error(ErrorCode::invalid_input, "preview_intent"));
  std::optional<AnnotationRenderPlan> origin_plan;
  if (object.origin) {
    const auto &origin = *object.origin;
    if (object.intent != PreviewIntent::replace ||
        !same_annotation_source(origin.object, *found) || origin.clip != clip ||
        origin.scale != scale)
      return Result<AnnotationRenderPlan, Error>::failure(
          plan_error(ErrorCode::invalid_input, "preview_origin"));
    auto checked = update_annotation_render_plan(doc, doc, plan, clip, scale,
                                                 text, cache, &origin);
    if (!checked)
      return checked;
    origin_plan = std::move(checked.value());
    plan = &*origin_plan;
  }
  if (object.intent == PreviewIntent::create)
    next.objects.push_back(object.object);
  else
    *found = object.object;
  return AnnotationPlanUpdater::update(doc, next, plan, clip, scale, text,
                                       cache, nullptr, false);
}
} // namespace hdrshot
