#include "platform/cpu/cpu_analysis_port.hpp"
#include "platform/windows/windows_analysis_backend.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

using namespace hdrshot;
using namespace hdrshot::analysis;

namespace {
FloatImage pixels() {
  FloatImage values(64);
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) {
      const float p = float(x + y * 8) / 63;
      values[std::size_t(y) * 8 + x] = {p * 3.f, p * .9f - .12f, p * .3f, 1};
    }
  values[0] = {-0.25f, 1.5f, 2.2f, 1};
  values[63] = {std::numeric_limits<float>::quiet_NaN(), 0, 0, 1};
  return values;
}
Request request() {
  Request r;
  r.settings = {WorkingSpace::display_p3_pq, 203., 0.};
  r.scopes.wave_width = 8;
  r.scopes.wave_height = 32;
  r.scopes.vector_width = 64;
  r.scopes.vector_height = 64;
  r.scopes.histogram_bins = 128;
  r.scopes.wave_detail_width = 8;
  r.scopes.wave_detail_height = 64;
  r.scopes.vector_detail_width = 64;
  r.scopes.vector_detail_height = 64;
  r.scopes.vector_viewport_width = 256;
  r.scopes.vector_viewport_height = 128;
  r.samples = {{1, 1, 1, 3, false}, {0, 6, 6, 1, true}};
  return r;
}
template <class T>
void compare_samples(const SharedSamples<T> &a, const SharedSamples<T> &b,
                     std::string_view label) {
  HDRSHOT_CHECK(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); i++)
    if (a[i] != b[i]) {
      std::ostringstream message;
      message << label << " bin=" << i << " GPU=" << a[i] << " CPU=" << b[i];
      throw std::runtime_error(message.str());
    }
}
void native_reference_parity() {
  auto native = make_windows_analysis_backend();
  HDRSHOT_CHECK(native.has_value());
  auto cpu = make_cpu_analysis_port();
  auto source = make_cpu_analysis_source({8, 8}, pixels());
  HDRSHOT_CHECK(source.has_value());
  Input input{source.value(), 7, true};
  auto r = request();
  r.revision = 9;
  auto expected = cpu->analyze(input, r),
       actual = native.value().port->analyze(input, r);
  HDRSHOT_CHECK(expected.has_value());
  HDRSHOT_CHECK(actual.has_value());
  const auto &a = *actual.value();
  const auto &b = *expected.value();
  HDRSHOT_CHECK(a.valid_count == b.valid_count);
  HDRSHOT_CHECK(a.invalid_count == b.invalid_count);
  HDRSHOT_CHECK(a.hue_count == b.hue_count);
  for (int c = 0; c < 4; c++) {
    compare_samples(a.waveform[c].counts, b.waveform[c].counts,
                    "waveform[" + std::to_string(c) + "]");
    compare_samples(a.histograms[c].counts, b.histograms[c].counts,
                    "histogram[" + std::to_string(c) + "]");
    compare_samples(a.waveform_detail[c].counts, b.waveform_detail[c].counts,
                    "waveform_detail[" + std::to_string(c) + "]");
  }
  compare_samples(a.vectorscope.counts, b.vectorscope.counts, "vector");
  compare_samples(a.vectorscope_detail.counts, b.vectorscope_detail.counts,
                  "vector_detail");
  for (int c = 0; c < 3; c++) {
    HDRSHOT_CHECK_NEAR(a.mask_mean.source_rgb_edr[c],
                       b.mask_mean.source_rgb_edr[c], 2e-6);
    HDRSHOT_CHECK_NEAR(a.mask_mean.work_rgb_edr[c], b.mask_mean.work_rgb_edr[c],
                       2e-6);
  }
  HDRSHOT_CHECK(a.samples.size() == b.samples.size());
  for (std::size_t i = 0; i < a.samples.size(); i++)
    HDRSHOT_CHECK_NEAR(a.samples[i].mean.intensity, b.samples[i].mean.intensity,
                       3e-5);
  auto moved = r;
  moved.revision++;
  moved.scopes.vector_pan = {.04, -.02};
  moved.scopes.vector_zoom = 2.0;
  auto refined = native.value().port->analyze(input, moved);
  HDRSHOT_CHECK(refined.has_value());
  HDRSHOT_CHECK(refined.value()->prepare_ms == 0);
  HDRSHOT_CHECK(refined.value()->statistics_ms == 0);
  HDRSHOT_CHECK(refined.value()->waveform[0].counts.shares_storage_with(
      a.waveform[0].counts));
  HDRSHOT_CHECK(refined.value()->histograms[0].counts.shares_storage_with(
      a.histograms[0].counts));
  HDRSHOT_CHECK(refined.value()->vector_detail_center ==
                moved.scopes.vector_pan);
  auto hover = moved;
  hover.revision++;
  hover.samples = {{0, 2, 3, 1, true}};
  auto hovered = native.value().port->analyze(input, hover);
  HDRSHOT_CHECK(hovered.has_value());
  HDRSHOT_CHECK(hovered.value()->prepare_ms == 0);
  HDRSHOT_CHECK(hovered.value()->statistics_ms == 0);
  HDRSHOT_CHECK(hovered.value()->detail_ms == 0);
  HDRSHOT_CHECK(hovered.value()->waveform[0].counts.shares_storage_with(
      refined.value()->waveform[0].counts));
  HDRSHOT_CHECK(hovered.value()->retained_bytes <=
                refined.value()->retained_bytes);
}
void mask_and_work_cache() {
  auto native = make_windows_analysis_backend();
  HDRSHOT_CHECK(native.has_value());
  auto cpu = make_cpu_analysis_port();
  auto source = make_cpu_analysis_source({8, 8}, pixels());
  HDRSHOT_CHECK(source.has_value());
  Input input{source.value(), 1, true};
  auto r = request();
  r.mask = {true, MaskShape::ellipse, {1, 1, 5, 4}};
  r.settings.blur_sigma_px = 1.;
  auto expected = cpu->analyze(input, r),
       actual = native.value().port->analyze(input, r);
  HDRSHOT_CHECK(expected.has_value());
  HDRSHOT_CHECK(actual.has_value());
  HDRSHOT_CHECK(actual.value()->valid_count == expected.value()->valid_count);
  HDRSHOT_CHECK(actual.value()->invalid_count ==
                expected.value()->invalid_count);
  for (int c = 0; c < 3; c++)
    HDRSHOT_CHECK_NEAR(actual.value()->mask_mean.work_rgb_edr[c],
                       expected.value()->mask_mean.work_rgb_edr[c], 3e-5);
  auto gain_only = r;
  gain_only.scopes.colorize = false;
  auto reused = native.value().port->analyze(input, gain_only);
  HDRSHOT_CHECK(reused.has_value());
  HDRSHOT_CHECK(reused.value()->prepare_ms == 0);
  HDRSHOT_CHECK(reused.value()->statistics_ms == 0);

  // A new frozen frame with identical geometry cannot reuse the previous S/W.
  auto replacement = make_cpu_analysis_source(
      {8, 8}, FloatImage(64, {0.02f, 0.03f, 0.04f, 1}));
  HDRSHOT_CHECK(replacement.has_value());
  auto refreshed = native.value().port->analyze(
      {replacement.value(), input.revision + 1, true}, gain_only);
  HDRSHOT_CHECK(refreshed.has_value());
  HDRSHOT_CHECK(refreshed.value()->statistics_ms > 0);
  HDRSHOT_CHECK(std::abs(refreshed.value()->mask_mean.source_rgb_edr[0] -
                         reused.value()->mask_mean.source_rgb_edr[0]) > .1);
}
void detail_projection_is_lazy_and_reused() {
  auto native = make_windows_analysis_backend();
  HDRSHOT_CHECK(native.has_value());
  auto source = make_cpu_analysis_source({8, 8}, pixels());
  HDRSHOT_CHECK(source.has_value());
  Input input{source.value(), 1, true};
  auto r = request();
  r.scopes.wave_detail_width = r.scopes.wave_detail_height = 0;
  r.scopes.vector_detail_width = r.scopes.vector_detail_height = 0;
  auto base = native.value().port->analyze(input, r);
  HDRSHOT_CHECK(base.has_value());
  auto fine = request();
  auto detail = native.value().port->analyze(input, fine);
  HDRSHOT_CHECK(detail.has_value());
  HDRSHOT_CHECK(detail.value()->prepare_ms == 0);
  HDRSHOT_CHECK(detail.value()->statistics_ms == 0);
  HDRSHOT_CHECK(detail.value()->detail_ms > 0);
  HDRSHOT_CHECK(detail.value()->retained_bytes >=
                base.value()->retained_bytes + 64 * 24);
  fine.samples = {{0, 3, 4, 1, true}};
  auto hover = native.value().port->analyze(input, fine);
  HDRSHOT_CHECK(hover.has_value());
  HDRSHOT_CHECK(hover.value()->prepare_ms == 0);
  HDRSHOT_CHECK(hover.value()->statistics_ms == 0);
  HDRSHOT_CHECK(hover.value()->detail_ms == 0);
}
void report_uses_hdr_source_and_fixed_overlay() {
  auto native = make_windows_analysis_backend();
  HDRSHOT_CHECK(native.has_value());
  auto cpu = make_cpu_analysis_port();
  auto source = make_cpu_analysis_source({4, 3}, FloatImage(12, {2, 4, 8, 1}));
  HDRSHOT_CHECK(source.has_value());
  Input input{source.value(), 1, true};
  ReportPlan report;
  report.source_view.target_size = {5, 3};
  report.source_view.scale = 2;
  report.source_view.offset_x = -1;
  report.source_view.offset_y = -1;
  report.source_rect = {1, 2, 5, 3};
  report.underlay.size = {9, 7};
  report.overlay.size = {9, 7};
  report.underlay.rgba.assign(9 * 7 * 4, 255);
  report.overlay.rgba.assign(9 * 7 * 4, 0);
  auto mark = std::size_t(3 * 9 + 2) * 4;
  report.overlay.rgba[mark] = 255;
  report.overlay.rgba[mark + 3] = 128;
  auto expected = cpu->compose_report(input, report),
       actual = native.value().port->compose_report(input, report);
  HDRSHOT_CHECK(expected.has_value());
  HDRSHOT_CHECK(actual.has_value());
  auto a = actual.value()->read_region({0, 0, 9, 7});
  auto b = expected.value()->read_region({0, 0, 9, 7});
  HDRSHOT_CHECK(a.has_value());
  HDRSHOT_CHECK(b.has_value());
  for (std::size_t i = 0; i < a.value().size(); i++)
    HDRSHOT_CHECK_NEAR(a.value()[i], b.value()[i], 2e-5);
  HDRSHOT_CHECK(a.value()[std::size_t(2 * 9 + 1) * 4 + 2] > 7.99f);
  HDRSHOT_CHECK(a.value()[mark + 2] > 3.9f);
}
void clean_roi_bakes_annotation_once() {
  auto native = make_windows_analysis_backend();
  HDRSHOT_CHECK(native.has_value());
  auto cpu = make_cpu_analysis_port();
  const std::array<float, 8> samples{2, 3, 4, 1, 2, 3, 4, 1};
  SelectionRoiView roi;
  roi.size_px = {2, 1};
  roi.source_rect_px = {0, 0, 2, 1};
  roi.encoding = {ColorPrimaries::display_p3, TransferFunction::linear,
                  AlphaMode::opaque, 0};
  roi.rgba_float = samples;
  roi.row_stride_samples = 8;
  roi.float_storage_capacity_samples = 8;
  AnnotationPixelPlan plan;
  plan.output_size_px = {2, 1};
  plan.source_visible_spans.push_back({0, 0, 1});
  AnnotationOwnedSpan mark;
  mark.y = 0;
  mark.x = 1;
  mark.length = 1;
  mark.edge_samples.push_back({.2f, .3f, .4f, .25f});
  plan.annotation_owned_spans.push_back(mark);
  auto expected = cpu->prepare(roi, plan),
       actual = native.value().port->prepare(roi, plan);
  HDRSHOT_CHECK(expected.has_value());
  HDRSHOT_CHECK(actual.has_value());
  auto a = actual.value()->read_region({0, 0, 2, 1});
  auto b = expected.value()->read_region({0, 0, 2, 1});
  HDRSHOT_CHECK(a.has_value());
  HDRSHOT_CHECK(b.has_value());
  for (std::size_t i = 0; i < a.value().size(); i++)
    HDRSHOT_CHECK_NEAR(a.value()[i], b.value()[i], 0);
  HDRSHOT_CHECK_NEAR(a.value()[4], .7, 1e-7);
  HDRSHOT_CHECK_NEAR(a.value()[5], 1.05, 1e-7);
  HDRSHOT_CHECK_NEAR(a.value()[6], 1.4, 1e-7);
}
void six_k_dispatch_covers_every_pixel() {
  constexpr PixelSize size{6016, 3384};
  constexpr std::size_t count = std::size_t(size.width) * size.height;
  FloatImage values(count, {.25f, .5f, 1.25f, 1});
  auto source = make_cpu_analysis_source(size, std::move(values));
  HDRSHOT_CHECK(source.has_value());
  auto native = make_windows_analysis_backend();
  HDRSHOT_CHECK(native.has_value());
  Request r;
  r.settings = {WorkingSpace::display_p3_pq, 203, 0};
  r.scopes.waveform_visible = false;
  r.scopes.vector_visible = false;
  r.scopes.histogram_bins = 16;
  auto started = std::chrono::steady_clock::now();
  auto result = native.value().port->analyze({source.value(), 1, true}, r);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value()->valid_count == count);
  HDRSHOT_CHECK(result.value()->invalid_count == 0);
  std::uint64_t sum{};
  for (auto v : result.value()->histograms[0].counts)
    sum += v;
  HDRSHOT_CHECK(sum == count);
  std::cout << "6K all-pixel native dispatch ms="
            << std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - started)
                   .count()
            << " retained bytes=" << result.value()->retained_bytes << '\n';
}
} // namespace

int main(int argc, char **argv) {
  try {
    native_reference_parity();
    mask_and_work_cache();
    detail_projection_is_lazy_and_reused();
    report_uses_hdr_source_and_fixed_overlay();
    clean_roi_bakes_annotation_once();
    if (argc > 1 && std::string_view(argv[1]) == "--6k")
      six_k_dispatch_covers_every_pixel();
    std::cout << "Windows D3D analysis parity passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
