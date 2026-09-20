#include "domain/annotation/annotation_render_plan.hpp"
#include "domain/annotation/annotation_geometry.hpp"
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
namespace {

Error plan_error(const ErrorCode code, const char* reason, const ObjectId object_id = {}) {
  auto context = std::map<std::string, std::string>{{"reason", reason}};
  if (object_id.value != 0U) {
    context.emplace("objectId", std::to_string(object_id.value));
  }
  return Error{code, "AnnotationRenderPlanner", Retryability::never, std::move(context)};
}

bool inside_output(const PixelRect bounds, const PixelSize output) {
  return !bounds.empty() && bounds.x >= 0 && bounds.y >= 0 &&
      bounds.right() <= output.width && bounds.bottom() <= output.height;
}

bool inside_output(const PixelPoint point, const PixelSize output) {
  return point.x >= 0 && point.y >= 0 && point.x <= output.width && point.y <= output.height;
}

bool inside_selection(const PixelRect bounds, const PixelRect selection) {
  return !bounds.empty() && !selection.empty() &&
      bounds.x >= selection.x && bounds.y >= selection.y &&
      bounds.right() <= selection.right() && bounds.bottom() <= selection.bottom();
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

ArrowSegments arrow_segments(const AnnotationObject& object) {
  const auto geometry = object.arrow_geometry.value();
  const auto style = std::get<ShapeStyle>(object.style);
  const auto angle = std::atan2(
      static_cast<double>(geometry.start.y - geometry.end.y),
      static_cast<double>(geometry.start.x - geometry.end.x));
  const auto head = std::max(12.0, static_cast<double>(style.line_width_px) * 4.0);
  return ArrowSegments{
      static_cast<double>(geometry.start.x),
      static_cast<double>(geometry.start.y),
      static_cast<double>(geometry.end.x),
      static_cast<double>(geometry.end.y),
      static_cast<double>(geometry.end.x) + std::cos(angle + std::numbers::pi / 6.0) * head,
      static_cast<double>(geometry.end.y) + std::sin(angle + std::numbers::pi / 6.0) * head,
      static_cast<double>(geometry.end.x) + std::cos(angle - std::numbers::pi / 6.0) * head,
      static_cast<double>(geometry.end.y) + std::sin(angle - std::numbers::pi / 6.0) * head,
  };
}

double distance_to_shape(const AnnotationObject& object, const double x, const double y) {
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
  const auto segments = arrow_segments(object);
  return std::min({
      distance_to_segment(
          x, y, segments.start_x, segments.start_y, segments.end_x, segments.end_y),
      distance_to_segment(
          x, y, segments.end_x, segments.end_y, segments.first_x, segments.first_y),
      distance_to_segment(
          x, y, segments.end_x, segments.end_y, segments.second_x, segments.second_y),
  });
}

PixelRect shape_scan_bounds(const AnnotationObject& object, const PixelSize output) {
  const auto style = std::get<ShapeStyle>(object.style);
  const auto margin = static_cast<std::int32_t>(style.line_width_px / 2U) + 2;
  if (is_rotatable(object) && object.transform != ShapeTransform{}) {
    return intersect_output(shape_pixel_bounds(object, margin), output);
  }
  double left = object.bounds.x;
  double top = object.bounds.y;
  double right = object.bounds.right();
  double bottom = object.bounds.bottom();
  if (object.kind == AnnotationKind::arrow) {
    const auto segments = arrow_segments(object);
    left = std::min({segments.start_x, segments.end_x, segments.first_x, segments.second_x});
    top = std::min({segments.start_y, segments.end_y, segments.first_y, segments.second_y});
    right = std::max({segments.start_x, segments.end_x, segments.first_x, segments.second_x});
    bottom = std::max({segments.start_y, segments.end_y, segments.first_y, segments.second_y});
  }
  return intersect_output(PixelRect{
      static_cast<std::int32_t>(std::floor(left)) - margin,
      static_cast<std::int32_t>(std::floor(top)) - margin,
      static_cast<std::int32_t>(std::ceil(right - left)) + margin * 2,
      static_cast<std::int32_t>(std::ceil(bottom - top)) + margin * 2,
  }, output);
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
    const auto first=std::max(scan.y,static_cast<int>(std::floor(std::min(a.y,b.y)-margin)));
    const auto last=std::min(scan.bottom(),static_cast<int>(std::ceil(std::max(a.y,b.y)+margin)));
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
      const auto left=std::max(scan.x,static_cast<int>(std::floor(std::min(start_x,end_x)-margin)));
      const auto right=std::min(scan.right(),static_cast<int>(std::ceil(std::max(start_x,end_x)+margin)));
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
        const auto distance=distance_to_shape(object,
            local_center.x+c*dx+s*dy, local_center.y-s*dx+c*dy);
        samples[static_cast<std::size_t>(x-left)]=static_cast<std::uint8_t>(std::lround(
            std::clamp(half_width+0.5-distance,0.0,1.0)*255.0));
      }
      append_row_spans(layer,y,left,std::move(samples));
    }
  }
}

Result<AnnotationCoverageLayer, Error> rasterize_text(
    const AnnotationObject& object,
    const double pixels_per_point,
    TextRasterizerPort* const text_rasterizer) {
  if (text_rasterizer == nullptr) {
    return Result<AnnotationCoverageLayer, Error>::failure(
        plan_error(ErrorCode::unsupported_encoding, "text_rasterizer_not_connected", object.id));
  }
  const auto style = std::get<TextStyle>(object.style);
  auto mask = text_rasterizer->rasterize(TextRasterRequest{
      object.id,
      object.text,
      PixelSize{object.bounds.width, object.bounds.height},
      style.font_size_pt,
      pixels_per_point,
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

Result<AnnotationCoverageLayer, Error> AnnotationRenderPlanner::rasterize_shape(
    const AnnotationObject& object,
    const PixelSize output_size_px) {
  if (output_size_px.width <= 0 || output_size_px.height <= 0 ||
      object.kind == AnnotationKind::text || !std::holds_alternative<ShapeStyle>(object.style) ||
      !valid_shape_transform(object) || object.bounds.empty() ||
      !inside_output(is_rotatable(object) ? shape_pixel_bounds(object) : object.bounds, output_size_px) ||
      (object.kind == AnnotationKind::arrow &&
       (!object.arrow_geometry.has_value() ||
        !inside_output(object.arrow_geometry->start, output_size_px) ||
        !inside_output(object.arrow_geometry->end, output_size_px)))) {
    return Result<AnnotationCoverageLayer, Error>::failure(
        plan_error(ErrorCode::invalid_input, "shape_out_of_selection", object.id));
  }
  const auto style = std::get<ShapeStyle>(object.style);
  const auto scan_bounds = shape_scan_bounds(object, output_size_px);
  AnnotationCoverageLayer layer{
      object.id,
      object.kind,
      scan_bounds,
      style.color_srgb_rgb,
      {},
      "hdrshot-shape-coverage-v1",
  };
  const auto half_width = static_cast<double>(style.line_width_px) / 2.0;
  if (is_rotatable(object) && object.transform != ShapeTransform{}) {
    rasterize_transformed_shape(layer, object, scan_bounds, half_width);
    return Result<AnnotationCoverageLayer, Error>::success(std::move(layer));
  }
  for (std::int32_t y = scan_bounds.y; y < scan_bounds.bottom(); ++y) {
    if (object.kind == AnnotationKind::ellipse) {
      append_ellipse_row(layer, object, scan_bounds, y, half_width);
      continue;
    }
    std::vector<std::uint8_t> row(static_cast<std::size_t>(scan_bounds.width), 0U);
    for (std::int32_t x = scan_bounds.x; x < scan_bounds.right(); ++x) {
      const auto distance = distance_to_shape(
          object, static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5);
      const auto coverage = std::clamp(half_width + 0.5 - distance, 0.0, 1.0);
      row[static_cast<std::size_t>(x - scan_bounds.x)] =
          static_cast<std::uint8_t>(std::lround(coverage * 255.0));
    }
    append_row_spans(layer, y, scan_bounds.x, std::move(row));
  }
  return Result<AnnotationCoverageLayer, Error>::success(std::move(layer));
}

Result<AnnotationRenderPlan, Error> AnnotationRenderPlanner::build(
    const AnnotationDocumentSnapshot& annotations,
    const PixelRect selection_rect_px,
    const double pixels_per_point,
    TextRasterizerPort* const text_rasterizer) {
  const auto output_size_px = PixelSize{
      selection_rect_px.width, selection_rect_px.height};
  if (output_size_px.width <= 0 || output_size_px.height <= 0 ||
      !std::isfinite(pixels_per_point) || pixels_per_point <= 0.0) {
    return Result<AnnotationRenderPlan, Error>::failure(
        plan_error(ErrorCode::invalid_input, "invalid_plan_contract"));
  }
  AnnotationRenderPlan plan{
      annotations.revision, output_size_px, {}, selection_rect_px};
  plan.ordered_layers.reserve(annotations.objects.size());
  for (const auto& object : annotations.objects) {
    if (!valid_shape_transform(object) || !inside_selection(
        is_rotatable(object) ? shape_pixel_bounds(object) : object.bounds, selection_rect_px)) {
      return Result<AnnotationRenderPlan, Error>::failure(
          plan_error(ErrorCode::invalid_input, "annotation_out_of_selection", object.id));
    }
    const auto local_object = selection_local_object(object, selection_rect_px);
    auto layer = local_object.kind == AnnotationKind::text
        ? rasterize_text(local_object, pixels_per_point, text_rasterizer)
        : rasterize_shape(local_object, output_size_px);
    if (!layer) {
      return Result<AnnotationRenderPlan, Error>::failure(layer.error());
    }
    plan.ordered_layers.push_back(std::move(layer.value()));
  }
  return Result<AnnotationRenderPlan, Error>::success(std::move(plan));
}

Result<AnnotationRenderPlan, Error> AnnotationRenderPlanner::rebase(
    const AnnotationRenderPlan& render_plan,
    const PixelRect selection_rect_px) {
  if (render_plan.source_selection_rect_px.empty() || selection_rect_px.empty()) {
    return Result<AnnotationRenderPlan, Error>::failure(
        plan_error(ErrorCode::invalid_input, "invalid_rebase_selection"));
  }
  const auto output_size_px = PixelSize{
      selection_rect_px.width, selection_rect_px.height};
  const auto delta_x = render_plan.source_selection_rect_px.x - selection_rect_px.x;
  const auto delta_y = render_plan.source_selection_rect_px.y - selection_rect_px.y;
  auto rebased = render_plan;
  rebased.output_size_px = output_size_px;
  rebased.source_selection_rect_px = selection_rect_px;
  for (auto& layer : rebased.ordered_layers) {
    layer.bounds_px.x += delta_x;
    layer.bounds_px.y += delta_y;
    layer.bounds_px = intersect_output(layer.bounds_px, output_size_px);
    for (auto& span : layer.spans) {
      span.x += delta_x;
      span.y += delta_y;
      if (span.x < 0 || span.y < 0 || span.y >= output_size_px.height ||
          span.x + static_cast<std::int32_t>(span.coverage_u8.size()) >
              output_size_px.width) {
        return Result<AnnotationRenderPlan, Error>::failure(
            plan_error(
                ErrorCode::invalid_input,
                "rebase_excludes_annotation",
                layer.object_id));
      }
    }
  }
  return Result<AnnotationRenderPlan, Error>::success(std::move(rebased));
}

Result<AnnotationPixelPlan, Error> AnnotationRenderPlanner::build_pixel_plan(
    const AnnotationRenderPlan& render_plan) {
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
    std::uint32_t layer_index{};
    const CoverageSpan* span{};
  };
  std::vector<std::vector<RowCoverage>> spans_by_row(height);
  for (std::size_t layer_index = 0; layer_index < render_plan.ordered_layers.size();
       ++layer_index) {
    const auto& layer = render_plan.ordered_layers[layer_index];
    for (const auto& span : layer.spans) {
      if (span.y < 0 || span.y >= size.height || span.x < 0 ||
          static_cast<std::int64_t>(span.x) +
                  static_cast<std::int64_t>(span.coverage_u8.size()) >
              static_cast<std::int64_t>(size.width)) {
        return Result<AnnotationPixelPlan, Error>::failure(
            plan_error(ErrorCode::state_inconsistent,
                       "coverage_span_out_of_bounds", layer.object_id));
      }
      spans_by_row[static_cast<std::size_t>(span.y)].push_back(RowCoverage{
          static_cast<std::uint32_t>(layer_index), &span});
    }
  }

  AnnotationPixelPlan pixel_plan{
      render_plan.source_document_revision,
      size,
      {},
      {},
  };
  constexpr auto kSourceOwner = std::numeric_limits<std::uint32_t>::max();
  std::vector<std::uint32_t> owners(width, kSourceOwner);
  std::vector<std::array<float, 4>> samples(width);
  std::vector<bool> edge(width);
  std::vector<std::array<float, 3>> colors;
  colors.reserve(render_plan.ordered_layers.size());
  for (const auto& layer : render_plan.ordered_layers)
    colors.push_back(ExtendedP3Mapper::annotation_linear_display_p3(layer.color_srgb_rgb));
  for (std::int32_t y = 0; y < size.height; ++y) {
    std::fill(owners.begin(), owners.end(), kSourceOwner);
    std::fill(samples.begin(), samples.end(), std::array<float, 4>{0, 0, 0, 1});
    std::fill(edge.begin(), edge.end(), false);
    for (const auto& coverage : spans_by_row[static_cast<std::size_t>(y)]) {
      for (std::size_t offset = 0; offset < coverage.span->coverage_u8.size(); ++offset) {
        const auto amount = coverage.span->coverage_u8[offset];
        if (amount == 0U) continue;
        const auto index = static_cast<std::size_t>(coverage.span->x) + offset;
        const auto& color = colors[coverage.layer_index];
        const float alpha = static_cast<float>(amount) / 255.0F;
        auto& pixel = samples[index];
        for (std::size_t channel = 0; channel < 3; ++channel)
          pixel[channel] = alpha * color[channel] + (1.0F - alpha) * pixel[channel];
        pixel[3] *= 1.0F - alpha;
        edge[index] = amount != 255U;
        owners[index] = coverage.layer_index;
      }
    }
    std::int32_t x = 0;
    while (x < size.width) {
      const auto owner = owners[static_cast<std::size_t>(x)];
      auto end = x + 1;
      while (end < size.width && owners[static_cast<std::size_t>(end)] == owner &&
             edge[static_cast<std::size_t>(end)] == edge[static_cast<std::size_t>(x)]) {
        ++end;
      }
      if (owner == kSourceOwner) {
        pixel_plan.source_visible_spans.push_back(
            SourceVisibleSpan{y, x, end - x});
      } else {
        const auto& layer = render_plan.ordered_layers[owner];
        pixel_plan.annotation_owned_spans.push_back(AnnotationOwnedSpan{
            y, x, end - x, layer.object_id, layer.color_srgb_rgb});
        if (edge[static_cast<std::size_t>(x)]) {
          pixel_plan.annotation_owned_spans.back().edge_samples.assign(
              samples.begin() + x, samples.begin() + end);
        }
      }
      x = end;
    }
  }
  return Result<AnnotationPixelPlan, Error>::success(std::move(pixel_plan));
}

}  // namespace hdrshot
