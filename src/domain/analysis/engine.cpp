#include "domain/analysis/engine.hpp"
#include <algorithm>
#include <chrono>
#include <limits>
#include <numeric>

namespace hdrshot::analysis {
namespace {
using namespace analysis_math;
A3 triple(const std::array<float, 4> &v) { return a3(v[0], v[1], v[2]); }
Rgb rgb(const std::array<float, 4> &v) { return {v[0], v[1], v[2]}; }
std::uint32_t bin(double v, std::uint32_t n) { return signal_bin(float(v), n); }
struct Sum {
  Rgb source{}, work{};
  std::uint64_t n{}, invalid{};
  double x{};
  void add(const std::array<float, 4> &s, const std::array<float, 4> &w,
           double px) {
    if (!finite3(triple(s)) || !finite3(triple(w)) || w[3] == 0) {
      ++invalid;
      return;
    }
    for (std::size_t c = 0; c < 3; ++c) {
      source[c] += s[c];
      work[c] += w[c];
    }
    x += px;
    ++n;
  }
  Readout read(const Settings &settings) const {
    Rgb s = source, w = work;
    if (n)
      for (std::size_t c = 0; c < 3; ++c) {
        s[c] /= double(n);
        w[c] /= double(n);
      }
    return make_readout(s, w, n, invalid, settings);
  }
};
void density_add(DensityGrid &grid, std::uint32_t x, std::uint32_t y,
                 std::array<float, 3> color) {
  auto i = std::size_t(y) * grid.width + x;
  grid.maximum = std::max(grid.maximum, ++grid.counts[i]);
  if (!grid.color_sums.empty())
    for (std::size_t c = 0; c < 3; ++c)
      grid.color_sums[i][c] += color[c];
}
DensityGrid grid(std::uint32_t w, std::uint32_t h, bool colors) {
  DensityGrid g;
  g.width = w;
  g.height = h;
  g.counts.resize(std::size_t(w) * h);
  if (colors)
    g.color_sums.resize(g.counts.size());
  return g;
}
} // namespace
Error analysis_error(const char *reason, ErrorCode code,
                     std::source_location origin) {
  return {
      code, "Analysis", Retryability::same_input, {{"reason", reason}}, origin};
}
Result<FloatImage, Error>
prepare_work(PixelSize size, std::span<const std::array<float, 4>> source,
             const Settings &settings) {
  using Out = Result<FloatImage, Error>;
  if (size.width <= 0 || size.height <= 0 ||
      std::uint64_t(size.width) * std::uint64_t(size.height) != source.size() ||
      !valid_settings(settings))
    return Out::failure(analysis_error("invalid_work_input"));
  try {
    FloatImage output(source.size()), horizontal;
    auto weights = gaussian_weights(settings.blur_sigma_px);
    int radius = int(weights.size() / 2);
    if (radius) {
      horizontal.resize(source.size());
      for (int y = 0; y < size.height; ++y)
        for (int x = 0; x < size.width; ++x) {
          std::array<float, 4> sum{};
          for (int dx = -radius; dx <= radius; ++dx) {
            int xx = x + dx;
            if (xx < 0 || xx >= size.width)
              continue;
            auto v = source[std::size_t(y) * std::size_t(size.width) +
                            std::size_t(xx)];
            if (!finite3(triple(v)))
              continue;
            float weight = weights[std::size_t(dx + radius)];
            for (int c = 0; c < 3; ++c)
              sum[std::size_t(c)] += v[std::size_t(c)] * weight;
            sum[3] += weight;
          }
          horizontal[std::size_t(y) * std::size_t(size.width) +
                     std::size_t(x)] = sum;
        }
    }
    for (int y = 0; y < size.height; ++y)
      for (int x = 0; x < size.width; ++x) {
        auto i = std::size_t(y) * std::size_t(size.width) + std::size_t(x);
        A3 b = triple(source[i]);
        if (!finite3(b)) {
          output[i] = {0, 0, 0, 0};
          continue;
        }
        if (radius) {
          std::array<float, 4> sum{};
          for (int dy = -radius; dy <= radius; ++dy) {
            int yy = y + dy;
            if (yy < 0 || yy >= size.height)
              continue;
            auto v = horizontal[std::size_t(yy) * std::size_t(size.width) +
                                std::size_t(x)];
            float weight = weights[std::size_t(dy + radius)];
            for (int c = 0; c < 4; ++c)
              sum[std::size_t(c)] += v[std::size_t(c)] * weight;
          }
          if (!(sum[3] > 0)) {
            output[i] = {0, 0, 0, 0};
            continue;
          }
          b = a3(sum[0] / sum[3], sum[1] / sum[3], sum[2] / sum[3]);
        }
        auto w = working_rgb(b, unsigned(settings.working_space),
                             float(settings.reference_white_nits));
        output[i] = {w.x, w.y, w.z, finite3(w) ? 1.f : 0.f};
      }
    return Out::success(std::move(output));
  } catch (const std::bad_alloc &) {
    return Out::failure(analysis_error("work_allocation_failed"));
  }
}
SampleResult summarize_sample(PixelSize full, const SampleRequest &sample,
                              const Request &request, Rect bounds,
                              std::span<const std::array<float, 4>> source,
                              std::span<const std::array<float, 4>> work) {
  SampleResult result;
  result.request = sample;
  result.clipped_bounds = bounds;
  if (source.size() != work.size() ||
      source.size() != std::size_t(bounds.width * bounds.height))
    return result;
  Sum sums;
  result.distribution.reserve(source.size());
  for (int y = 0; y < int(bounds.height); ++y)
    for (int x = 0; x < int(bounds.width); ++x) {
      double sx = bounds.x + x + .5, sy = bounds.y + y + .5;
      if (sample.respect_mask && !contains(request.mask, sx, sy))
        continue;
      auto i = std::size_t(y) * std::size_t(bounds.width) + std::size_t(x);
      auto before = sums.n;
      sums.add(source[i], work[i], sx / full.width);
      if (sums.n == before)
        continue;
      result.distribution.push_back(project_sample(
          rgb(work[i]), sx / full.width, request.settings, request.scopes));
    }
  result.mean = sums.read(request.settings);
  if (sums.n)
    result.mean_position =
        project_sample(result.mean.work_rgb_edr, sums.x / double(sums.n),
                       request.settings, request.scopes);
  return result;
}
void refresh_result_projection(ResultData &result, const Request &request) {
  result.revision = request.revision;
  result.settings = request.settings;
  result.scopes = request.scopes;
  result.amplitude_ticks =
      amplitude_ticks(request.settings, request.scopes.amplitude_view);
  result.histogram_ticks =
      histogram_ticks(request.settings, request.scopes.histogram_mode,
                      request.scopes.histogram_view);
  result.vector_calibration = vector_calibration(
      request.settings, request.scopes.vector_mode, request.scopes.vector_width,
      request.scopes.vector_height, request.scopes.vector_zoom, request.scopes.vector_pan);
}
Result<ResultData, Error>
analyze_cpu(PixelSize size, std::span<const std::array<float, 4>> source,
            std::span<const std::array<float, 4>> work,
            const Request &request) {
  using Out = Result<ResultData, Error>;
  if (!valid_request(request) || source.size() != work.size() ||
      size.width <= 0 || size.height <= 0 ||
      std::uint64_t(size.width) * std::uint64_t(size.height) != source.size())
    return Out::failure(analysis_error("invalid_analysis_request"));
  try {
    auto start = std::chrono::steady_clock::now();
    ResultData result;
    refresh_result_projection(result, request);
    auto &o = request.scopes;
    const auto wave_width = std::min(o.wave_width, std::uint32_t(size.width));
    bool intensity = o.wave_mode == WaveMode::intensity ||
                     o.wave_mode == WaveMode::parade_intensity_rgb;
    bool wave_rgb = o.wave_mode != WaveMode::intensity;
    if (o.waveform_visible)
      for (int c = 0; c < 4; ++c)
        if (c == 0 ? intensity : wave_rgb)
          result.waveform[std::size_t(c)] =
              grid(wave_width, o.wave_height, c == 0);
    if (o.vector_visible) {
      result.vectorscope = grid(o.vector_width, o.vector_height, true);
      auto base = vector_calibration(request.settings, o.vector_mode, 1, 1, 1);
      result.vector_grid_x_extent = base.x_extent;
      result.vector_grid_y_extent = base.y_extent;
      // Mask/view changes must not move the grid origin or discard unseen
      // colors. Reduce the full immutable working image, not the current ROI.
      for (std::size_t i = 0; i < work.size(); ++i) {
        if (!finite3(triple(source[i])) || work[i][3] == 0)
          continue;
        auto signals = signals_from_work(
            triple(work[i]), unsigned(request.settings.working_space),
            float(request.settings.reference_white_nits));
        if (!signals.valid)
          continue;
        auto plane = o.vector_mode == VectorMode::ycbcr ? signals.ncl
                                                        : signals.perceptual;
        result.vector_grid_x_extent =
            std::max(result.vector_grid_x_extent, double(std::abs(plane.y)));
        result.vector_grid_y_extent =
            std::max(result.vector_grid_y_extent, double(std::abs(plane.z)));
      }
    }
    bool hist_rgb = o.histogram_mode != HistogramMode::intensity &&
                    o.histogram_mode != HistogramMode::hue;
    if (o.histogram_visible)
      for (int c = 0; c < 4; ++c)
        if (hist_rgb ? c > 0 : c == 0) {
          result.histograms[std::size_t(c)].counts.resize(o.histogram_bins);
          result.histograms[std::size_t(c)].color_sums.resize(o.histogram_bins);
        }
    Sum all;
    for (int y = 0; y < size.height; ++y)
      for (int x = 0; x < size.width; ++x) {
        if (!contains(request.mask, x + .5, y + .5))
          continue;
        auto i = std::size_t(y) * std::size_t(size.width) + std::size_t(x);
        auto s = signals_from_work(
            triple(work[i]), unsigned(request.settings.working_space),
            float(request.settings.reference_white_nits));
        if (!s.valid) {
          ++all.invalid;
          continue;
        }
        auto before = all.n;
        all.add(source[i], work[i], (x + .5) / size.width);
        if (before == all.n)
          continue;
        result.hue_count += s.hue_valid ? 1u : 0u;
        auto p = project_sample(rgb(work[i]), (x + .5) / size.width,
                                request.settings, o);
        auto color = display_srgb(rgb(work[i]), request.settings.working_space);
        if (o.waveform_visible) {
          std::array<double, 4> values{p.intensity, p.signal_rgb[0],
                                       p.signal_rgb[1], p.signal_rgb[2]};
          for (std::size_t c = 0; c < 4; ++c)
            if (!result.waveform[c].counts.empty()) {
              density_add(result.waveform[c],
                          source_column_bin(unsigned(x), unsigned(size.width),
                                            wave_width),
                          bin(values[c], o.wave_height), color);
            }
        }
        if (o.vector_visible) {
          const std::array<double, 2> v{
              .5 + p.vector[0] / (2 * result.vector_grid_x_extent),
              .5 + p.vector[1] / (2 * result.vector_grid_y_extent)};
          density_add(result.vectorscope, bin(v[0], o.vector_width),
                      bin(v[1], o.vector_height), color);
        }
        if (o.histogram_visible &&
            (o.histogram_mode != HistogramMode::hue || s.hue_valid))
          for (std::size_t c = 0; c < 4; ++c)
            if (!result.histograms[c].counts.empty()) {
              auto &h = result.histograms[c];
              auto b = bin(p.histogram[c ? c - 1 : 0], o.histogram_bins);
              h.maximum = std::max(h.maximum, ++h.counts[b]);
              for (std::size_t k = 0; k < 3; ++k)
                h.color_sums[b][k] += color[k];
            }
      }
    for (auto &wave : result.waveform)
      set_waveform_column_coverage(wave, std::uint32_t(size.width));
    result.valid_count = all.n;
    result.invalid_count = all.invalid;
    result.mask_mean = all.read(request.settings);
    if (all.n)
      result.mask_mean_position =
          project_sample(result.mask_mean.work_rgb_edr, all.x / double(all.n),
                         request.settings, o);
    auto statistics_end = std::chrono::steady_clock::now();
    result.statistics_ms =
        std::chrono::duration<double, std::milli>(statistics_end - start)
            .count();
    refine_cpu_scopes(result, size, source, work, request);
    auto sample_start = std::chrono::steady_clock::now();
    refresh_cpu_samples(result, size, source, work, request);
    result.sampling_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - sample_start)
                             .count();
    result.retained_bytes =
        result_bytes(result) + source.size_bytes() + work.size_bytes();
    result.peak_bytes = result.retained_bytes;
    return Out::success(std::move(result));
  } catch (const std::bad_alloc &) {
    return Out::failure(analysis_error("statistics_allocation_failed"));
  }
}
void refresh_cpu_samples(ResultData &result, PixelSize size,
                         std::span<const std::array<float, 4>> source,
                         std::span<const std::array<float, 4>> work,
                         const Request &request) {
  result.samples.clear();
  for (const auto &sample : request.samples) {
    auto b = sample_bounds(size, sample);
    FloatImage ss, ww;
    ss.reserve(std::size_t(b.width * b.height));
    ww.reserve(ss.capacity());
    for (int y = int(b.y); y < int(b.y + b.height); ++y)
      for (int x = int(b.x); x < int(b.x + b.width); ++x) {
        auto i = std::size_t(y) * std::size_t(size.width) + std::size_t(x);
        ss.push_back(source[i]);
        ww.push_back(work[i]);
      }
    result.samples.push_back(
        summarize_sample(size, sample, request, b, ss, ww));
  }
}
bool same_statistics_request(const Request &a, const Request &b) {
  return same_wave_statistics_request(a, b) &&
         same_vector_statistics_request(a, b) &&
         same_histogram_statistics_request(a, b);
}
bool same_wave_statistics_request(const Request &a, const Request &b) {
  auto &x = a.scopes;
  auto &y = b.scopes;
  return a.settings == b.settings && a.mask == b.mask &&
         x.waveform_visible == y.waveform_visible &&
         (!x.waveform_visible ||
          (x.wave_mode == y.wave_mode && x.wave_width == y.wave_width &&
           x.wave_height == y.wave_height));
}
bool same_vector_statistics_request(const Request &a, const Request &b) {
  auto &x = a.scopes;
  auto &y = b.scopes;
  return a.settings == b.settings && a.mask == b.mask &&
         x.vector_visible == y.vector_visible &&
         (!x.vector_visible ||
          (x.vector_mode == y.vector_mode && x.vector_width == y.vector_width &&
           x.vector_height == y.vector_height));
}
bool same_histogram_statistics_request(const Request &a, const Request &b) {
  auto &x = a.scopes;
  auto &y = b.scopes;
  return a.settings == b.settings && a.mask == b.mask &&
         x.histogram_visible == y.histogram_visible &&
         (!x.histogram_visible || (x.histogram_mode == y.histogram_mode &&
                                   x.histogram_bins == y.histogram_bins));
}
bool same_detail_request(const Request &a, const Request &b) {
  return same_wave_detail_request(a, b) && same_vector_detail_request(a, b);
}
bool same_wave_detail_request(const Request &a, const Request &b) {
  const auto &x = a.scopes;
  const auto &y = b.scopes;
  return a.settings == b.settings && a.mask == b.mask &&
         x.wave_detail_width == y.wave_detail_width &&
         x.wave_detail_height == y.wave_detail_height &&
         (!x.wave_detail_width ||
          (x.wave_mode == y.wave_mode && x.amplitude_view == y.amplitude_view &&
           x.waveform_visible == y.waveform_visible));
}
bool same_vector_detail_request(const Request &a, const Request &b) {
  const auto &x = a.scopes;
  const auto &y = b.scopes;
  return a.settings == b.settings && a.mask == b.mask &&
         x.vector_detail_width == y.vector_detail_width &&
         x.vector_detail_height == y.vector_detail_height &&
         (!x.vector_detail_width ||
          (x.vector_mode == y.vector_mode && x.vector_zoom == y.vector_zoom &&
           x.vector_pan == y.vector_pan &&
           vector_detail_extents(a) == vector_detail_extents(b) &&
           x.vector_visible == y.vector_visible));
}
void refine_cpu_scopes(ResultData &result, PixelSize size,
                       std::span<const std::array<float, 4>> source,
                       std::span<const std::array<float, 4>> work,
                       const Request &request, bool refine_wave,
                       bool refine_vector) {
  const auto &o = request.scopes;
  if (refine_wave)
    result.waveform_detail = {};
  if (refine_vector)
    result.vectorscope_detail = {};
  result.detail_ms = result.detail_gpu_ms = 0;
  const bool wave = refine_wave && o.waveform_visible && o.wave_detail_width;
  const bool vector =
      refine_vector && o.vector_visible && o.vector_detail_width;
  if (!wave && !vector)
    return;
  const auto start = std::chrono::steady_clock::now();
  const auto ww = std::min(o.wave_detail_width, std::uint32_t(size.width));
  if (wave)
    for (std::size_t c = 0; c < 4; ++c)
      if (c == 0 ? o.wave_mode == WaveMode::intensity ||
                       o.wave_mode == WaveMode::parade_intensity_rgb
                 : o.wave_mode != WaveMode::intensity)
        result.waveform_detail[c] = grid(ww, o.wave_detail_height, c == 0);
  if (refine_wave)
    result.waveform_detail_view = o.amplitude_view;
  if (vector) {
    result.vectorscope_detail =
        grid(o.vector_detail_width, o.vector_detail_height, true);
    const auto extents = vector_detail_extents(request);
    result.vector_detail_zoom = o.vector_zoom;
    result.vector_detail_center = o.vector_pan;
    result.vector_detail_x_extent = extents[0];
    result.vector_detail_y_extent = extents[1];
  }
  for (int y = 0; y < size.height; ++y)
    for (int x = 0; x < size.width; ++x) {
      const auto i = std::size_t(y) * size.width + x;
      if (!contains(request.mask, x + .5, y + .5) ||
          !finite3(triple(source[i])) || work[i][3] == 0)
        continue;
      const auto s = signals_from_work(
          triple(work[i]), unsigned(request.settings.working_space),
          float(request.settings.reference_white_nits));
      if (!s.valid)
        continue;
      const auto color =
          display_srgb(rgb(work[i]), request.settings.working_space);
      if (wave) {
        const std::array<double, 4> values{s.intensity, s.encoded.x,
                                           s.encoded.y, s.encoded.z};
        for (std::size_t c = 0; c < 4; ++c)
          if (!result.waveform_detail[c].counts.empty()) {
            const auto v = project_axis(values[c], o.amplitude_view);
            if (v >= 0 && v <= 1)
              density_add(
                  result.waveform_detail[c],
                  source_column_bin(unsigned(x), unsigned(size.width), ww),
                  bin(v, o.wave_detail_height), color);
          }
      }
      if (vector) {
        const auto plane =
            o.vector_mode == VectorMode::ycbcr ? s.ncl : s.perceptual;
        const double vx = .5 + (plane.y - o.vector_pan[0]) / (2 * result.vector_detail_x_extent),
                     vy = .5 + (plane.z - o.vector_pan[1]) / (2 * result.vector_detail_y_extent);
        if (vx >= 0 && vx <= 1 && vy >= 0 && vy <= 1)
          density_add(result.vectorscope_detail, bin(vx, o.vector_detail_width),
                      bin(vy, o.vector_detail_height), color);
      }
    }
  if (refine_wave)
    for (auto &grid : result.waveform_detail)
      set_waveform_column_coverage(grid, unsigned(size.width));
  result.detail_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - start)
                         .count();
}
std::size_t result_bytes(const ResultData &r) {
  std::size_t n = sizeof(r);
  for (auto &g : r.waveform)
    n += g.counts.capacity() * sizeof(std::uint32_t) +
         g.color_sums.capacity() * sizeof(std::array<float, 3>) +
         g.column_coverage.capacity() * sizeof(float);
  n += r.vectorscope.counts.capacity() * sizeof(std::uint32_t) +
       r.vectorscope.color_sums.capacity() * sizeof(std::array<float, 3>);
  for (auto &g : r.waveform_detail)
    n += g.counts.capacity() * sizeof(std::uint32_t) +
         g.color_sums.capacity() * sizeof(std::array<float, 3>) +
         g.column_coverage.capacity() * sizeof(float);
  n +=
      r.vectorscope_detail.counts.capacity() * sizeof(std::uint32_t) +
      r.vectorscope_detail.color_sums.capacity() * sizeof(std::array<float, 3>);
  for (auto &h : r.histograms)
    n += h.counts.capacity() * sizeof(std::uint64_t) +
         h.color_sums.capacity() * sizeof(std::array<float, 3>);
  n += r.samples.capacity() * sizeof(SampleResult);
  for (auto &s : r.samples)
    n += s.distribution.capacity() * sizeof(ProjectedSample);
  auto ticks = [&](const std::vector<AxisTick> &values) {
    n += values.capacity() * sizeof(AxisTick);
    for (const auto &v : values)
      n += v.label.capacity();
  };
  ticks(r.amplitude_ticks);
  ticks(r.histogram_ticks);
  ticks(r.vector_calibration.x_ticks);
  ticks(r.vector_calibration.y_ticks);
  auto targets = [&](const std::vector<ReferenceTarget> &values) {
    n += values.capacity() * sizeof(ReferenceTarget);
    for (const auto &v : values)
      n += v.label.capacity();
  };
  targets(r.vector_calibration.full_targets);
  targets(r.vector_calibration.reduced_targets);
  n += r.vector_calibration.x_label.capacity() +
       r.vector_calibration.y_label.capacity();
  return n;
}
std::size_t additional_result_bytes(const ResultData &r,
                                    const ResultData &other) {
  auto bytes = result_bytes(r);
  auto subtract_shared = [&](const auto &a, const auto &b) {
    if (a.shares_storage_with(b))
      bytes -= a.capacity() * sizeof(*a.data());
  };
  for (std::size_t c = 0; c < 4; ++c) {
    subtract_shared(r.waveform[c].counts, other.waveform[c].counts);
    subtract_shared(r.waveform[c].color_sums, other.waveform[c].color_sums);
    subtract_shared(r.waveform[c].column_coverage,
                    other.waveform[c].column_coverage);
    subtract_shared(r.waveform_detail[c].counts,
                    other.waveform_detail[c].counts);
    subtract_shared(r.waveform_detail[c].color_sums,
                    other.waveform_detail[c].color_sums);
    subtract_shared(r.waveform_detail[c].column_coverage,
                    other.waveform_detail[c].column_coverage);
    subtract_shared(r.histograms[c].counts, other.histograms[c].counts);
    subtract_shared(r.histograms[c].color_sums, other.histograms[c].color_sums);
  }
  subtract_shared(r.vectorscope.counts, other.vectorscope.counts);
  subtract_shared(r.vectorscope.color_sums, other.vectorscope.color_sums);
  subtract_shared(r.vectorscope_detail.counts, other.vectorscope_detail.counts);
  subtract_shared(r.vectorscope_detail.color_sums,
                  other.vectorscope_detail.color_sums);
  for (const auto &sample : r.samples)
    for (const auto &other_sample : other.samples)
      if (sample.distribution.shares_storage_with(other_sample.distribution)) {
        subtract_shared(sample.distribution, other_sample.distribution);
        break;
      }
  return bytes;
}
void set_waveform_column_coverage(DensityGrid &grid,
                                  std::uint32_t source_width) {
  if (grid.counts.empty() || !grid.width)
    return;
  grid.column_coverage.clear();
  grid.column_coverage.resize(grid.width);
  for (std::uint32_t x = 0; x < source_width; ++x)
    ++grid.column_coverage[analysis_math::source_column_bin(x, source_width,
                                                            grid.width)];
  grid.maximum_density = 0;
  for (std::uint32_t y = 0; y < grid.height; ++y)
    for (std::uint32_t x = 0; x < grid.width; ++x)
      if (grid.column_coverage[x] > 0)
        grid.maximum_density =
            std::max(grid.maximum_density,
                     double(grid.counts[std::size_t(y) * grid.width + x]) /
                         grid.column_coverage[x]);
}
} // namespace hdrshot::analysis
