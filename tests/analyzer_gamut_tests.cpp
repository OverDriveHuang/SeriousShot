#include "domain/analysis/gamut_boundary.hpp"
#include "domain/analysis/math.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>

namespace {
using namespace hdrshot;
using namespace hdrshot::analysis;
using Point = std::array<float, 2>;
double outside_distance(std::span<const Point> polygon, Point p) {
  bool inside = false;
  double distance = std::numeric_limits<double>::max();
  for (std::size_t i = 1; i < polygon.size(); ++i) {
    const auto a = polygon[i - 1], b = polygon[i];
    const double dx = double(b[0]) - a[0], dy = double(b[1]) - a[1];
    const double t = std::clamp(((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) /
                                  std::max(1e-30, dx * dx + dy * dy), 0., 1.);
    distance = std::min(distance, std::hypot(p[0] - a[0] - t * dx, p[1] - a[1] - t * dy));
    if ((a[1] > p[1]) != (b[1] > p[1]) &&
        p[0] < a[0] + (p[1] - a[1]) * dx / dy) inside = !inside;
  }
  return inside ? 0. : distance;
}
void immutable_tables() {
  std::size_t bytes = 0;
  for (unsigned s = 0; s < 4; ++s) {
    const auto space = WorkingSpace(s);
    auto polygon = perceptual_gamut_boundary(space);
    HDRSHOT_CHECK(polygon.size() > 30 && polygon.size() < 20000);
    HDRSHOT_CHECK(polygon.front() == polygon.back());
    HDRSHOT_CHECK(perceptual_gamut_boundary(space).data() == polygon.data());
    HDRSHOT_CHECK(outside_distance(polygon, {0, 0}) < 1e-5);
    for (auto p : polygon) HDRSHOT_CHECK(std::isfinite(p[0]) && std::isfinite(p[1]));
    bytes += polygon.size_bytes();
  }
  HDRSHOT_CHECK(bytes < 640000);
  HDRSHOT_CHECK(perceptual_gamut_boundary(WorkingSpace(99)).empty());
  std::cout << "static gamut payload bytes=" << bytes << '\n';
}
void independent_rgb_samples() {
  using namespace analysis_math;
  for (unsigned s = 0; s < 4; ++s) {
    auto polygon = perceptual_gamut_boundary(WorkingSpace(s));
    std::mt19937 generator(20260920);
    std::uniform_real_distribution<float> random(0.f, 1.f);
    double maximum = 0.;
    std::size_t count = 0;
    auto check = [&](A3 v) {
      // Alternate linear and PQ-uniform samples. These are not mesh nodes.
      if (s >= 2)
        v = count % 2 ? scale3(v, 10000.f) : a3(pq_decode(v.x), pq_decode(v.y), pq_decode(v.z));
      const auto p = s >= 2 ? itp_from_lms(lms_from_rgb(v, working_gamut(s)))
                            : lab_from_xyz(xyz_from_rgb(v, working_gamut(s)));
      maximum = std::max(maximum, outside_distance(polygon, {p.y, p.z}));
      ++count;
    };
    for (int r = 0; r <= 12; ++r)
      for (int g = 0; g <= 12; ++g)
        for (int b = 0; b <= 12; ++b) check(a3(float(r) / 12.f, float(g) / 12.f, float(b) / 12.f));
    for (int i = 0; i < 6000; ++i) {
      auto v = a3(random(generator), random(generator), random(generator));
      // Explicit independent surface samples in addition to volume samples.
      if (i % 3 == 0) v.x = i % 2 ? 0.f : 1.f;
      if (i % 3 == 1) v.y = i % 2 ? 0.f : 1.f;
      if (i % 3 == 2) v.z = i % 2 ? 0.f : 1.f;
      check(v);
    }
    std::cout << "space=" << s << " independent_samples=" << count
              << " outside_distance=" << maximum << '\n';
    HDRSHOT_CHECK(maximum < (s >= 2 ? 8e-5 : .03));
  }
}
void equal_units_and_reference_white() {
  for (unsigned s = 0; s < 4; ++s)
    for (const auto size : {PixelSize{300, 900}, PixelSize{1800, 280}, PixelSize{700, 700}})
      for (double zoom : {.5, 1., 4., 32.}) {
        Settings settings; settings.working_space = WorkingSpace(s);
        const auto c = vector_calibration(settings, VectorMode::perceptual,
                                         std::uint32_t(size.width), std::uint32_t(size.height), zoom);
        const auto x = project_vector({.1, 0}, c, zoom);
        const auto y = project_vector({0, .1}, c, zoom);
        HDRSHOT_CHECK_NEAR((x[0] - .5) * size.width, (y[1] - .5) * size.height, 1e-8);
        settings.reference_white_nits = 100;
        const auto other = vector_calibration(settings, VectorMode::perceptual,
                                             std::uint32_t(size.width), std::uint32_t(size.height), zoom);
        HDRSHOT_CHECK(c.x_extent == other.x_extent && c.y_extent == other.y_extent);
      }
}
} // namespace
int main() {
  return hdrshot::test::run({{"four immutable closed whole-volume outlines", immutable_tables},
                            {"independent full RGB volume and surface samples", independent_rgb_samples},
                            {"equal color-plane units independent of white and aspect", equal_units_and_reference_white}});
}
