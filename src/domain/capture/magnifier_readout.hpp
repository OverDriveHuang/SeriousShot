#pragma once
#include "domain/analysis/math.hpp"
#include "domain/color/capture_edr_boundary.hpp"
namespace hdrshot {
struct MagnifierReadout {
  bool valid{}, sdr{}, itp_valid{};
  analysis::Rgb source{}, srgb{}, pq{}, nits{}, lab{}, itp{};
  double srgb_y{}, pq_y{}, y_nits{}, lab_chroma{}, tp_chroma{};
  analysis::Rgb srgb_sdr{}, bt2020_pq{}, srgb_nits{}, bt2020_nits{};
  double srgb_sdr_y{}, bt2020_pq_y{}, srgb_y_nits{}, bt2020_y_nits{};
  std::optional<double> lab_hue, tp_hue;
};
inline MagnifierReadout magnifier_readout(analysis::Rgb source, double white,
                                          bool tolerance,
                                          bool capture_frozen_sdr = false) {
  using namespace analysis_math_double;
  MagnifierReadout r;
  const auto v = a3(source[0], source[1], source[2]);
  if (!finite3(v))
    return r;
  r.valid = true;
  r.source = source;
  r.sdr = capture_frozen_sdr ||
          !source_requires_hdr(std::max({source[0], source[1], source[2]}),
                               tolerance);
  // Match the analyzer's SDR working_rgb policy without changing the sampled
  // source, its absolute luminance, or PQ/ITP values. Keep Lab math in double.
  const auto sdr = r.sdr ? a3(bound(v.x, 0., 1.), bound(v.y, 0., 1.),
                             bound(v.z, 0., 1.)) : v;
  auto rgb = [](A3 a) { return analysis::Rgb{a.x, a.y, a.z}; };
  const auto s = encode_working_rgb(sdr, 1u, white),
             p = encode_working_rgb(v, 2u, white), n = scale3(v, white);
  r.srgb = rgb(s);
  r.pq = rgb(p);
  r.nits = rgb(n);
  r.srgb_y = dot3(y_coefficients(1u), s);
  r.pq_y = dot3(y_coefficients(1u), p);
  r.y_nits = dot3(y_coefficients(1u), n);
  // Reuse the analyzer's source-SDR clipping policy, then convert and clip the
  // target SDR gamut. Physical/PQ readouts always use the untouched source.
  const auto sdr_srgb = rgb_from_xyz(xyz_from_rgb(sdr, 1u), 0u);
  const auto encoded_srgb = encode_working_rgb(
      r.sdr ? a3(bound(sdr_srgb.x, 0., 1.), bound(sdr_srgb.y, 0., 1.),
                 bound(sdr_srgb.z, 0., 1.)) : sdr_srgb,
      0u, white);
  const auto xyz = xyz_from_rgb(v, 1u);
  const auto linear_srgb = rgb_from_xyz(xyz, 0u);
  const auto linear_bt2020 = rgb_from_xyz(xyz, 2u);
  const auto encoded_bt2020 = encode_working_rgb(linear_bt2020, 3u, white);
  const auto nits_srgb = scale3(linear_srgb, white);
  const auto nits_bt2020 = scale3(linear_bt2020, white);
  r.srgb_sdr = rgb(encoded_srgb);
  r.srgb_sdr_y = dot3(y_coefficients(0u), encoded_srgb);
  r.bt2020_pq = rgb(encoded_bt2020);
  r.bt2020_pq_y = dot3(y_coefficients(2u), encoded_bt2020);
  r.srgb_nits = rgb(nits_srgb);
  r.srgb_y_nits = dot3(y_coefficients(0u), nits_srgb);
  r.bt2020_nits = rgb(nits_bt2020);
  r.bt2020_y_nits = dot3(y_coefficients(2u), nits_bt2020);
  auto perceptual = [&](A3 a, unsigned space, analysis::Rgb &values,
                        double &chroma, std::optional<double> &hue) {
    values = rgb(a);
    chroma = a_sqrt(a.y * a.y + a.z * a.z);
    if (hue_defined(chroma, space))
      hue = hue_degrees(a.y, a.z);
  };
  perceptual(lab_from_xyz(xyz_from_rgb(sdr, 1u)), 1u, r.lab, r.lab_chroma,
             r.lab_hue);
  const auto lms = lms_from_rgb(n, 1u);
  r.itp_valid = finite3(lms) && lms.x >= 0 && lms.y >= 0 && lms.z >= 0;
  if (r.itp_valid)
    perceptual(itp_from_lms(lms), 2u, r.itp, r.tp_chroma, r.tp_hue);
  return r;
}
} // namespace hdrshot
