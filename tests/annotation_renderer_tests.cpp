#include "domain/annotation/annotation_renderer.hpp"
#include "domain/annotation/annotation_geometry.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "domain/color/pq_reference_white_mapper.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace hdrshot;

class SolidTextRasterizer final : public TextRasterizerPort {
 public:
  Result<TextCoverageMask, Error> rasterize(const TextRasterRequest& request) override {
    const auto sample_count = static_cast<std::size_t>(request.mask_size_px.width) *
        static_cast<std::size_t>(request.mask_size_px.height);
    return Result<TextCoverageMask, Error>::success(TextCoverageMask{
        request.object_id,
        request.mask_size_px,
        std::vector<std::uint8_t>(sample_count, 255),
        "test-font",
    });
  }
};

CanonicalFrameView frame() {
  std::vector<std::uint16_t> pixels;
  for (int index = 0; index < 16; ++index) {
    pixels.insert(pixels.end(), {0x3810, 0x3810, 0x3810, 0x3C00});
  }
  return CanonicalFrameView{
      FrameId{1},
      2,
      3,
      PixelRect{10, 20, 4, 4},
      PixelSize{4, 4},
      1.0,
      ColorEncoding{
          ColorPrimaries::display_p3,
          TransferFunction::extended_srgb,
          AlphaMode::straight,
          0.0},
      DisplayDynamicRange::hdr,
      std::move(pixels),
  };
}

void empty_document_matches_reference_mapper_for_every_pixel() {
  const auto rendered = AnnotationRenderer::render(frame(), AnnotationDocument::empty().snapshot());
  HDRSHOT_CHECK(rendered.has_value());
  const auto decoded = ExtendedP3Mapper::decode_binary16(0x3810);
  HDRSHOT_CHECK(decoded.has_value());
  const auto expected = PqReferenceWhiteMapper::map_linear_edr(
      ExtendedP3Mapper::inverse_extended_srgb(decoded.value()));
  HDRSHOT_CHECK(expected.has_value());
  for (std::size_t pixel = 0; pixel < 16; ++pixel) {
    for (std::size_t channel = 0; channel < 3; ++channel) {
      HDRSHOT_CHECK(rendered.value().rgba_png_u16[pixel * 4 + channel] == expected.value().png_u16);
    }
    HDRSHOT_CHECK(rendered.value().rgba_png_u16[pixel * 4 + 3] == 65535);
  }
  HDRSHOT_CHECK(rendered.value().source_selection_revision == 3);
  HDRSHOT_CHECK(rendered.value().source_document_revision == 0);
}

void rectangle_changes_only_covered_pixels() {
  auto document = AnnotationDocument::empty();
  auto created = AnnotationDocument::apply(
      document,
      CreateAnnotation{AnnotationObject{
          ObjectId{1},
          AnnotationKind::rectangle,
          PixelRect{11, 21, 1, 1},
          ShapeStyle{0xFF4D67, 2},
          {},
          std::nullopt,
      }});
  HDRSHOT_CHECK(created.has_value());
  document = std::move(created.value());

  const auto baseline = AnnotationRenderer::render(frame(), AnnotationDocument::empty().snapshot());
  const auto rendered = AnnotationRenderer::render(frame(), document.snapshot());
  HDRSHOT_CHECK(baseline.has_value());
  HDRSHOT_CHECK(rendered.has_value());
  HDRSHOT_CHECK(rendered.value().source_document_revision == 1);
  HDRSHOT_CHECK(rendered.value().rgba_png_u16[3 * 4] == baseline.value().rgba_png_u16[3 * 4]);
  HDRSHOT_CHECK(rendered.value().rgba_png_u16[5 * 4] != baseline.value().rgba_png_u16[5 * 4]);
}

void text_without_font_rasterizer_fails_explicitly() {
  auto document = AnnotationDocument::empty();
  auto created = AnnotationDocument::apply(
      document,
      CreateAnnotation{AnnotationObject{
          ObjectId{2},
          AnnotationKind::text,
          PixelRect{10, 20, 4, 4},
          TextStyle{0xFFFFFF, 20},
          "text",
          std::nullopt,
      }});
  HDRSHOT_CHECK(created.has_value());
  const auto rendered = AnnotationRenderer::render(frame(), created.value().snapshot());
  HDRSHOT_CHECK(!rendered.has_value());
  HDRSHOT_CHECK(rendered.error().code == ErrorCode::unsupported_encoding);
  HDRSHOT_CHECK(rendered.error().safe_context.at("reason") == "text_rasterizer_not_connected");
}

void text_uses_injected_coverage_mask() {
  auto document = AnnotationDocument::empty();
  auto created = AnnotationDocument::apply(
      document,
      CreateAnnotation{AnnotationObject{
          ObjectId{3},
          AnnotationKind::text,
          PixelRect{11, 21, 1, 1},
          TextStyle{0xFFFFFF, 20},
          "字",
          std::nullopt,
      }});
  HDRSHOT_CHECK(created.has_value());
  SolidTextRasterizer rasterizer;
  const auto baseline = AnnotationRenderer::render(frame(), AnnotationDocument::empty().snapshot());
  const auto rendered = AnnotationRenderer::render(
      frame(), created.value().snapshot(), 203.0, &rasterizer);
  HDRSHOT_CHECK(baseline.has_value());
  HDRSHOT_CHECK(rendered.has_value());
  HDRSHOT_CHECK(rendered.value().rgba_png_u16[5 * 4] != baseline.value().rgba_png_u16[5 * 4]);
  HDRSHOT_CHECK(rendered.value().rgba_png_u16[0] == baseline.value().rgba_png_u16[0]);
}

std::uint8_t coverage_at(
    const AnnotationCoverageLayer& layer,
    const std::int32_t x,
    const std::int32_t y) {
  for (const auto& span : layer.spans) {
    if (span.y == y && x >= span.x &&
        x < span.x + static_cast<std::int32_t>(span.coverage_u8.size())) {
      return span.coverage_u8[static_cast<std::size_t>(x - span.x)];
    }
  }
  return 0U;
}

double reference_distance_to_ellipse_boundary(
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
    parameter = std::clamp(
        parameter - derivative / second_derivative,
        0.0,
        std::numbers::pi / 2.0);
  }
  const auto distance_at = [&](const double candidate) {
    return std::hypot(
        radius_x * std::cos(candidate) - point_x,
        radius_y * std::sin(candidate) - point_y);
  };
  return std::min({distance_at(parameter), distance_at(0.0),
                   distance_at(std::numbers::pi / 2.0)});
}

void sparse_ellipse_matches_exhaustive_reference() {
  struct EllipseCase {
    PixelSize output;
    PixelRect bounds;
    std::uint16_t line_width;
  };
  const std::vector<EllipseCase> cases{
      {{100, 50}, {10, 10, 80, 20}, 2},
      {{100, 50}, {10, 10, 80, 20}, 8},
      {{100, 50}, {10, 10, 80, 20}, 12},
      {{64, 64}, {3, 2, 17, 31}, 4},
      {{64, 64}, {0, 0, 20, 20}, 6},
      {{128, 96}, {20, 15, 63, 47}, 8},
  };
  for (const auto& test_case : cases) {
    const AnnotationObject ellipse{
        ObjectId{40},
        AnnotationKind::ellipse,
        test_case.bounds,
        ShapeStyle{0x36A3FF, test_case.line_width},
        {},
        std::nullopt,
    };
    const auto layer = AnnotationRenderPlanner::rasterize_shape(
        ellipse, test_case.output);
    HDRSHOT_CHECK(layer.has_value());
    const auto half_width = static_cast<double>(test_case.line_width) / 2.0;
    for (std::int32_t y = 0; y < test_case.output.height; ++y) {
      for (std::int32_t x = 0; x < test_case.output.width; ++x) {
        const auto distance = reference_distance_to_ellipse_boundary(
            static_cast<double>(x) + 0.5,
            static_cast<double>(y) + 0.5,
            test_case.bounds);
        const auto expected = static_cast<std::uint8_t>(std::lround(
            std::clamp(half_width + 0.5 - distance, 0.0, 1.0) * 255.0));
        const auto actual = coverage_at(layer.value(), x, y);
        if (actual != expected) {
          throw std::runtime_error(
              "ellipse coverage mismatch at " + std::to_string(x) + "," +
              std::to_string(y) + ": actual=" + std::to_string(actual) +
              " expected=" + std::to_string(expected) + " bounds=" +
              std::to_string(test_case.bounds.x) + "," +
              std::to_string(test_case.bounds.y) + "," +
              std::to_string(test_case.bounds.width) + "," +
              std::to_string(test_case.bounds.height) + " width=" +
              std::to_string(test_case.line_width));
        }
      }
    }
  }
}

void render_plan_rebase_preserves_absolute_coverage_and_rejects_exclusion() {
  auto document = AnnotationDocument::empty();
  auto created = AnnotationDocument::apply(
      document,
      CreateAnnotation{AnnotationObject{
          ObjectId{41},
          AnnotationKind::ellipse,
          PixelRect{130, 140, 80, 40},
          ShapeStyle{0x36A3FF, 8},
          {},
          std::nullopt,
      }});
  HDRSHOT_CHECK(created.has_value());
  const auto original = AnnotationRenderPlanner::build(
      created.value().snapshot(), PixelRect{100, 100, 160, 120}, 1.0);
  HDRSHOT_CHECK(original.has_value());
  const auto original_copy = original.value();
  const auto rebased = AnnotationRenderPlanner::rebase(
      original.value(), PixelRect{80, 90, 220, 160});
  HDRSHOT_CHECK(rebased.has_value());
  HDRSHOT_CHECK(rebased.value().ordered_layers.size() == 1U);
  const auto& old_layer = original.value().ordered_layers.front();
  const auto& new_layer = rebased.value().ordered_layers.front();
  HDRSHOT_CHECK(old_layer.spans.size() == new_layer.spans.size());
  for (std::size_t index = 0; index < old_layer.spans.size(); ++index) {
    HDRSHOT_CHECK(old_layer.spans[index].coverage_u8 ==
                  new_layer.spans[index].coverage_u8);
    HDRSHOT_CHECK(100 + old_layer.spans[index].x ==
                  80 + new_layer.spans[index].x);
    HDRSHOT_CHECK(100 + old_layer.spans[index].y ==
                  90 + new_layer.spans[index].y);
  }

  const auto rejected = AnnotationRenderPlanner::rebase(
      original.value(), PixelRect{160, 100, 100, 120});
  HDRSHOT_CHECK(!rejected.has_value());
  HDRSHOT_CHECK(original.value() == original_copy);
}

void non_circular_ellipse_keeps_constant_geometric_stroke_width() {
  const AnnotationObject ellipse{
      ObjectId{4},
      AnnotationKind::ellipse,
      PixelRect{10, 10, 80, 20},
      ShapeStyle{0x36A3FF, 8},
      {},
      std::nullopt,
  };
  const auto layer = AnnotationRenderPlanner::rasterize_shape(
      ellipse, PixelSize{100, 50});
  HDRSHOT_CHECK(layer.has_value());
  // The old normalized-radius formula incorrectly covered x=0 at the wide
  // ellipse's horizontal end (roughly 4x the requested stroke width).
  HDRSHOT_CHECK(coverage_at(layer.value(), 0, 20) == 0U);
  HDRSHOT_CHECK(coverage_at(layer.value(), 6, 20) > 0U);
  HDRSHOT_CHECK(coverage_at(layer.value(), 50, 6) > 0U);
  HDRSHOT_CHECK(coverage_at(layer.value(), 50, 0) == 0U);
}

void renderer_consumes_the_exact_prebuilt_plan_revision() {
  auto document = AnnotationDocument::empty();
  auto created = AnnotationDocument::apply(
      document,
      CreateAnnotation{AnnotationObject{
          ObjectId{5},
          AnnotationKind::rectangle,
          PixelRect{11, 21, 2, 2},
          ShapeStyle{0x25C06D, 2},
          {},
          std::nullopt,
      }});
  HDRSHOT_CHECK(created.has_value());
  const auto plan = AnnotationRenderPlanner::build(
      created.value().snapshot(), PixelRect{10, 20, 4, 4}, 1.0);
  HDRSHOT_CHECK(plan.has_value());
  const auto rendered = AnnotationRenderer::render(frame(), plan.value());
  HDRSHOT_CHECK(rendered.has_value());
  HDRSHOT_CHECK(rendered.value().source_document_revision ==
                plan.value().source_document_revision);
}

void annotation_owned_pixel_does_not_read_or_blend_hidden_source() {
  const AnnotationRenderPlan plan{
      4,
      PixelSize{4, 4},
      {AnnotationCoverageLayer{
          ObjectId{9},
          AnnotationKind::rectangle,
          PixelRect{0, 0, 1, 1},
          0x25C06D,
          {CoverageSpan{0, 0, {255U}}},
          "test",
      }},
  };
  auto first = frame();
  auto second = frame();
  first.rgba_half[0] = 0x3810;
  second.rgba_half[0] = 0x3900;
  const auto first_rendered = AnnotationRenderer::render(first, plan);
  const auto second_rendered = AnnotationRenderer::render(second, plan);
  HDRSHOT_CHECK(first_rendered.has_value());
  HDRSHOT_CHECK(second_rendered.has_value());
  for (std::size_t channel = 0; channel < 4U; ++channel) {
    HDRSHOT_CHECK(first_rendered.value().rgba_png_u16[channel] ==
                  second_rendered.value().rgba_png_u16[channel]);
  }
  HDRSHOT_CHECK(first_rendered.value().encoding.alpha == AlphaMode::opaque);
}

void pixel_plan_resolves_topmost_opaque_owner_and_exhaustive_source_spans() {
  const AnnotationRenderPlan plan{
      7,
      PixelSize{3, 1},
      {
          AnnotationCoverageLayer{
              ObjectId{1}, AnnotationKind::rectangle, PixelRect{0, 0, 2, 1},
              0xFF0000, {CoverageSpan{0, 0, {255U, 255U}}}, "first"},
          AnnotationCoverageLayer{
              ObjectId{2}, AnnotationKind::rectangle, PixelRect{1, 0, 1, 1},
              0x0000FF, {CoverageSpan{0, 1, {255U}}}, "second"},
      },
  };
  const auto pixel_plan = AnnotationRenderPlanner::build_pixel_plan(plan);
  HDRSHOT_CHECK(pixel_plan.has_value());
  HDRSHOT_CHECK(pixel_plan.value().source_visible_spans ==
                (std::vector<SourceVisibleSpan>{{0, 2, 1}}));
  HDRSHOT_CHECK(pixel_plan.value().annotation_owned_spans.size() == 2U);
  HDRSHOT_CHECK(pixel_plan.value().annotation_owned_spans[0].object_id == ObjectId{1});
  HDRSHOT_CHECK(pixel_plan.value().annotation_owned_spans[1].object_id == ObjectId{2});
}

void rotated_shapes_match_exhaustive_geometry_and_rebase() {
  for(auto kind : {AnnotationKind::rectangle,AnnotationKind::ellipse}) {
    for(auto angle : {0.0,0.37,-0.91,std::numbers::pi/2}) {
      AnnotationObject object{ObjectId{80},kind,{30,42,64,28},ShapeStyle{0xFF4D67,8},{},std::nullopt};
      object.transform={angle,0.25,0.75};
      const auto layer=AnnotationRenderPlanner::rasterize_shape(object,{128,128});
      HDRSHOT_CHECK(layer.has_value());
      int partial=0;
      const auto center=shape_center(object);
      for(int y=0;y<128;++y) for(int x=0;x<128;++x) {
        // Independent inverse transform and exhaustive per-pixel oracle.
        const auto dx=x+0.5-center.x,dy=y+0.5-center.y;
        const auto lx=std::cos(angle)*dx+std::sin(angle)*dy;
        const auto ly=-std::sin(angle)*dx+std::cos(angle)*dy;
        double distance;
        if(kind==AnnotationKind::rectangle) {
          const auto qx=std::abs(lx)-32, qy=std::abs(ly)-14;
          distance=std::abs(std::hypot(std::max(qx,0.0),std::max(qy,0.0))+std::min(std::max(qx,qy),0.0));
        } else distance=reference_distance_to_ellipse_boundary(lx+62,ly+56,object.bounds);
        const auto expected=static_cast<int>(std::lround(std::clamp(4.5-distance,0.0,1.0)*255));
        const auto actual=coverage_at(layer.value(),x,y);
        if(std::abs(static_cast<int>(actual)-expected)>1) throw std::runtime_error(
            "rotated coverage mismatch kind="+std::to_string(static_cast<int>(kind))+" at="+
            std::to_string(x)+","+std::to_string(y)+" angle="+std::to_string(angle));
        if(actual>0 && actual<255) ++partial;
      }
      HDRSHOT_CHECK(partial>0);
      object.bounds.x+=100; object.bounds.y+=100;
      AnnotationDocumentSnapshot snapshot{3,{object},object.id};
      const auto original=AnnotationRenderPlanner::build(snapshot,{100,100,128,128},1);
      HDRSHOT_CHECK(original.has_value());
      const auto shifted=AnnotationRenderPlanner::rebase(original.value(),{90,90,150,150});
      const auto rebuilt=AnnotationRenderPlanner::build(snapshot,{90,90,150,150},1);
      HDRSHOT_CHECK(shifted && rebuilt);
      HDRSHOT_CHECK(AnnotationRenderPlanner::build_pixel_plan(shifted.value()).value()==
                    AnnotationRenderPlanner::build_pixel_plan(rebuilt.value()).value());
    }
  }
}

void rotated_large_ellipse_is_sparse_and_bounded() {
  AnnotationObject object{ObjectId{81},AnnotationKind::ellipse,{2000,3600,4000,800},
      ShapeStyle{0xFFFFFF,12},{},std::nullopt};
  object.transform.rotation_radians=0.72;
  const auto start=std::chrono::steady_clock::now();
  const auto result=AnnotationRenderPlanner::rasterize_shape(object,{8192,8192});
  HDRSHOT_CHECK(result.has_value());
  std::size_t samples=0;
  for(const auto& span:result.value().spans) samples+=span.coverage_u8.size();
  HDRSHOT_CHECK(samples>10000 && samples<200000);
  const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  std::cout << "rotated 8K ellipse raster_ms=" << ms << " covered_samples=" << samples << '\n';
  // Safety bound, not an interactive performance claim for every machine.
  HDRSHOT_CHECK(ms<10000);
}

void ellipse_convergence_matches_fixed_iteration_reference() {
  for (const auto size : {PixelSize{80,80},PixelSize{120,4},PixelSize{4,120},
                          PixelSize{119,17},PixelSize{9,7}}) {
    for (const auto width : {1U,8U,32U}) {
      for (const auto angle : {0.0,0.17,0.91,-1.72}) {
        AnnotationObject object{ObjectId{82},AnnotationKind::ellipse,
            {100-size.width/2,100-size.height/2,size.width,size.height},
            ShapeStyle{0xFFFFFF,static_cast<std::uint16_t>(width)},{},std::nullopt};
        object.transform={angle,0.25,0.75};
        const auto result=AnnotationRenderPlanner::rasterize_shape(object,{202,202});
        HDRSHOT_CHECK(result.has_value());
        const auto center=shape_center(object);
        const auto c=std::cos(angle),s=std::sin(angle);
        for (int y=0;y<202;++y) for (int x=0;x<202;++x) {
          const auto dx=x+0.5-center.x,dy=y+0.5-center.y;
          const auto distance=reference_distance_to_ellipse_boundary(
              object.bounds.x+size.width*0.5+c*dx+s*dy,
              object.bounds.y+size.height*0.5-s*dx+c*dy,object.bounds);
          const auto expected=static_cast<int>(std::lround(
              std::clamp(width*0.5+0.5-distance,0.0,1.0)*255));
          const auto actual=static_cast<int>(coverage_at(result.value(),x,y));
          if (std::abs(actual-expected)>1) throw std::runtime_error(
              "convergence coverage mismatch size="+std::to_string(size.width)+"x"+
              std::to_string(size.height)+" width="+std::to_string(width)+
              " angle="+std::to_string(angle)+" at="+std::to_string(x)+","+std::to_string(y));
        }
      }
    }
  }
}

void ellipse_raster_timing_samples() {
  for (const auto size : {PixelSize{600,300},PixelSize{1600,800},PixelSize{4000,800}}) {
    AnnotationObject object{ObjectId{83},AnnotationKind::ellipse,
        {2000,3600,size.width,size.height},ShapeStyle{0xFFFFFF,12},{},std::nullopt};
    object.transform.rotation_radians=0.72;
    std::vector<double> timings;
    for (int run=0;run<8;++run) {
      const auto start=std::chrono::steady_clock::now();
      const auto result=AnnotationRenderPlanner::rasterize_shape(object,{8192,8192});
      HDRSHOT_CHECK(result.has_value());
      const auto ms=std::chrono::duration<double,std::milli>(
          std::chrono::steady_clock::now()-start).count();
      if (run>0) timings.push_back(ms);
    }
    std::sort(timings.begin(),timings.end());
    std::cout << "ellipse " << size.width << 'x' << size.height
        << " raster_ms median=" << timings[3] << " min=" << timings.front()
        << " max=" << timings.back() << '\n';
  }
}

}  // namespace

int main() {
  using hdrshot::test::TestCase;
  return hdrshot::test::run(std::vector<TestCase>{
      {"rotated coverage matches exhaustive reference and rebase",rotated_shapes_match_exhaustive_geometry_and_rebase},
      {"rotated large ellipse uses sparse coverage",rotated_large_ellipse_is_sparse_and_bounded},
      {"ellipse convergence matches fixed-iteration oracle",ellipse_convergence_matches_fixed_iteration_reference},
      {"ellipse timing samples",ellipse_raster_timing_samples},
      {"D6 empty document matches mapper", empty_document_matches_reference_mapper_for_every_pixel},
      {"D6 rectangle modifies covered pixels", rectangle_changes_only_covered_pixels},
      {"D6 text rasterizer absence is explicit", text_without_font_rasterizer_fails_explicitly},
      {"D6 text consumes injected mask", text_uses_injected_coverage_mask},
      {"D6 ellipse uses constant geometric width", non_circular_ellipse_keeps_constant_geometric_stroke_width},
      {"D6 sparse ellipse matches exhaustive reference", sparse_ellipse_matches_exhaustive_reference},
      {"D6 render plan rebase preserves coverage", render_plan_rebase_preserves_absolute_coverage_and_rejects_exclusion},
      {"D6 consumes prebuilt render plan", renderer_consumes_the_exact_prebuilt_plan_revision},
      {"D6 annotation ownership skips hidden source", annotation_owned_pixel_does_not_read_or_blend_hidden_source},
      {"D6 pixel plan resolves opaque topmost ownership", pixel_plan_resolves_topmost_opaque_owner_and_exhaustive_source_spans},
  });
}
