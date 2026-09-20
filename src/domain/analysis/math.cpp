#include "domain/analysis/math.hpp"
#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

namespace hdrshot::analysis {
namespace {
using namespace analysis_math;
A3 triple(const Rgb &v) { return a3(float(v[0]), float(v[1]), float(v[2])); }
Rgb rgb(A3 v) { return {v.x, v.y, v.z}; }
std::string number(double v) {
  std::ostringstream out;
  out << std::setprecision(5) << v;
  return out.str();
}
bool adobe(HistogramMode mode) {
  return mode == HistogramMode::rgb_adobe ||
         mode == HistogramMode::parade_adobe;
}
} // namespace
bool is_hdr(WorkingSpace space) {
  return space == WorkingSpace::display_p3_pq ||
         space == WorkingSpace::bt2020_pq;
}
bool valid_settings(const Settings &s) {
  return unsigned(s.working_space) <= 3u &&
         (s.reference_white_nits == 100 || s.reference_white_nits == 203) &&
         (s.blur_sigma_px == 0 || s.blur_sigma_px == .5 ||
          s.blur_sigma_px == 1 || s.blur_sigma_px == 2 ||
          s.blur_sigma_px == 4 || s.blur_sigma_px == 8);
}
bool valid_request(const Request &r) {
  auto axis = [](AxisView v) {
    return std::isfinite(v.zoom) && v.zoom >= 1 && v.zoom <= 128 &&
           std::isfinite(v.pan) && std::abs(v.pan) <= 128;
  };
  auto detail = [](std::uint32_t width, std::uint32_t height,
                   std::uint32_t max_width) {
    return (!width && !height) ||
           (width && height && width <= max_width && height <= 2048);
  };
  if (!valid_settings(r.settings) || unsigned(r.scopes.wave_mode) > 3u ||
      unsigned(r.scopes.histogram_mode) > 5u ||
      unsigned(r.scopes.vector_mode) > 1u || unsigned(r.mask.shape) > 1u ||
      !axis(r.scopes.amplitude_view) || !axis(r.scopes.histogram_view) ||
      !std::isfinite(r.scopes.vector_zoom) || r.scopes.vector_zoom < .125 ||
      !std::isfinite(r.scopes.vector_pan[0]) ||
      !std::isfinite(r.scopes.vector_pan[1]) ||
      std::abs(r.scopes.vector_pan[0]) > 1e6 ||
      std::abs(r.scopes.vector_pan[1]) > 1e6 ||
      r.scopes.vector_zoom > 128 || r.scopes.wave_width == 0 ||
      r.scopes.wave_width > 4096 || r.scopes.wave_height == 0 ||
      r.scopes.wave_height > 2048 || r.scopes.vector_width == 0 ||
      r.scopes.vector_width > 2048 || r.scopes.vector_height == 0 ||
      r.scopes.vector_height > 2048 || r.scopes.histogram_bins < 2 ||
      r.scopes.histogram_bins > max_histogram_bins || r.samples.size() > 256 ||
      !detail(r.scopes.wave_detail_width, r.scopes.wave_detail_height, 4096) ||
      ((!r.scopes.vector_viewport_width) != (!r.scopes.vector_viewport_height)) ||
      !detail(r.scopes.vector_detail_width, r.scopes.vector_detail_height,
              2048))
    return false;
  if (r.mask.enabled &&
      (!std::isfinite(r.mask.bounds.x) || !std::isfinite(r.mask.bounds.y) ||
       !std::isfinite(r.mask.bounds.width) ||
       !std::isfinite(r.mask.bounds.height)))
    return false;
  for (auto &s : r.samples)
    if (s.side != 1 && s.side != 3 && s.side != 5 && s.side != 11 &&
        s.side != 31 && s.side != 101)
      return false;
  return true;
}
bool contains(const Mask &m, double x, double y) {
  return analysis_math::mask_contains(
      m.enabled ? (m.shape == MaskShape::rectangle ? 1u : 2u) : 0u, float(x),
      float(y), float(m.bounds.x), float(m.bounds.y), float(m.bounds.width),
      float(m.bounds.height));
}
bool valid_source_view(const SourceView &v) {
  auto coordinate = [](double x) {
    return std::isfinite(x) && std::abs(x) <= 1e9;
  };
  return valid_settings(v.settings) && v.target_size.width > 0 &&
         v.target_size.height > 0 && v.target_size.width <= 16384 &&
         v.target_size.height <= 16384 && std::isfinite(v.scale) &&
         v.scale >= 1e-9 && v.scale <= 1e9 && coordinate(v.offset_x) &&
         coordinate(v.offset_y) && std::isfinite(v.mask_outside_factor) &&
         v.mask_outside_factor >= 0 && v.mask_outside_factor <= 1 &&
         (!v.operation_overlay ||
          (v.operation_overlay->size == v.target_size &&
           v.operation_overlay->rgba.size() ==
               std::size_t(v.target_size.width) *
                   std::size_t(v.target_size.height) * 4)) &&
         unsigned(v.mask.shape) <= 1u &&
         (!v.mask.enabled ||
          (coordinate(v.mask.bounds.x) && coordinate(v.mask.bounds.y) &&
           coordinate(v.mask.bounds.width) &&
           coordinate(v.mask.bounds.height)));
}
bool valid_report_plan(const ReportPlan &p) {
  return valid_source_view(p.source_view) && !p.source_view.operation_overlay &&
         p.underlay.size == p.overlay.size && p.underlay.size.width > 0 &&
         p.underlay.size.height > 0 && p.underlay.size.width <= 16384 &&
         p.underlay.size.height <= 16384 &&
         p.underlay.rgba.size() == std::size_t(p.underlay.size.width) *
                                       std::size_t(p.underlay.size.height) *
                                       4 &&
         p.overlay.rgba.size() == p.underlay.rgba.size() &&
         std::isfinite(p.ui_white_edr) && p.ui_white_edr > 0 &&
         p.ui_white_edr <= 100 &&
         std::abs(std::int64_t(p.source_rect.x)) <= 1000000000 &&
         std::abs(std::int64_t(p.source_rect.y)) <= 1000000000 &&
         p.source_rect.width == p.source_view.target_size.width &&
         p.source_rect.height == p.source_view.target_size.height;
}
Rect sample_bounds(PixelSize size, const SampleRequest &r) {
  auto radius = std::int64_t(r.side / 2),
       left = std::max<std::int64_t>(0, std::int64_t(r.x) - radius),
       top = std::max<std::int64_t>(0, std::int64_t(r.y) - radius),
       right =
           std::min<std::int64_t>(size.width, std::int64_t(r.x) + radius + 1),
       bottom =
           std::min<std::int64_t>(size.height, std::int64_t(r.y) + radius + 1);
  return {double(left), double(top),
          double(std::max<std::int64_t>(0, right - left)),
          double(std::max<std::int64_t>(0, bottom - top))};
}
std::vector<float> gaussian_weights(double sigma) {
  if (sigma == 0)
    return {1};
  if (!std::isfinite(sigma) || sigma <= 0 || sigma > 8)
    return {};
  int radius = int(std::ceil(3 * sigma));
  std::vector<float> result(std::size_t(radius * 2 + 1));
  double sum = 0;
  for (int j = -radius; j <= radius; ++j)
    sum += std::exp(-double(j * j) / (2 * sigma * sigma));
  for (int j = -radius; j <= radius; ++j)
    result[std::size_t(j + radius)] =
        float(std::exp(-double(j * j) / (2 * sigma * sigma)) / sum);
  return result;
}
double project_axis(double domain, const AxisView &view) {
  return (domain - view.pan) * view.zoom;
}
double axis_value_to_domain(const Settings &s, HistogramMode mode,
                            double value) {
  if (mode == HistogramMode::hue)
    return value / 360.0;
  if (adobe(mode) && is_hdr(s.working_space))
    return analysis_math::adobe_coordinate(
        float(value / s.reference_white_nits));
  return is_hdr(s.working_space) ? analysis_math::pq_encode(float(value))
                                 : value / 100.0;
}
double axis_domain_to_value(const Settings &s, HistogramMode mode,
                            double domain) {
  if (mode == HistogramMode::hue)
    return domain * 360;
  if (adobe(mode) && is_hdr(s.working_space))
    return analysis_math::adobe_inverse(float(domain)) * s.reference_white_nits;
  return is_hdr(s.working_space) ? analysis_math::pq_decode(float(domain))
                                 : domain * 100;
}
std::vector<AxisTick> histogram_ticks(const Settings &s, HistogramMode mode,
                                      const AxisView &view) {
  std::vector<AxisTick> out;
  auto tick = [&](double value, std::string label, bool boundary = false) {
    out.push_back({value,
                   project_axis(axis_value_to_domain(s, mode, value), view),
                   std::move(label), true, boundary});
  };
  if (mode == HistogramMode::hue)
    for (int a = 0; a <= 360; a += 60)
      tick(a, std::to_string(a) + "°");
  else if (adobe(mode) && is_hdr(s.working_space)) {
    for (int p : {0, 25, 50, 75, 100}) {
      double value =
          analysis_math::srgb_decode(float(p) / 100) * s.reference_white_nits;
      tick(value, std::to_string(p) + "%", p == 100);
    }
    for (int ev = 1; ev <= 5; ++ev)
      tick(std::exp2(ev) * s.reference_white_nits,
           "+" + std::to_string(ev) + " EV");
  } else if (is_hdr(s.working_space))
    for (double n : {0., 1., 10., 100., 1000., 10000.})
      tick(n, number(n));
  else
    for (int p = 0; p <= 100; p += 25)
      tick(p, std::to_string(p) + "%");
  // Keep global landmarks at all zooms. Supplement only when fewer than two
  // are visible, with signal-domain local ticks using the same inverse map.
  auto visible = std::count_if(out.begin(), out.end(), [](auto &t) {
    return t.position >= 0 && t.position <= 1;
  });
  if (visible < 2 && view.zoom > 2 && mode != HistogramMode::hue)
    for (int i = 1; i < 5; ++i) {
      double domain = view.pan + double(i) / (4 * view.zoom);
      if (domain < 0 || domain > 1)
        continue;
      double value = axis_domain_to_value(s, mode, domain);
      out.push_back(
          {value, project_axis(domain, view), number(value), false, false});
    }
  return out;
}
std::vector<AxisTick> amplitude_ticks(const Settings &s, const AxisView &view) {
  return histogram_ticks(s, HistogramMode::intensity, view);
}
std::array<double, 2> vector_domain_extents(const Settings &s, VectorMode mode,
                                           std::uint32_t width,
                                           std::uint32_t height) {
  const double extent =
      mode == VectorMode::ycbcr ? .55 : (is_hdr(s.working_space) ? .5 : 150.0);
  const double aspect = double(std::max(1u, width)) / std::max(1u, height);
  return {extent * std::max(1.0, aspect), extent * std::max(1.0, 1 / aspect)};
}
std::array<double, 2> vector_detail_extents(const Request &r) {
  const auto &o = r.scopes;
  auto extents = vector_domain_extents(r.settings, o.vector_mode,
      o.vector_viewport_width ? o.vector_viewport_width : o.vector_detail_width,
      o.vector_viewport_height ? o.vector_viewport_height : o.vector_detail_height);
  for (auto &extent : extents)
    extent /= o.vector_zoom;
  return extents;
}
VectorCalibration vector_calibration(const Settings &s, VectorMode mode,
                                     std::uint32_t width, std::uint32_t height,
                                     double zoom, std::array<double, 2> center) {
  VectorCalibration c;
  const auto extents = vector_domain_extents(s, mode, width, height);
  c.x_extent = extents[0];
  c.y_extent = extents[1];
  c.x_label =
      mode == VectorMode::ycbcr ? "Cb" : (is_hdr(s.working_space) ? "T" : "a*");
  c.y_label =
      mode == VectorMode::ycbcr ? "Cr" : (is_hdr(s.working_space) ? "P" : "b*");
  // BT.2100 nominal chroma is +/-0.5; BT.2124 scales T = 0.5*Ct.
  // Reference decoration only: do not clamp the floating-point samples.
  if (mode == VectorMode::ycbcr)
    c.reference_extent = {{.5, .5}};
  else if (is_hdr(s.working_space))
    c.reference_extent = {{.25, .5}};
  auto ticks = [&](double half_span, std::size_t axis) {
    std::vector<AxisTick> values;
    const double visible = half_span / zoom;
    const double limit = c.reference_extent ? (*c.reference_extent)[axis]
                                           : std::abs(center[axis]) + visible;
    const double low = std::max(-limit, center[axis] - visible);
    const double high = std::min(limit, center[axis] + visible);
    if (low > high)
      return values;
    const double span = std::min(visible, limit);
    const double raw_step = span / 3;
    const double magnitude = std::pow(10., std::floor(std::log10(raw_step)));
    const double fraction = raw_step / magnitude;
    const double step = (fraction <= 1   ? 1
                         : fraction <= 2 ? 2
                         : fraction <= 5 ? 5
                                         : 10) *
                        magnitude;
    const int first = int(std::ceil(low / step));
    const int last = int(std::floor(high / step));
    for (int i = first; i <= last; ++i) {
      const double value = i * step;
      values.push_back(
          {value, .5 + (value - center[axis]) / (2 * visible), number(value), true, false});
    }
    if (c.reference_extent)
      for (double value : {-limit, limit}) {
        if (value < low || value > high)
          continue;
        const auto existing = std::find_if(values.begin(), values.end(), [&](const auto &t) {
          return std::abs(t.value - value) < 1e-12;
        });
        if (existing != values.end())
          existing->boundary = true;
        else
          values.push_back({value, .5 + (value - center[axis]) / (2 * visible), number(value), true, true});
      }
    // Endpoints remain readable before optional interior labels when zoomed out.
    std::stable_sort(values.begin(), values.end(), [](const auto &a, const auto &b) {
      return a.boundary > b.boundary;
    });
    return values;
  };
  c.x_ticks = ticks(c.x_extent, 0);
  c.y_ticks = ticks(c.y_extent, 1);
  if (mode == VectorMode::ycbcr) {
    const std::array<Rgb, 6> primaries{
        {{1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}, {1, 0, 1}}};
    const std::array<const char *, 6> labels{"R", "Y", "G", "C", "B", "M"};
    for (std::size_t i = 0; i < primaries.size(); ++i) {
      auto rgb = primaries[i];
      auto a = analysis_math::ncl_from_signal(
          triple(rgb), analysis_math::working_gamut(unsigned(s.working_space)));
      auto b = analysis_math::ncl_from_signal(
          analysis_math::scale3(triple(rgb), .75f),
          analysis_math::working_gamut(unsigned(s.working_space)));
      std::array<float, 3> color{float(rgb[0]), float(rgb[1]), float(rgb[2])};
      c.full_targets.push_back({{a.y, a.z}, color, labels[i]});
      c.reduced_targets.push_back({{b.y, b.z}, color, labels[i]});
    }
    double blue = -.48 / .41, green = (-.299 - .114 * blue) / .587;
    auto d = analysis_math::ncl_from_signal(
        analysis_math::a3(1, float(green), float(blue)),
        analysis_math::working_gamut(unsigned(s.working_space)));
    double n = std::hypot(d.y, d.z);
    c.skin_direction = {d.y / n, d.z / n};
  } else if (is_hdr(s.working_space)) {
    double angle = 122.5 * std::acos(-1.0) / 180, x = .5 * std::cos(angle),
           y = std::sin(angle), n = std::hypot(x, y);
    c.skin_direction = {x / n, y / n};
  } else {
    double a = 46 * std::acos(-1.0) / 180;
    c.skin_direction = {std::cos(a), std::sin(a)};
  }
  return c;
}
std::array<double, 2> project_vector(std::array<double, 2> value,
                                     const VectorCalibration &c, double zoom,
                                     std::array<double, 2> center) {
  return {.5 + (value[0] - center[0]) * zoom / (2 * c.x_extent),
          .5 + (value[1] - center[1]) * zoom / (2 * c.y_extent)};
}
std::array<float, 3> display_srgb(const Rgb &work, WorkingSpace space) {
  auto v = analysis_math::display_srgb_from_work(triple(work), unsigned(space));
  // UI colors are explicitly SDR; do not feed this visual convenience upstream.
  return {v.x, v.y, v.z};
}
Readout make_readout(const Rgb &source, const Rgb &work, std::uint64_t valid,
                     std::uint64_t invalid, const Settings &s) {
  Readout r;
  r.valid_count = valid;
  r.invalid_count = invalid;
  if (!valid)
    return r;
  r.source_rgb_edr = source;
  r.work_rgb_edr = work;
  auto v = analysis_math::signals_from_work(
      triple(work), unsigned(s.working_space), float(s.reference_white_nits));
  r.signal_rgb = rgb(v.encoded);
  r.y_nits = v.y_nit;
  r.intensity = v.intensity;
  r.intensity_nits =
      is_hdr(s.working_space) ? analysis_math::pq_decode(v.intensity) : 0;
  for (int i = 0; i < 3; ++i)
    r.work_rgb_nits[std::size_t(i)] =
        work[std::size_t(i)] * s.reference_white_nits;
  r.perceptual = rgb(v.perceptual);
  r.chroma_plane = {v.perceptual.y, v.perceptual.z};
  r.ycbcr = {v.ncl.y, v.ncl.z};
  r.chroma = v.chroma;
  if (v.hue_valid)
    r.hue_degrees = v.hue;
  r.display_rgb = display_srgb(work, s.working_space);
  return r;
}
ProjectedSample project_sample(const Rgb &work, double x, const Settings &s,
                               const ScopeOptions &o) {
  auto v = analysis_math::signals_from_work(
      triple(work), unsigned(s.working_space), float(s.reference_white_nits));
  ProjectedSample p;
  p.source_x = x;
  p.intensity = v.intensity;
  p.signal_rgb = rgb(v.encoded);
  p.hue_valid = v.hue_valid;
  p.vector = o.vector_mode == VectorMode::ycbcr
                 ? std::array<double, 2>{v.ncl.y, v.ncl.z}
                 : std::array<double, 2>{v.perceptual.y, v.perceptual.z};
  if (o.histogram_mode == HistogramMode::hue)
    p.histogram = {v.hue / 360., v.hue / 360., v.hue / 360.};
  else if (o.histogram_mode == HistogramMode::intensity)
    p.histogram = {v.intensity, v.intensity, v.intensity};
  else
    p.histogram = adobe(o.histogram_mode) ? rgb(v.adobe) : rgb(v.encoded);
  return p;
}
} // namespace hdrshot::analysis
