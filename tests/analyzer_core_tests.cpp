#include "domain/analysis/engine.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "domain/color/pq_reference_white_mapper.hpp"
#include "platform/cpu/cpu_analysis_port.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <limits>
#include <numeric>
#include <thread>

namespace {
using namespace hdrshot;
using namespace hdrshot::analysis;
using V = std::array<double, 3>;
using M = std::array<V, 3>;
// Independent binary64 oracle: separate rational D65 matrices and explicit
// XYZ->2020->LMS route, never the production merged P3->LMS or math helpers.
V mul(M m, V v) {
  V out{};
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c)
      out[std::size_t(r)] +=
          m[std::size_t(r)][std::size_t(c)] * v[std::size_t(c)];
  return out;
}
M p3xyz{{{608311. / 1250200, 189793. / 714400, 198249. / 1000160},
         {35783. / 156275, 247089. / 357200, 198249. / 2500400},
         {0, 32229. / 714400, 5220557. / 5000800}}};
M srgbxyz{{{506752. / 1228815, 87881. / 245763, 12673. / 70218},
           {87098. / 409605, 175762. / 245763, 12673. / 175545},
           {7918. / 409605, 87881. / 737289, 1001167. / 1053270}}};
M xyz2020{{{1.716651187971268, -.355670783776392, -.253366281373660},
           {-.666684351832489, 1.616481236634939, .015768545813911},
           {.017639857445311, -.042770613257809, .942103121235474}}};
M xyzsrgb{{{3.2409699419045226, -1.537383177570094, -.4986107602930034},
           {-.9692436362808796, 1.8759675015077202, .0415550574071756},
           {.0556300796969937, -.2039769588889765, 1.0569715142428786}}};
double pq(double n) {
  double t = std::pow(n / 10000, 2610. / 16384);
  return std::pow((3424. / 4096 + 2413. / 128 * t) / (1 + 2392. / 128 * t),
                  2523. / 32);
}
double srgb(double v) {
  return v <= .0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - .055;
}
V oracle_work(V source, int space, double white) {
  if (space < 2)
    for (double &c : source)
      c = std::clamp(c, 0., 1.);
  if (space == 0)
    source = mul(xyzsrgb, mul(p3xyz, source));
  if (space == 3)
    source = mul(xyz2020, mul(p3xyz, source));
  for (double &c : source)
    c = std::clamp(c, 0., space < 2 ? 1. : 10000 / white);
  return source;
}
V oracle_itp(V work, int space, double white) {
  V bt = space == 3 ? work : mul(xyz2020, mul(p3xyz, work));
  V lms = mul(M{{{1688. / 4096, 2146. / 4096, 262. / 4096},
                 {683. / 4096, 2951. / 4096, 462. / 4096},
                 {99. / 4096, 309. / 4096, 3688. / 4096}}},
              bt);
  for (double &v : lms)
    v = pq(std::clamp(v * white, 0., 10000.));
  return mul(M{{{.5, .5, 0},
                {6610. / 8192, -13613. / 8192, 7003. / 8192},
                {17933. / 4096, -17390. / 4096, -543. / 4096}}},
             lms);
}
void independent_color_oracle() {
  double max_work = 0, max_signal = 0, max_percept = 0;
  for (int space = 0; space < 4; ++space)
    for (double white : {100., 203.})
      for (int n = 0; n < 1024; ++n) {
        V source{double((n * 13) % 257) / 7 - 2, double((n * 29) % 263) / 9 - 2,
                 double((n * 37) % 269) / 11 - 2};
        auto expected = oracle_work(source, space, white);
        auto actual = analysis_math::working_rgb(
            {float(source[0]), float(source[1]), float(source[2])},
            unsigned(space), float(white));
        V w{actual.x, actual.y, actual.z};
        for (int c = 0; c < 3; ++c) {
          max_work = std::max(
              max_work, std::abs(w[std::size_t(c)] - expected[std::size_t(c)]));
          HDRSHOT_CHECK_NEAR(w[std::size_t(c)], expected[std::size_t(c)], 2e-5);
        }
        auto signals = analysis_math::signals_from_work(actual, unsigned(space),
                                                        float(white));
        V encoded{signals.encoded.x, signals.encoded.y, signals.encoded.z};
        for (std::size_t c = 0; c < 3; ++c) {
          double e = space >= 2 ? pq(expected[c] * white) : srgb(expected[c]);
          max_signal = std::max(max_signal, std::abs(e - encoded[c]));
          HDRSHOT_CHECK_NEAR(encoded[c], e, 2e-5);
        }
        V percept;
        if (space >= 2)
          percept = oracle_itp(expected, space, white);
        else {
          V xyz = mul(space == 0 ? srgbxyz : p3xyz, expected);
          xyz[0] /= .9504559270516716;
          xyz[2] /= 1.0890577507598784;
          for (double &v : xyz)
            v = v > 216. / 24389 ? std::cbrt(v) : (24389. / 27 * v + 16) / 116;
          percept = {116 * xyz[1] - 16, 500 * (xyz[0] - xyz[1]),
                     200 * (xyz[1] - xyz[2])};
        }
        V got{signals.perceptual.x, signals.perceptual.y, signals.perceptual.z};
        for (std::size_t c = 0; c < 3; ++c) {
          max_percept = std::max(max_percept, std::abs(percept[c] - got[c]));
          HDRSHOT_CHECK_NEAR(percept[c], got[c], space >= 2 ? 1e-4 : 2e-4);
        }
      }
  std::cout << "8192 independent float64 cases: max work=" << max_work
            << " signal=" << max_signal << " perceptual=" << max_percept
            << '\n';
  HDRSHOT_CHECK_NEAR(analysis_math::pq_encode(203), .58068888104161, 1e-5);
  HDRSHOT_CHECK_NEAR(analysis_math::pq_decode(.5f), 92.245708994, 1e-3);
  auto red = make_readout({1, 0, 0}, {1, 0, 0}, 1, 0,
                          {WorkingSpace::display_p3_pq, 203, 0});
  HDRSHOT_CHECK_NEAR(red.y_nits, 46.481836506, 1e-5);
  HDRSHOT_CHECK_NEAR(red.intensity, .43451027, 1e-5);
  HDRSHOT_CHECK(std::abs(red.intensity - pq(red.y_nits)) > .001);
}
void range_and_mean_order() {
  FloatImage source{{0, 0, 0, 1}, {2, 2, 2, 1}};
  Request r;
  r.settings = {WorkingSpace::display_p3_sdr, 203, 0};
  r.scopes.wave_width = 2;
  r.scopes.wave_height = 16;
  r.scopes.vector_width = 16;
  r.scopes.vector_height = 16;
  r.scopes.histogram_bins = 16;
  r.samples = {{1, 0, 0, 3, false}};
  auto work = prepare_work({2, 1}, source, r.settings);
  HDRSHOT_CHECK(work.has_value());
  auto result = analyze_cpu({2, 1}, source, work.value(), r);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK_NEAR(result.value().mask_mean.source_rgb_edr[0], 1, 0);
  HDRSHOT_CHECK_NEAR(result.value().mask_mean.work_rgb_edr[0], .5, 0);
  HDRSHOT_CHECK_NEAR(result.value().mask_mean.signal_rgb[0], .73535698, 1e-6);
  r.settings.reference_white_nits = 100;
  auto same = analyze_cpu({2, 1}, source, work.value(), r);
  HDRSHOT_CHECK(same.has_value());
  HDRSHOT_CHECK(same.value().mask_mean.signal_rgb ==
                result.value().mask_mean.signal_rgb);
  HDRSHOT_CHECK(same.value().mask_mean.perceptual ==
                result.value().mask_mean.perceptual);
  HDRSHOT_CHECK_NEAR(same.value().mask_mean.y_nits, 50, 1e-5);
  auto p3 = analysis_math::working_rgb({1, 0, 0}, 0, 203);
  HDRSHOT_CHECK_NEAR(p3.x, 1, 0);
  HDRSHOT_CHECK_NEAR(p3.y, 0, 0);
  HDRSHOT_CHECK_NEAR(p3.z, 0, 0);
  auto hdr = analysis_math::working_rgb({120, -1, 2}, 2, 100);
  HDRSHOT_CHECK_NEAR(hdr.x, 100, 0);
  HDRSHOT_CHECK_NEAR(hdr.y, 0, 0);
}
void existing_transfer_path_equivalence() {
  std::uint64_t finite_cases = 0;
  for (std::uint32_t bits = 0; bits <= 65535; ++bits) {
    auto decoded = ExtendedP3Mapper::decode_binary16(std::uint16_t(bits));
    if (!decoded)
      continue;
    const float value = decoded.value();
    HDRSHOT_CHECK(
        std::bit_cast<std::uint32_t>(analysis_math::srgb_encode(value)) ==
        std::bit_cast<std::uint32_t>(
            ExtendedP3Mapper::encode_extended_srgb(value)));
    HDRSHOT_CHECK(
        std::bit_cast<std::uint32_t>(analysis_math::srgb_decode(value)) ==
        std::bit_cast<std::uint32_t>(
            ExtendedP3Mapper::inverse_extended_srgb(value)));
    ++finite_cases;
  }
  HDRSHOT_CHECK(finite_cases == 63488);
  for (float breakpoint : {.0031308f, .04045f})
    for (float v : {std::nextafter(breakpoint, 0.f), breakpoint,
                    std::nextafter(breakpoint, 1.f), -breakpoint}) {
      HDRSHOT_CHECK(analysis_math::srgb_encode(v) ==
                    ExtendedP3Mapper::encode_extended_srgb(v));
      HDRSHOT_CHECK(analysis_math::srgb_decode(v) ==
                    ExtendedP3Mapper::inverse_extended_srgb(v));
    }
  double max_code = 0, max_nit = 0;
  for (int i = 0; i <= 65536; ++i) {
    float code = float(i) / 65536.f;
    float nit =
        i == 0 ? 0.f : float(std::pow(10., -6. + double(i) * 10. / 65536));
    max_code =
        std::max(max_code, std::abs(double(analysis_math::pq_encode(nit)) -
                                    PqReferenceWhiteMapper::st2084_oetf(nit)));
    max_nit =
        std::max(max_nit, std::abs(double(analysis_math::pq_decode(code)) -
                                   PqReferenceWhiteMapper::st2084_eotf(code)));
  }
  HDRSHOT_CHECK(max_code < 2e-5);
  HDRSHOT_CHECK(max_nit < 1.0);
  for (float v : {-1.f, 0.f, 100.f, 203.f, 1000.f, 10000.f, 20000.f})
    HDRSHOT_CHECK_NEAR(analysis_math::pq_encode(v),
                       PqReferenceWhiteMapper::st2084_oetf(v), 2e-5);
  std::cout
      << "legacy transfer equivalence: signed sRGB bit-exact finite half cases="
      << finite_cases << " PQ FP32-vs-FP64 max code=" << max_code
      << " max decoded nit=" << max_nit << '\n';
}
void gaussian_and_mask_independence() {
  for (double sigma : {.5, 1., 2., 4., 8.}) {
    auto w = gaussian_weights(sigma);
    HDRSHOT_CHECK(w.size() == std::size_t(2 * std::ceil(3 * sigma) + 1));
    HDRSHOT_CHECK_NEAR(std::accumulate(w.begin(), w.end(), 0.), 1, 1e-7);
    if (sigma == .5)
      HDRSHOT_CHECK_NEAR(w[3] / w[2], std::exp(-2), 1e-7);
    FloatImage constant(77, {2.3f, .5f, .1f, 1});
    auto image = prepare_work({11, 7}, constant,
                              {WorkingSpace::display_p3_pq, 203, sigma});
    HDRSHOT_CHECK(image.has_value());
    for (auto &p : image.value())
      HDRSHOT_CHECK_NEAR(p[0], 2.3, 2e-6);
  }
  FloatImage impulse(121, {0, 0, 0, 1});
  impulse[60] = {1, 1, 1, 1};
  auto blurred =
      prepare_work({11, 11}, impulse, {WorkingSpace::display_p3_pq, 203, .5});
  HDRSHOT_CHECK(blurred.has_value());
  HDRSHOT_CHECK(blurred.value()[60][0] < 1);
  HDRSHOT_CHECK(blurred.value()[59][0] > 0);
  auto weights = gaussian_weights(.5);
  HDRSHOT_CHECK_NEAR(blurred.value()[60][0], double(weights[2]) * weights[2],
                     1e-7);
  Request r;
  r.settings = {WorkingSpace::display_p3_pq, 203, .5};
  r.samples = {{1, 5, 5, 5, false}, {0, 5, 5, 5, true}};
  r.scopes.wave_width = 16;
  r.scopes.wave_height = 16;
  r.scopes.vector_width = 16;
  r.scopes.vector_height = 16;
  auto a = analyze_cpu({11, 11}, impulse, blurred.value(), r);
  HDRSHOT_CHECK(a.has_value());
  r.mask = {true, MaskShape::rectangle, {0, 0, 4, 4}};
  auto b = analyze_cpu({11, 11}, impulse, blurred.value(), r);
  HDRSHOT_CHECK(b.has_value());
  HDRSHOT_CHECK(a.value().samples[0].mean.work_rgb_edr ==
                b.value().samples[0].mean.work_rgb_edr);
  HDRSHOT_CHECK(b.value().samples[1].mean.valid_count == 1);
}
void invalid_and_mask_counts() {
  FloatImage data(25, {.18f, .18f, .18f, 1});
  data[0][0] = std::numeric_limits<float>::quiet_NaN();
  data[7][2] = std::numeric_limits<float>::infinity();
  Request r;
  r.settings = {WorkingSpace::display_p3_sdr, 203, 1};
  r.scopes.wave_width = 8;
  r.scopes.wave_height = 8;
  r.scopes.vector_width = 8;
  r.scopes.vector_height = 8;
  r.scopes.histogram_bins = 16;
  auto work = prepare_work({5, 5}, data, r.settings);
  HDRSHOT_CHECK(work.has_value());
  auto all = analyze_cpu({5, 5}, data, work.value(), r);
  HDRSHOT_CHECK(all.has_value());
  HDRSHOT_CHECK(all.value().valid_count == 23);
  HDRSHOT_CHECK(all.value().invalid_count == 2);
  HDRSHOT_CHECK(all.value().hue_count == 0);
  HDRSHOT_CHECK(!all.value().mask_mean.hue_degrees);
  HDRSHOT_CHECK_NEAR(all.value().mask_mean.work_rgb_edr[0], .18, 1e-6);
  HDRSHOT_CHECK(std::accumulate(all.value().histograms[0].counts.begin(),
                                all.value().histograms[0].counts.end(),
                                std::uint64_t{}) == 23);
  r.mask = {true, MaskShape::ellipse, {1, 1, 3, 3}};
  auto masked = analyze_cpu({5, 5}, data, work.value(), r);
  HDRSHOT_CHECK(masked.has_value());
  HDRSHOT_CHECK(masked.value().valid_count == 8);
  HDRSHOT_CHECK(masked.value().invalid_count == 1);
  r.mask.bounds = {0, 0, 0, 0};
  auto empty = analyze_cpu({5, 5}, data, work.value(), r);
  HDRSHOT_CHECK(empty.has_value());
  HDRSHOT_CHECK(empty.value().valid_count == 0);
  HDRSHOT_CHECK(empty.value().mask_mean.valid_count == 0);
}
void hue_false_color_and_projection() {
  using namespace analysis_math;
  HDRSHOT_CHECK_NEAR(hue_degrees(1, 0), 0, 0);
  HDRSHOT_CHECK_NEAR(hue_degrees(0, 1), 90, 1e-5);
  HDRSHOT_CHECK_NEAR(hue_degrees(-1, 0), 180, 1e-5);
  HDRSHOT_CHECK_NEAR(hue_degrees(0, -1), 270, 1e-5);
  HDRSHOT_CHECK(hue_degrees(1, -.0001f) > 359);
  HDRSHOT_CHECK(!hue_defined(1.f, 0u));
  HDRSHOT_CHECK(hue_defined(std::nextafter(1.f, 2.f), 1u));
  HDRSHOT_CHECK(!hue_defined(1.f / 720.f, 2u));
  HDRSHOT_CHECK(hue_defined(std::nextafter(1.f / 720.f, 1.f), 3u));
  HDRSHOT_CHECK(!hue_defined(std::numeric_limits<float>::infinity(), 3u));
  HDRSHOT_CHECK(!hue_defined(std::numeric_limits<float>::quiet_NaN(), 3u));
  const std::array<double, 14> ev{-6,  -4.5, -3.5, -2.5, -1.5, -.75, -.25,
                                  .25, .75,  1.5,  2.5,  3.5,  4.5,  6};
  for (std::size_t i = 0; i < ev.size(); ++i) {
    float threshold = float(.18 * std::exp2(ev[i]));
    HDRSHOT_CHECK(false_color_index(threshold) == i + 1);
    HDRSHOT_CHECK(false_color_index(std::nextafter(threshold, 0.f)) == i);
  }
  HDRSHOT_CHECK(false_color_index(0) == 0);
  HDRSHOT_CHECK(false_color_index(.18f) == 7);
  HDRSHOT_CHECK_NEAR(adobe_axis(1), 4. / 9, 1e-7);
  HDRSHOT_CHECK_NEAR(adobe_axis(32), 1, 0);
  HDRSHOT_CHECK_NEAR(adobe_axis(64), 1, 0);
  HDRSHOT_CHECK(adobe_coordinate(64) > 1);
  HDRSHOT_CHECK(signal_bin(1, 16) == 15);
  HDRSHOT_CHECK(signal_bin(-2, 16) == 0);
  Settings settings;
  AxisView view{4, .2};
  double n = 203;
  double domain = axis_value_to_domain(settings, HistogramMode::rgb, n);
  HDRSHOT_CHECK_NEAR(axis_domain_to_value(settings, HistogramMode::rgb, domain),
                     n, .03);
  HDRSHOT_CHECK_NEAR(project_axis(domain, view), (domain - .2) * 4, 0);
  auto c = vector_calibration(settings, VectorMode::perceptual, 400, 200, 2);
  HDRSHOT_CHECK_NEAR(std::atan2(c.skin_direction[1], c.skin_direction[0]) *
                         180 / std::acos(-1.),
                     107.6685046, 1e-6);
  auto p = project_vector({.1, .1}, c, 2);
  HDRSHOT_CHECK_NEAR((p[0] - .5) * 400, (p[1] - .5) * 200, 1e-9);
  auto ncl = vector_calibration(settings, VectorMode::ycbcr, 200, 200, 1);
  HDRSHOT_CHECK_NEAR(std::atan2(ncl.skin_direction[1], ncl.skin_direction[0]) *
                         180 / std::acos(-1.),
                     131.3234, .001);
  HDRSHOT_CHECK(ncl.full_targets.size() == 6);
}
void stable_histogram_and_cpu_adapter() {
  FloatImage pixels(512);
  for (std::size_t i = 0; i < pixels.size(); ++i)
    pixels[i] = {float(i) / 128, float(i % 127) / 31, float(i % 59) / 19, 1};
  auto src = make_cpu_analysis_source({32, 16}, pixels);
  HDRSHOT_CHECK(src.has_value());
  auto port = make_cpu_analysis_port();
  Request r;
  r.scopes.histogram_mode = HistogramMode::rgb_adobe;
  r.scopes.histogram_bins = 32;
  r.scopes.wave_width = 32;
  r.scopes.wave_height = 32;
  r.scopes.vector_width = 32;
  r.scopes.vector_height = 32;
  auto a = port->analyze({src.value(), 1, true}, r);
  HDRSHOT_CHECK(a.has_value());
  r.scopes.histogram_view = {2, .25};
  auto b = port->analyze({src.value(), 1, true}, r);
  HDRSHOT_CHECK(b.has_value());
  for (int c = 1; c < 4; ++c) {
    HDRSHOT_CHECK(a.value()->histograms[std::size_t(c)].counts ==
                  b.value()->histograms[std::size_t(c)].counts);
    HDRSHOT_CHECK(a.value()->histograms[std::size_t(c)].maximum ==
                  b.value()->histograms[std::size_t(c)].maximum);
  }
  r.scopes.histogram_bins = 64;
  auto fine = port->analyze({src.value(), 1, true}, r);
  HDRSHOT_CHECK(fine.has_value());
  for (std::size_t i = 0; i < 32; ++i)
    HDRSHOT_CHECK(a.value()->histograms[1].counts[i] ==
                  fine.value()->histograms[1].counts[i * 2] +
                      fine.value()->histograms[1].counts[i * 2 + 1]);
}
void full_domain_counts_survive_views_and_share_immutable_payload() {
  PixelSize size{17, 11};
  FloatImage pixels(std::size_t(size.width * size.height));
  for (std::size_t i = 0; i < pixels.size(); ++i)
    pixels[i] = {float(i % 17) * 3, float(i % 11) * 4, float(i % 7) * 7, 1};
  auto source = make_cpu_analysis_source(size, pixels);
  HDRSHOT_CHECK(source.has_value());
  auto port = make_cpu_analysis_port();
  Request request;
  request.scopes.wave_mode = WaveMode::parade_intensity_rgb;
  request.scopes.wave_width = 128;
  request.scopes.wave_height = 64;
  request.scopes.vector_width = request.scopes.vector_height = 64;
  auto a = port->analyze({source.value(), 1, true}, request);
  HDRSHOT_CHECK(a.has_value());
  request.revision = 2;
  request.scopes.amplitude_view = {128, .4};
  request.scopes.vector_zoom = 128;
  request.samples = {{0, 8, 5, 3, true}};
  auto b = port->analyze({source.value(), 1, true}, request);
  HDRSHOT_CHECK(b.has_value());
  HDRSHOT_CHECK(b.value()->statistics_ms == 0);
  HDRSHOT_CHECK(b.value()->vectorscope.counts.shares_storage_with(
      a.value()->vectorscope.counts));
  HDRSHOT_CHECK(b.value()->vectorscope.color_sums.shares_storage_with(
      a.value()->vectorscope.color_sums));
  auto total = [](const auto &counts) {
    return std::accumulate(counts.begin(), counts.end(), std::uint64_t{});
  };
  HDRSHOT_CHECK(total(b.value()->vectorscope.counts) == pixels.size());
  for (auto &grid : b.value()->waveform) {
    HDRSHOT_CHECK(grid.width == std::uint32_t(size.width));
    HDRSHOT_CHECK(total(grid.counts) == pixels.size());
    for (std::uint32_t x = 0; x < grid.width; ++x) {
      std::uint64_t column = 0;
      for (std::uint32_t y = 0; y < grid.height; ++y)
        column += grid.counts[y * grid.width + x];
      HDRSHOT_CHECK(column == std::uint32_t(size.height));
    }
  }
  auto edited = *b.value();
  auto old = b.value()->vectorscope.counts[0];
  std::thread reader([&] {
    for (int i = 0; i < 1000; ++i)
      HDRSHOT_CHECK(total(b.value()->vectorscope.counts) == pixels.size());
  });
  edited.vectorscope.counts[0] = old + 7;
  reader.join();
  HDRSHOT_CHECK(b.value()->vectorscope.counts[0] == old);
  HDRSHOT_CHECK(!edited.vectorscope.counts.shares_storage_with(
      b.value()->vectorscope.counts));
  request.mask = {true, MaskShape::rectangle, {0, 0, 2, 2}};
  auto masked = port->analyze({source.value(), 1, true}, request);
  HDRSHOT_CHECK(masked.has_value());
  HDRSHOT_CHECK(total(masked.value()->vectorscope.counts) == 4);
  HDRSHOT_CHECK(masked.value()->vector_grid_x_extent ==
                a.value()->vector_grid_x_extent);
  HDRSHOT_CHECK(masked.value()->vector_grid_y_extent ==
                a.value()->vector_grid_y_extent);
}
void vector_ticks_follow_visible_numeric_domain() {
  for (auto space : {WorkingSpace::display_p3_pq, WorkingSpace::display_p3_sdr})
    for (double zoom : {.125, 1., 128.}) {
      auto c = vector_calibration({space, 203, 0}, VectorMode::perceptual, 1000,
                                  200, zoom);
      HDRSHOT_CHECK(c.x_ticks.size() >= 3 && c.y_ticks.size() >= 3);
      for (auto tick : c.x_ticks) {
        HDRSHOT_CHECK(tick.position >= 0 && tick.position <= 1);
        HDRSHOT_CHECK_NEAR(tick.position,
                           .5 + tick.value * zoom / (2 * c.x_extent), 1e-12);
      }
      if (space == WorkingSpace::display_p3_pq) {
        HDRSHOT_CHECK(c.reference_extent.has_value());
        HDRSHOT_CHECK_NEAR((*c.reference_extent)[0], .25, 0);
        HDRSHOT_CHECK_NEAR((*c.reference_extent)[1], .5, 0);
        for (const auto &t : c.x_ticks) HDRSHOT_CHECK(std::abs(t.value) <= .25);
        for (const auto &t : c.y_ticks) HDRSHOT_CHECK(std::abs(t.value) <= .5);
        if (zoom <= 1.) {
          HDRSHOT_CHECK(std::count_if(c.x_ticks.begin(), c.x_ticks.end(),
              [](const auto &t) { return t.boundary && std::abs(t.value) == .25; }) == 2);
        }
      } else HDRSHOT_CHECK(!c.reference_extent.has_value());
      auto ncl = vector_calibration({space, 203, 0}, VectorMode::ycbcr, 1000, 200, zoom);
      HDRSHOT_CHECK(ncl.reference_extent.has_value());
      for (const auto &t : ncl.x_ticks) HDRSHOT_CHECK(std::abs(t.value) <= .5);
      for (const auto &t : ncl.y_ticks) HDRSHOT_CHECK(std::abs(t.value) <= .5);
    }
}
void detail_domain_is_independent_of_texture_budget() {
  FloatImage source;
  for (unsigned n = 0; n < 1024; ++n)
    source.push_back({.1f + float(n % 31) / 89.f,
                      .1f + float(n % 43) / 137.f,
                      .1f + float(n % 59) / 173.f, 1});
  for (auto space : {WorkingSpace::srgb_sdr, WorkingSpace::display_p3_sdr,
                      WorkingSpace::display_p3_pq, WorkingSpace::bt2020_pq})
    for (auto mode : {VectorMode::perceptual, VectorMode::ycbcr})
      for (auto viewport : {std::array<unsigned, 2>{3712, 344},
                             {344, 3712}, {4312, 2744}}) {
        Request r;
        r.settings.working_space = space;
        auto &o = r.scopes;
        o.waveform_visible = o.histogram_visible = false;
        o.vector_width = o.vector_height = 32;
        o.vector_mode = mode;
        o.vector_zoom = 20.;
        // Offset in both axes, including SDR Lab's different unit scale.
        const double pan_unit = mode == VectorMode::ycbcr || is_hdr(space) ? .017 : 6.;
        o.vector_pan = {pan_unit, -pan_unit};
        o.vector_detail_width = 65;
        o.vector_detail_height = 49;
        o.vector_viewport_width = viewport[0];
        o.vector_viewport_height = viewport[1];
        HDRSHOT_CHECK(valid_request(r));
        const double unit = mode == VectorMode::ycbcr ? .55 : is_hdr(space) ? .5 : 150.;
        const double ratio = double(viewport[0]) / viewport[1];
        const double ex = unit * std::max(1., ratio) / 20.;
        const double ey = unit * std::max(1., 1. / ratio) / 20.;
        auto work = prepare_work({32, 32}, source, r.settings);
        HDRSHOT_CHECK(work.has_value());
        auto result = analyze_cpu({32, 32}, source, work.value(), r);
        HDRSHOT_CHECK(result.has_value());
        HDRSHOT_CHECK_NEAR(result.value().vector_detail_x_extent, ex, 1e-12);
        HDRSHOT_CHECK_NEAR(result.value().vector_detail_y_extent, ey, 1e-12);
        HDRSHOT_CHECK(result.value().vector_detail_center == o.vector_pan);
        std::uint64_t visible = 0;
        for (auto pixel : work.value()) {
          const auto s = analysis_math::signals_from_work({pixel[0], pixel[1], pixel[2]},
              unsigned(space), 203.f);
          const auto plane = mode == VectorMode::ycbcr ? s.ncl : s.perceptual;
          visible += std::abs(plane.y - o.vector_pan[0]) <= ex && std::abs(plane.z - o.vector_pan[1]) <= ey;
        }
        HDRSHOT_CHECK(std::accumulate(result.value().vectorscope_detail.counts.begin(),
            result.value().vectorscope_detail.counts.end(), std::uint64_t{}) == visible);
        auto changed = r;
        changed.scopes.vector_viewport_width += 600;
        HDRSHOT_CHECK(same_statistics_request(r, changed));
        HDRSHOT_CHECK(!same_vector_detail_request(r, changed));
        auto same_aspect = r;
        same_aspect.scopes.vector_viewport_width *= 2;
        same_aspect.scopes.vector_viewport_height *= 2;
        HDRSHOT_CHECK(same_vector_detail_request(r, same_aspect));
        auto pan_only = r;
        pan_only.scopes.vector_pan[0] += ex / 2;
        HDRSHOT_CHECK(same_statistics_request(r, pan_only));
        HDRSHOT_CHECK(!same_vector_detail_request(r, pan_only));
        auto calibration = vector_calibration(r.settings, mode, viewport[0], viewport[1], 20., o.vector_pan);
        const auto center = project_vector(o.vector_pan, calibration, 20., o.vector_pan);
        HDRSHOT_CHECK_NEAR(center[0], .5, 1e-12);
        HDRSHOT_CHECK_NEAR(center[1], .5, 1e-12);
        for (auto tick : calibration.x_ticks) {
          HDRSHOT_CHECK(tick.position >= -1e-12 && tick.position <= 1+1e-12);
          HDRSHOT_CHECK_NEAR(project_vector({tick.value, 0}, calibration, 20., o.vector_pan)[0], tick.position, 1e-12);
        }
      }
  Request invalid;
  invalid.scopes.vector_viewport_width = 4000;
  HDRSHOT_CHECK(!valid_request(invalid));
  invalid = {};
  invalid.scopes.vector_pan[0] = std::numeric_limits<double>::infinity();
  HDRSHOT_CHECK(!valid_request(invalid));
}
void waveform_non_integer_footprint_has_uniform_display_density() {
  const PixelSize size{1878, 3};
  FloatImage pixels(std::size_t(size.width * size.height),
                    {.18f, .18f, .18f, 1});
  Request r;
  r.scopes.wave_width = 1024;
  r.scopes.wave_height = 32;
  r.scopes.vector_visible = r.scopes.histogram_visible = false;
  auto work = prepare_work(size, pixels, r.settings);
  HDRSHOT_CHECK(work.has_value());
  auto result = analyze_cpu(size, pixels, work.value(), r);
  HDRSHOT_CHECK(result.has_value());
  const auto &grid = result.value().waveform[0];
  for (std::uint32_t x = 0; x < grid.width; ++x) {
    double count = 0;
    for (std::uint32_t y = 0; y < grid.height; ++y)
      count += grid.counts[std::size_t(y) * grid.width + x];
    HDRSHOT_CHECK_NEAR(count / grid.column_coverage[x], 3, 0);
  }
  HDRSHOT_CHECK_NEAR(grid.maximum_density, 3, 0);
}
} // namespace
int main() {
  return hdrshot::test::run(
      {{"independent double-precision color oracle", independent_color_oracle},
       {"existing transfer paths retain numeric equivalence",
        existing_transfer_path_equivalence},
       {"range policy and nonlinear mean order", range_and_mean_order},
       {"Gaussian weights, edges and mask independence",
        gaussian_and_mask_independence},
       {"invalid values and pixel-center masks", invalid_and_mask_counts},
       {"Hue, hard EV bands and shared projections",
        hue_false_color_and_projection},
       {"stable full-domain histograms and CPU adapter",
        stable_histogram_and_cpu_adapter},
       {"full-domain view-independent counts and immutable sharing",
        full_domain_counts_survive_views_and_share_immutable_payload},
       {"vector ticks cover visible numeric domain",
        vector_ticks_follow_visible_numeric_domain},
       {"detail color-plane domain is independent of capped texture dimensions",
        detail_domain_is_independent_of_texture_budget},
       {"non-integer source-column footprint density",
        waveform_non_integer_footprint_has_uniform_display_density}});
}
