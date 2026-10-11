#pragma once
#include "domain/analysis/math.hpp"
#include "domain/color/capture_edr_boundary.hpp"
namespace hdrshot {
struct MagnifierReadout {
  bool valid{}, sdr{}, itp_valid{};
  analysis::Rgb source{}, srgb{}, pq{}, nits{}, lab{}, itp{};
  double srgb_y{}, pq_y{}, y_nits{}, lab_chroma{}, tp_chroma{};
  std::optional<double> lab_hue, tp_hue;
};
inline MagnifierReadout magnifier_readout(analysis::Rgb source, double white,
                                          bool tolerance) {
  using namespace analysis_math_double;
  MagnifierReadout r;
  const auto v = a3(source[0], source[1], source[2]);
  if (!finite3(v))
    return r;
  r.valid = true;
  r.source = source;
  r.sdr = !source_requires_hdr(std::max({source[0], source[1], source[2]}),
                               tolerance);
  auto rgb = [](A3 a) { return analysis::Rgb{a.x, a.y, a.z}; };
  const auto s = encode_working_rgb(v, 1u, white),
             p = encode_working_rgb(v, 2u, white), n = scale3(v, white);
  r.srgb = rgb(s);
  r.pq = rgb(p);
  r.nits = rgb(n);
  r.srgb_y = dot3(y_coefficients(1u), s);
  r.pq_y = dot3(y_coefficients(1u), p);
  r.y_nits = dot3(y_coefficients(1u), n);
  auto perceptual = [&](A3 a, unsigned space, analysis::Rgb &values,
                        double &chroma, std::optional<double> &hue) {
    values = rgb(a);
    chroma = a_sqrt(a.y * a.y + a.z * a.z);
    if (hue_defined(chroma, space))
      hue = hue_degrees(a.y, a.z);
  };
  perceptual(lab_from_xyz(xyz_from_rgb(v, 1u)), 1u, r.lab, r.lab_chroma,
             r.lab_hue);
  const auto lms = lms_from_rgb(n, 1u);
  r.itp_valid = finite3(lms) && lms.x >= 0 && lms.y >= 0 && lms.z >= 0;
  if (r.itp_valid)
    perceptual(itp_from_lms(lms), 2u, r.itp, r.tp_chroma, r.tp_hue);
  return r;
}
} // namespace hdrshot
