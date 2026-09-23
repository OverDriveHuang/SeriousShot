#include "domain/analysis/color_difference.hpp"

#include <algorithm>
#include <cmath>

namespace hdrshot::analysis {
namespace {
constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double rad = pi / 180.0;
constexpr double deg = 180.0 / pi;

bool finite3(const std::array<double, 3> &v) {
  return std::all_of(v.begin(), v.end(), [](double x) { return std::isfinite(x); });
}
double hue_angle(double b, double a) {
  double h = std::atan2(b, a) * deg;
  if (h < 0)
    h += 360.0;
  return h;
}
} // namespace

std::optional<double> delta_e_itp(const std::array<double, 3> &a,
                                  const std::array<double, 3> &b) {
  if (!finite3(a) || !finite3(b))
    return std::nullopt;
  const double di = a[0] - b[0];
  const double dt = a[1] - b[1];
  const double dp = a[2] - b[2];
  const double value = 720.0 * std::sqrt(di * di + dt * dt + dp * dp);
  return std::isfinite(value) && value >= 0.0 ? std::optional<double>(value)
                                               : std::nullopt;
}

std::optional<double> delta_e_2000(const std::array<double, 3> &a,
                                   const std::array<double, 3> &b) {
  if (!finite3(a) || !finite3(b))
    return std::nullopt;
  // Sharma, Wu and Dalal, "The CIEDE2000 Color-Difference Formula:
  // Implementation Notes, Supplementary Test Data, and Mathematical
  // Observations", Color Research and Application 30(1), 2005. This is the
  // standard formula, with the three parametric factors fixed to one.
  const double l1 = a[0], aa1 = a[1], bb1 = a[2];
  const double l2 = b[0], aa2 = b[1], bb2 = b[2];
  const double c1 = std::hypot(aa1, bb1);
  const double c2 = std::hypot(aa2, bb2);
  const double cbar = (c1 + c2) * .5;
  const double cbar7 = std::pow(cbar, 7.0);
  const double twentyFive7 = std::pow(25.0, 7.0);
  const double g = .5 * (1.0 - std::sqrt(cbar7 / (cbar7 + twentyFive7)));
  const double ap1 = (1.0 + g) * aa1;
  const double ap2 = (1.0 + g) * aa2;
  const double cp1 = std::hypot(ap1, bb1);
  const double cp2 = std::hypot(ap2, bb2);
  const double hp1 = (cp1 == 0.0 && bb1 == 0.0) ? 0.0 : hue_angle(bb1, ap1);
  const double hp2 = (cp2 == 0.0 && bb2 == 0.0) ? 0.0 : hue_angle(bb2, ap2);
  const double d_l = l2 - l1;
  const double d_c = cp2 - cp1;
  double d_h = hp2 - hp1;
  if (cp1 * cp2 == 0.0)
    d_h = 0.0;
  else if (d_h > 180.0)
    d_h -= 360.0;
  else if (d_h < -180.0)
    d_h += 360.0;
  const double d_hp = 2.0 * std::sqrt(cp1 * cp2) * std::sin(d_h * rad * .5);
  const double lbar = (l1 + l2) * .5;
  const double cbarp = (cp1 + cp2) * .5;
  double hbarp = hp1 + hp2;
  if (cp1 * cp2 == 0.0)
    hbarp = hp1 + hp2;
  else if (std::abs(hp1 - hp2) <= 180.0)
    hbarp = (hp1 + hp2) * .5;
  else if (hp1 + hp2 < 360.0)
    hbarp = (hp1 + hp2 + 360.0) * .5;
  else
    hbarp = (hp1 + hp2 - 360.0) * .5;
  const double t = 1.0 - .17 * std::cos((hbarp - 30.0) * rad) +
                   .24 * std::cos(2.0 * hbarp * rad) +
                   .32 * std::cos((3.0 * hbarp + 6.0) * rad) -
                   .20 * std::cos((4.0 * hbarp - 63.0) * rad);
  const double dtheta = 30.0 * std::exp(-std::pow((hbarp - 275.0) / 25.0, 2.0));
  const double rc = 2.0 * std::sqrt(std::pow(cbarp, 7.0) /
                                     (std::pow(cbarp, 7.0) + twentyFive7));
  const double sl = 1.0 + .015 * std::pow(lbar - 50.0, 2.0) /
                              std::sqrt(20.0 + std::pow(lbar - 50.0, 2.0));
  const double sc = 1.0 + .045 * cbarp;
  const double sh = 1.0 + .015 * cbarp * t;
  const double rt = -std::sin(2.0 * dtheta * rad) * rc;
  const double x = d_l / sl;
  const double y = d_c / sc;
  const double z = d_hp / sh;
  const double value = std::sqrt(x * x + y * y + z * z + rt * y * z);
  return std::isfinite(value) && value >= 0.0 ? std::optional<double>(value)
                                               : std::nullopt;
}
} // namespace hdrshot::analysis
