#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>

#include "annotation_raster_reference.hpp"
#include "domain/annotation/annotation_render_plan.hpp"
using namespace hdrshot;
static void require(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
static AnnotationObject arrow(int x, int y, int ex, int ey, int width = 4) {
  AnnotationObject a;
  a.id = {1};
  a.kind = AnnotationKind::arrow;
  a.bounds = {std::min(x, ex), std::min(y, ey), std::max(1, std::abs(x - ex)),
              std::max(1, std::abs(y - ey))};
  a.style = ShapeStyle{0xff2800, std::uint16_t(width)};
  a.arrow_geometry = ArrowGeometry{{x, y}, {ex, ey}};
  return a;
}
int main() {
  AnnotationCoverageCache cache;
  std::mt19937 random(512);
  int shapes = 0, pixels = 0;
  for (int i = 0; i < 120; ++i) {
    auto a =
        arrow(int(random() % 400), int(random() % 300), int(random() % 400),
              int(random() % 300), 1 + int(random() % 30));
    if (i % 3 != 0) {
      a.kind = i % 3 == 1 ? AnnotationKind::rectangle : AnnotationKind::ellipse;
      a.arrow_geometry.reset();
    }
    for (int move = 0; move < 3; ++move) {
      if (move) {
        a.bounds.x += 7;
        a.bounds.y += 9;
        if (a.arrow_geometry) {
          a.arrow_geometry->start.x += 7;
          a.arrow_geometry->end.x += 7;
          a.arrow_geometry->start.y += 9;
          a.arrow_geometry->end.y += 9;
        }
      }
      auto ref = test_reference::rasterize_shape(a, {500, 400});
      auto fast = cache.get(a, {500, 400}, AnnotationScale::from(1).value());
      require(bool(ref) && bool(fast), "shape valid");
      if (ref.value() != fast.value()) {
        std::cerr << "shape mismatch case=" << i << " move=" << move
                  << " width=" << std::get<ShapeStyle>(a.style).line_width_dip
                  << " reference bounds=" << ref.value().bounds_px.x << ','
                  << ref.value().bounds_px.y << ','
                  << ref.value().bounds_px.width << ','
                  << ref.value().bounds_px.height
                  << " actual bounds=" << fast.value().bounds_px.x << ','
                  << fast.value().bounds_px.y << ','
                  << fast.value().bounds_px.width << ','
                  << fast.value().bounds_px.height << " spans_equal="
                  << (ref.value().spans == fast.value().spans) << '\n';
      }
      require(ref.value() == fast.value(), "shape pixels differ");
      ++shapes;
      AnnotationRenderPlan plan{1,
                                {500, 400},
                                {fast.value()},
                                {0, 0, 500, 400},
                                AnnotationScale::from(1).value()};
      auto b = a;
      b.id = {2};
      b.style = ShapeStyle{0x30ff80, 7};
      auto second = test_reference::rasterize_shape(b, {500, 400});
      require(bool(second), "second valid");
      plan.ordered_layers.push_back(second.value());
      // A sparse row containing explicit zero coverage exercises source gaps.
      plan.ordered_layers.push_back(
          {{3},
           AnnotationKind::text,
           {0, 0, 10, 1},
           0xffffff,
           {{0, 0, {0, 1, 128, 255, 0, 127, 0, 255, 0, 0}}},
           "fixture"});
      auto pr = test_reference::build_pixel_plan(plan),
           pf = AnnotationRenderPlanner::build_pixel_plan(plan);
      require(bool(pr) && bool(pf) && pr.value() == pf.value(),
              "pixel ownership/composition differs");
      ++pixels;
    }
  }
  for (double factor : {1.25, 1.5, 2.})
    for (auto kind : {AnnotationKind::rectangle, AnnotationKind::ellipse,
                      AnnotationKind::arrow}) {
      auto object = arrow(70, 60, 210, 180, 4);
      object.kind = kind;
      if (kind != AnnotationKind::arrow)
        object.arrow_geometry.reset();
      auto reference =
          test_reference::rasterize_shape(object, {300, 240}, factor);
      auto actual = AnnotationRenderPlanner::rasterize_shape(
          object, {300, 240}, AnnotationScale::from(factor).value());
      require(reference && actual && reference.value() == actual.value(),
              "DIP scaled independent oracle differs");
    }
  std::cout << shapes << " coverage/translation cases and " << pixels
            << " overlap pixel plans equal reference\n";
  // Empty plans and cache clipping changes must preserve complete source
  // ownership.
  for (auto size : {PixelSize{1, 1}, PixelSize{100, 70}}) {
    AnnotationRenderPlan plan{1,
                              size,
                              {},
                              {0, 0, size.width, size.height},
                              AnnotationScale::from(1).value()};
    auto a = test_reference::build_pixel_plan(plan),
         b = AnnotationRenderPlanner::build_pixel_plan(plan);
    require(a && b && a.value() == b.value(), "empty plan differs");
  }
  for (int end : {20, 80, 150}) {
    auto a = arrow(0, 0, end, end, 20);
    for (auto size :
         {PixelSize{200, 200}, PixelSize{160, 160}, PixelSize{200, 200}}) {
      auto ref = test_reference::rasterize_shape(a, size),
           got = cache.get(a, size, AnnotationScale::from(1).value());
      require(ref && got && ref.value() == got.value(),
              "cache clipped differently");
    }
  }
}
