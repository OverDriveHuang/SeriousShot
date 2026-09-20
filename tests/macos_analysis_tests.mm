#include "domain/analysis/engine.hpp"
#include "platform/cpu/cpu_analysis_port.hpp"
#include "platform/macos/macos_analysis_backend.hpp"
#include "platform/macos/macos_gpu_source.hpp"
#include "test_support.hpp"
#import <Foundation/Foundation.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <limits>
#include <mach/mach.h>
#include <numeric>

namespace {
using namespace hdrshot;
using namespace hdrshot::analysis;
LinearSourceRef source(PixelSize size, const FloatImage &pixels) {
  auto device = macos_gpu_device();
  auto descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                   width:NSUInteger(size.width)
                                  height:NSUInteger(size.height)
                               mipmapped:NO];
  descriptor.storageMode = MTLStorageModeShared;
  descriptor.usage = MTLTextureUsageShaderRead;
  auto staging = [device newTextureWithDescriptor:descriptor];
  [staging replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(size.width),
                                         NSUInteger(size.height))
             mipmapLevel:0
               withBytes:pixels.data()
             bytesPerRow:std::size_t(size.width) * 16];
  descriptor.storageMode = MTLStorageModePrivate;
  descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  auto output = [device newTextureWithDescriptor:descriptor];
  auto command = [macos_gpu_queue() commandBuffer];
  auto encoder = [command blitCommandEncoder];
  [encoder copyFromTexture:staging
               sourceSlice:0
               sourceLevel:0
              sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(NSUInteger(size.width),
                                       NSUInteger(size.height), 1)
                 toTexture:output
          destinationSlice:0
          destinationLevel:0
         destinationOrigin:MTLOriginMake(0, 0, 0)];
  [encoder endEncoding];
  auto result = macos_wrap_linear_texture(output, command);
  HDRSHOT_CHECK(result.has_value());
  return result.value();
}
MacAnalysisBackend backend() {
  auto result = make_macos_analysis_backend();
  if (!result) {
    for (auto &[k, v] : result.error().safe_context)
      std::cerr << k << ": " << v << '\n';
  }
  HDRSHOT_CHECK(result.has_value());
  return result.value();
}
void source_and_report_float_poc() {
  auto b = backend();
  FloatImage values(12, {2, 4, 8, 1});
  Input input{source({4, 3}, values), 1, true};
  SourceView view;
  view.target_size = {5, 3};
  view.scale = 2;
  view.offset_x = -1;
  view.offset_y = -1;
  auto rendered = macos_analysis_render_offscreen(input, view);
  HDRSHOT_CHECK(rendered.has_value());
  auto pixels = rendered.value()->read_region({0, 0, 5, 3});
  HDRSHOT_CHECK(pixels.has_value());
  HDRSHOT_CHECK_NEAR(pixels.value()[0], 2, 1e-6);
  HDRSHOT_CHECK_NEAR(pixels.value()[1], 4, 1e-6);
  HDRSHOT_CHECK_NEAR(pixels.value()[2], 8, 1e-6);
  view.settings.working_space = WorkingSpace::srgb_sdr;
  auto clipped = macos_analysis_render_offscreen(input, view);
  HDRSHOT_CHECK(clipped.has_value());
  auto cp = clipped.value()->read_region({0, 0, 1, 1});
  HDRSHOT_CHECK(cp.has_value());
  for (int c = 0; c < 3; ++c)
    HDRSHOT_CHECK_NEAR(cp.value()[std::size_t(c)], 1, 1e-6);
  view.settings.working_space = WorkingSpace::display_p3_pq;
  ReportPlan report;
  report.revision = 7;
  report.source_view = view;
  report.source_rect = {1, 2, 5, 3};
  report.underlay.size = {9, 7};
  report.overlay.size = {9, 7};
  report.underlay.rgba.assign(9 * 7 * 4, 255);
  report.overlay.rgba.assign(9 * 7 * 4, 0);
  auto overlay_i = std::size_t(3 * 9 + 2) * 4;
  report.overlay.rgba[overlay_i] = 255;
  report.overlay.rgba[overlay_i + 3] = 128;
  auto composed = b.port->compose_report(input, report);
  HDRSHOT_CHECK(composed.has_value());
  auto rp = composed.value()->read_region({0, 0, 9, 7});
  HDRSHOT_CHECK(rp.has_value());
  HDRSHOT_CHECK_NEAR(rp.value()[0], 1, 2e-6);
  HDRSHOT_CHECK_NEAR(rp.value()[std::size_t(2 * 9 + 1) * 4 + 2], 8, 1e-6);
  auto red = analysis_math::ui_rgb_to_linear_p3(0xff0000);
  HDRSHOT_CHECK_NEAR(rp.value()[overlay_i],
                     2 * (127. / 255) + red.x * (128. / 255), 2e-6);
  HDRSHOT_CHECK(rp.value()[overlay_i + 2] > 3.9);
  auto original = input.source->read_region({0, 0, 1, 1});
  HDRSHOT_CHECK(original.has_value());
  HDRSHOT_CHECK_NEAR(original.value()[2], 8, 0);
  std::cout << "POC Source peak=" << pixels.value()[2]
            << " report peak=" << rp.value()[std::size_t(2 * 9 + 1) * 4 + 2]
            << " overlaid B=" << rp.value()[overlay_i + 2] << '\n';
}
void operation_overlay_blends_once_and_preserves_hdr() {
  Input input{source({4, 3}, FloatImage(12, {2, 4, 8, 1})), 1, true};
  SourceView view;
  view.target_size = {4, 3};
  auto marks = std::make_shared<UiImage>();
  marks->size = view.target_size;
  marks->rgba.assign(4 * 3 * 4, 0);
  // Deliberately nonzero invisible RGB: alpha, not RGB, determines coverage.
  marks->rgba[0] = 255;
  marks->rgba[4] = 255;
  marks->rgba[7] = 128;
  marks->rgba[8] = 255;
  marks->rgba[11] = 255;
  view.operation_overlay = marks;
  auto result = macos_analysis_render_offscreen(input, view);
  HDRSHOT_CHECK(result.has_value());
  auto pixels = result.value()->read_region({0, 0, 4, 3});
  HDRSHOT_CHECK(pixels.has_value());
  const auto native_red = analysis_math::ui_rgb_to_linear_p3(0xff0000);
  const std::array<double, 3> red{native_red.x, native_red.y, native_red.z};
  for (std::size_t c = 0; c < 3; ++c) {
    const double ground = std::array<double, 3>{2, 4, 8}[c];
    HDRSHOT_CHECK_NEAR(pixels.value()[c], ground, 1e-6);
    HDRSHOT_CHECK_NEAR(pixels.value()[4 + c],
                       ground * (127. / 255.) + red[c] * (128. / 255.), 2e-6);
    HDRSHOT_CHECK_NEAR(pixels.value()[8 + c], red[c], 2e-6);
  }
  auto retained = input.source->read_region({0, 0, 1, 1});
  HDRSHOT_CHECK(retained.has_value() && retained.value()[2] == 8);
  // A malformed UI bitmap must fail before dispatch, not read off its end.
  marks->rgba.pop_back();
  HDRSHOT_CHECK(!macos_analysis_render_offscreen(input, view).has_value());
  marks->rgba.push_back(0);
  marks->size = {1, 1};
  HDRSHOT_CHECK(!macos_analysis_render_offscreen(input, view).has_value());
}
void positioned_roi_clean() {
  auto b = backend();
  FloatImage values(8 * 6);
  for (std::size_t i = 0; i < values.size(); ++i)
    values[i] = {float(i), float(i) / 2, float(i) / 4, 1};
  auto native = source({8, 6}, values);
  SelectionRoiView roi;
  roi.size_px = {3, 2};
  roi.source_rect_px = {2, 3, 3, 2};
  roi.encoding = {ColorPrimaries::display_p3, TransferFunction::linear,
                  AlphaMode::opaque, 0};
  roi.linear_source = native;
  roi.row_stride_samples = 32;
  roi.first_sample_offset = (3 * 8 + 2) * 4;
  AnnotationPixelPlan plan;
  plan.output_size_px = roi.size_px;
  plan.source_visible_spans = {{0, 0, 1}, {0, 2, 1}, {1, 0, 3}};
  AnnotationOwnedSpan span;
  span.y = 0;
  span.x = 1;
  span.length = 1;
  span.edge_samples = {{.1f, .2f, .3f, .5f}};
  plan.annotation_owned_spans = {span};
  auto clean = b.port->prepare(roi, plan);
  HDRSHOT_CHECK(clean.has_value());
  HDRSHOT_CHECK(clean.value()->byte_count() == 3 * 2 * 16);
  auto data = clean.value()->read_region({0, 0, 3, 2});
  HDRSHOT_CHECK(data.has_value());
  HDRSHOT_CHECK_NEAR(data.value()[0], 26, 0);
  HDRSHOT_CHECK_NEAR(data.value()[4], 13.6, 1e-6);
  HDRSHOT_CHECK_NEAR(data.value()[12], 34, 0);
  Request request;
  request.scopes.waveform_visible = false;
  request.scopes.vector_visible = false;
  request.scopes.histogram_visible = false;
  auto result = b.port->analyze({clean.value(), 2, true}, request);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value()->valid_count == 6);
}
void report_geometry_and_failure_boundaries() {
  auto b = backend();
  PixelSize size{9, 7};
  FloatImage values(63);
  for (std::size_t i = 0; i < values.size(); ++i)
    values[i] = {float(i % 9) / 2, float(i / 9) / 3, float(i % 5) / 4, 1};
  auto cpu_source = make_cpu_analysis_source(size, values);
  HDRSHOT_CHECK(cpu_source.has_value());
  Input gpu_input{source(size, values), 21, true};
  Input cpu_input{cpu_source.value(), 21, true};
  auto cpu = make_cpu_analysis_port();
  double maximum_error = 0;
  for (int space = 0; space < 4; ++space)
    for (bool false_color : {false, true})
      for (double scale : {.7, 2.3}) {
        ReportPlan plan;
        plan.underlay.size = plan.overlay.size = {17, 13};
        plan.underlay.rgba.resize(17 * 13 * 4);
        plan.overlay.rgba.resize(17 * 13 * 4);
        for (std::size_t i = 0; i < 17 * 13; ++i) {
          for (std::size_t c = 0; c < 3; ++c) {
            plan.underlay.rgba[i * 4 + c] =
                std::uint8_t((i * 13 + c * 61) % 256);
            plan.overlay.rgba[i * 4 + c] =
                std::uint8_t((i * 17 + c * 53) % 256);
          }
          plan.underlay.rgba[i * 4 + 3] = 201;
          plan.overlay.rgba[i * 4 + 3] = std::uint8_t((i * 19) % 256);
        }
        plan.source_rect = {-2, 2, 16, 12};
        plan.source_view.target_size = {16, 12};
        plan.source_view.offset_x = -1.25;
        plan.source_view.offset_y = 1.75;
        plan.source_view.scale = scale;
        plan.source_view.false_color = false_color;
        plan.source_view.settings = {WorkingSpace(space), 203, 1};
        plan.source_view.mask = {true, MaskShape::ellipse, {.3, 1.1, 5.7, 4.3}};
        plan.ui_white_edr = .8;
        auto expected = cpu->compose_report(cpu_input, plan);
        auto actual = b.port->compose_report(gpu_input, plan);
        HDRSHOT_CHECK(expected.has_value() && actual.has_value());
        auto ep = expected.value()->read_region({0, 0, 17, 13});
        auto ap = actual.value()->read_region({0, 0, 17, 13});
        HDRSHOT_CHECK(ep.has_value() && ap.has_value());
        for (std::size_t i = 0; i < ep.value().size(); ++i) {
          double delta = std::abs(double(ep.value()[i]) - ap.value()[i]);
          maximum_error = std::max(maximum_error, delta);
          HDRSHOT_CHECK(delta < 4e-6);
        }
        plan.source_view.offset_y = std::numeric_limits<double>::infinity();
        HDRSHOT_CHECK(!b.port->compose_report(gpu_input, plan));
        HDRSHOT_CHECK(!cpu->compose_report(cpu_input, plan));
      }
  Request invalid;
  invalid.scopes.histogram_bins = max_histogram_bins + 1;
  HDRSHOT_CHECK(!b.port->analyze(gpu_input, invalid));
  SourceView invalid_view;
  invalid_view.target_size = {2, 2};
  invalid_view.scale = std::numeric_limits<double>::denorm_min();
  HDRSHOT_CHECK(!macos_analysis_render_offscreen(gpu_input, invalid_view));
  std::cout << "16 report geometry cases (clipping, fractional pan/zoom, Mask, "
               "SDR/HDR, FC, straight alpha): max float error="
            << maximum_error << '\n';
}
void window_resource_lifetimes() {
  const auto before = macos_gpu_device().currentAllocatedSize;
  std::weak_ptr<const LinearSource> weak_a, weak_b;
  @autoreleasepool {
    auto a = backend(), b = backend();
    FloatImage values(512 * 256, {2, 1, .5, 1});
    Input ia{source({512, 256}, values), 1, true};
    Input ib{source({512, 256}, values), 2, true};
    weak_a = ia.source;
    weak_b = ib.source;
    Request r;
    auto ar = a.port->analyze(ia, r), br = b.port->analyze(ib, r);
    HDRSHOT_CHECK(ar.has_value() && br.has_value());
    HDRSHOT_CHECK(ar.value()->valid_count == 512 * 256);
    a = {};
    ia = {};
    HDRSHOT_CHECK(weak_a.expired());
    HDRSHOT_CHECK(!weak_b.expired());
    HDRSHOT_CHECK(b.port->analyze(ib, r).has_value());
  }
  HDRSHOT_CHECK(weak_b.expired());
  const auto after = macos_gpu_device().currentAllocatedSize;
  std::cout
      << "two-window release: both immutable S owners expired; device before="
      << before << " after=" << after << '\n';
  // Runtime pipelines may remain; complete image textures must not.
  HDRSHOT_CHECK(after <= before + 1024 * 1024);
}
FloatImage fixture(PixelSize size) {
  FloatImage values;
  for (int y = 0; y < size.height; ++y)
    for (int x = 0; x < size.width; ++x) {
      float a = float((x * 13 + y * 31) % 101) / 17.3f,
            b = float((x * 29 + y * 7) % 109) / 31.7f,
            c = float((x * 17 + y * 19) % 113) / 51.1f;
      values.push_back({a, b, c, 1});
    }
  values[0] = {-.2f, 2.7f, .31f, 1};
  values[1] = {100, 120, 110, 1};
  return values;
}
void gpu_matches_cpu_full_statistics() {
  auto b = backend();
  PixelSize size{17, 13};
  auto values = fixture(size);
  values[4][0] = std::numeric_limits<float>::quiet_NaN();
  values[8][1] = std::numeric_limits<float>::infinity();
  Input input{source(size, values), 9, true};
  double max_mean_error = 0;
  std::uint64_t comparisons = 0;
  for (int space = 0; space < 4; ++space)
    for (double sigma : {0., .5, 1., 2., 4., 8.}) {
      Request request;
      request.revision = ++comparisons;
      request.settings = {WorkingSpace(space), space % 2 ? 100. : 203., sigma};
      request.scopes.wave_width = 17;
      request.scopes.wave_height = 64;
      // Odd dimensions put neutral at a bucket center. A separate test below
      // records the deliberately boundary-adjacent P3 sample at a 40-row grid.
      request.scopes.vector_width = 49;
      request.scopes.vector_height = 41;
      request.scopes.histogram_bins = 64;
      request.scopes.wave_mode = WaveMode::parade_intensity_rgb;
      request.scopes.histogram_mode = HistogramMode::rgb;
      request.mask = {true, MaskShape::ellipse, {1, 1, 14, 10}};
      request.samples = {{0, 5, 5, 5, true}, {1, 0, 0, 3, false}};
      auto work = prepare_work(size, values, request.settings);
      HDRSHOT_CHECK(work.has_value());
      auto cpu = analyze_cpu(size, values, work.value(), request);
      HDRSHOT_CHECK(cpu.has_value());
      auto gpu = b.port->analyze(input, request);
      HDRSHOT_CHECK(gpu.has_value());
      HDRSHOT_CHECK(gpu.value()->valid_count == cpu.value().valid_count);
      HDRSHOT_CHECK(gpu.value()->invalid_count == cpu.value().invalid_count);
      HDRSHOT_CHECK(gpu.value()->hue_count == cpu.value().hue_count);
      for (std::size_t c = 0; c < 3; ++c) {
        double err = std::abs(gpu.value()->mask_mean.work_rgb_edr[c] -
                              cpu.value().mask_mean.work_rgb_edr[c]);
        max_mean_error = std::max(max_mean_error, err);
        HDRSHOT_CHECK(err < 3e-5);
      }
      for (std::size_t c = 0; c < 4; ++c) {
        HDRSHOT_CHECK(gpu.value()->histograms[c].counts ==
                      cpu.value().histograms[c].counts);
        HDRSHOT_CHECK(gpu.value()->waveform[c].counts ==
                      cpu.value().waveform[c].counts);
      }
      if (gpu.value()->vectorscope.counts != cpu.value().vectorscope.counts) {
        std::cerr << "vector mismatch space=" << space << " sigma=" << sigma
                  << '\n';
        for (std::size_t j = 0; j < cpu.value().vectorscope.counts.size(); ++j)
          if (gpu.value()->vectorscope.counts[j] !=
              cpu.value().vectorscope.counts[j])
            std::cerr << "bin " << j
                      << " gpu=" << gpu.value()->vectorscope.counts[j]
                      << " cpu=" << cpu.value().vectorscope.counts[j] << '\n';
        for (int yy = 0; yy < size.height; ++yy)
          for (int xx = 0; xx < size.width; ++xx) {
            if (!contains(request.mask, xx + .5, yy + .5))
              continue;
            auto raw = work.value()[std::size_t(yy) * std::size_t(size.width) +
                                    std::size_t(xx)];
            auto point = project_sample({raw[0], raw[1], raw[2]}, 0,
                                        request.settings, request.scopes);
            auto projected =
                project_vector(point.vector, cpu.value().vector_calibration,
                               request.scopes.vector_zoom);
            if (std::floor(projected[0] * 48) == 18 &&
                std::abs(projected[1] * 40 - 19) < .001)
              std::cerr << "boundary xy=" << xx << ',' << yy
                        << " rgb=" << raw[0] << ',' << raw[1] << ',' << raw[2]
                        << " plane=" << point.vector[0] << ','
                        << point.vector[1] << " bin_y=" << std::setprecision(12)
                        << projected[1] * 40 << '\n';
          }
      }
      HDRSHOT_CHECK(gpu.value()->vectorscope.counts ==
                    cpu.value().vectorscope.counts);
      HDRSHOT_CHECK(gpu.value()->samples[0].mean.valid_count ==
                    cpu.value().samples[0].mean.valid_count);
    }
  std::cout << "CPU/Metal exact grid matrices=" << comparisons
            << " max work mean error=" << max_mean_error << '\n';
}
void mask_swatches_bins_and_invalid() {
  auto b = backend();
  PixelSize size{64, 32};
  auto values = fixture(size);
  values[2][0] = std::numeric_limits<float>::quiet_NaN();
  Input input{source(size, values), 1, true};
  Request r;
  r.scopes.wave_width = 32;
  r.scopes.wave_height = 32;
  r.scopes.vector_width = 32;
  r.scopes.vector_height = 32;
  r.scopes.histogram_bins = 128;
  r.scopes.histogram_mode = HistogramMode::hue;
  r.settings.blur_sigma_px = 1;
  r.samples = {{1, 20, 12, 11, false}};
  auto a = b.port->analyze(input, r);
  HDRSHOT_CHECK(a.has_value());
  HDRSHOT_CHECK(a.value()->invalid_count == 1);
  auto sum = [](const Histogram &h) {
    return std::accumulate(h.counts.begin(), h.counts.end(), std::uint64_t{});
  };
  HDRSHOT_CHECK(sum(a.value()->histograms[0]) == a.value()->hue_count);
  r.scopes.histogram_view = {3, .15};
  r.revision = 2;
  auto pan = b.port->analyze(input, r);
  HDRSHOT_CHECK(pan.has_value());
  HDRSHOT_CHECK(a.value()->histograms[0].counts ==
                pan.value()->histograms[0].counts);
  HDRSHOT_CHECK(pan.value()->statistics_ms == 0);
  r.mask = {true, MaskShape::rectangle, {0, 0, 4, 4}};
  auto masked = b.port->analyze(input, r);
  HDRSHOT_CHECK(masked.has_value());
  HDRSHOT_CHECK(masked.value()->valid_count == 15);
  HDRSHOT_CHECK(masked.value()->invalid_count == 1);
  HDRSHOT_CHECK(masked.value()->samples[0].mean.work_rgb_edr ==
                a.value()->samples[0].mean.work_rgb_edr);
  r.mask.bounds = {0, 0, 0, 0};
  auto empty = b.port->analyze(input, r);
  HDRSHOT_CHECK(empty.has_value());
  HDRSHOT_CHECK(empty.value()->valid_count == 0);
  HDRSHOT_CHECK(empty.value()->mask_mean.valid_count == 0);
  HDRSHOT_CHECK(sum(empty.value()->histograms[0]) == 0);
}
void full_domain_cache_and_independent_scope_refinement() {
  auto b = backend();
  PixelSize size{37, 23};
  auto values = fixture(size);
  values[0] = {0, 100, 0, 1};
  Input input{source(size, values), 1, true};
  Request r;
  r.scopes.wave_mode = WaveMode::parade_intensity_rgb;
  r.scopes.wave_width = 128;
  r.scopes.wave_height = 64;
  r.scopes.vector_width = r.scopes.vector_height = 64;
  r.scopes.histogram_bins = 128;
  auto a = b.port->analyze(input, r);
  HDRSHOT_CHECK(a.has_value());
  auto total = [](const auto &counts) {
    return std::accumulate(counts.begin(), counts.end(), std::uint64_t{});
  };
  HDRSHOT_CHECK(total(a.value()->vectorscope.counts) == values.size());
  HDRSHOT_CHECK(a.value()->vector_grid_y_extent >= .5);
  for (auto &grid : a.value()->waveform) {
    HDRSHOT_CHECK(grid.width == std::uint32_t(size.width));
    HDRSHOT_CHECK(total(grid.counts) == values.size());
    for (std::uint32_t x = 0; x < grid.width; ++x) {
      std::uint64_t column = 0;
      for (std::uint32_t y = 0; y < grid.height; ++y)
        column += grid.counts[y * grid.width + x];
      HDRSHOT_CHECK(column == std::uint32_t(size.height));
    }
  }
  r.revision = 2;
  r.scopes.amplitude_view = {64, .5};
  r.scopes.vector_zoom = 128;
  r.samples = {{0, 4, 5, 3, true}};
  auto pan = b.port->analyze(input, r);
  HDRSHOT_CHECK(pan.has_value());
  HDRSHOT_CHECK(pan.value()->statistics_ms == 0);
  HDRSHOT_CHECK(pan.value()->vectorscope.counts.shares_storage_with(
      a.value()->vectorscope.counts));
  HDRSHOT_CHECK(pan.value()->waveform[0].counts.shares_storage_with(
      a.value()->waveform[0].counts));
  r.scopes.histogram_bins *= 2;
  auto refined = b.port->analyze(input, r);
  HDRSHOT_CHECK(refined.has_value());
  HDRSHOT_CHECK(refined.value()->vectorscope.counts.shares_storage_with(
      pan.value()->vectorscope.counts));
  HDRSHOT_CHECK(refined.value()->waveform[0].counts.shares_storage_with(
      pan.value()->waveform[0].counts));
  HDRSHOT_CHECK(!refined.value()->histograms[0].counts.shares_storage_with(
      pan.value()->histograms[0].counts));
  HDRSHOT_CHECK(total(refined.value()->histograms[0].counts) == values.size());
  r.mask = {true, MaskShape::rectangle, {1, 1, 2, 2}};
  auto masked = b.port->analyze(input, r);
  HDRSHOT_CHECK(masked.has_value());
  HDRSHOT_CHECK(total(masked.value()->vectorscope.counts) == 4);
  HDRSHOT_CHECK(masked.value()->vector_grid_y_extent ==
                a.value()->vector_grid_y_extent);
  HDRSHOT_CHECK(masked.value()->vector_grid_x_extent ==
                a.value()->vector_grid_x_extent);
}
void resources_and_latency() {
  auto b = backend();
  PixelSize size{512, 288};
  auto values = fixture(size);
  Input input{source(size, values), 1, true};
  Request r;
  r.scopes.wave_width = 128;
  r.scopes.wave_height = 128;
  r.scopes.vector_width = 128;
  r.scopes.vector_height = 128;
  auto a = b.port->analyze(input, r);
  HDRSHOT_CHECK(a.has_value());
  r.samples = {{0, 100, 100, 101, true}};
  auto sampled = b.port->analyze(input, r);
  HDRSHOT_CHECK(sampled.has_value());
  HDRSHOT_CHECK(sampled.value()->valid_count == 512 * 288);
  HDRSHOT_CHECK(sampled.value()->samples[0].mean.valid_count == 10201);
  std::cout << "512x288 work=" << a.value()->prepare_ms
            << "ms stats=" << a.value()->statistics_ms
            << "ms sample101=" << sampled.value()->sampling_ms
            << "ms retained=" << sampled.value()->retained_bytes
            << " peak=" << sampled.value()->peak_bytes << '\n';
}
void deep_zoom_refines_from_cached_fp32_projections() {
  auto b = backend();
  const PixelSize size{7, 1024};
  FloatImage values(std::size_t(size.width * size.height));
  for (int y = 0; y < size.height; ++y) {
    const float signal = .5f + (float(y) + .375f) / float(size.height) / 64.f;
    const float linear = analysis_math::srgb_decode(signal);
    for (int x = 0; x < size.width; ++x)
      values[std::size_t(y * size.width + x)] = {linear, linear, linear, 1};
  }
  Input input{source(size, values), 1, false};
  Request r;
  r.settings.working_space = WorkingSpace::display_p3_sdr;
  r.scopes.wave_width = 7;
  r.scopes.wave_height = 64;
  r.scopes.vector_width = r.scopes.vector_height = 65;
  r.scopes.wave_mode = WaveMode::parade_intensity_rgb;
  auto base = b.port->analyze(input, r);
  HDRSHOT_CHECK(base.has_value());
  r.scopes.amplitude_view = {64, .5};
  r.scopes.vector_zoom = 64;
  r.scopes.wave_detail_width = 7;
  r.scopes.wave_detail_height = 256;
  r.scopes.vector_detail_width = r.scopes.vector_detail_height = 65;
  auto detail = b.port->analyze(input, r);
  HDRSHOT_CHECK(detail.has_value());
  HDRSHOT_CHECK(detail.value()->statistics_ms == 0);
  HDRSHOT_CHECK(detail.value()->prepare_ms == 0);
  HDRSHOT_CHECK(detail.value()->detail_ms > 0);
  HDRSHOT_CHECK(detail.value()->waveform[0].counts.shares_storage_with(
      base.value()->waveform[0].counts));
  auto work = prepare_work(size, values, r.settings);
  HDRSHOT_CHECK(work.has_value());
  auto cpu = analyze_cpu(size, values, work.value(), r);
  HDRSHOT_CHECK(cpu.has_value());
  for (std::size_t c = 0; c < 4; ++c)
    HDRSHOT_CHECK(detail.value()->waveform_detail[c].counts ==
                  cpu.value().waveform_detail[c].counts);
  HDRSHOT_CHECK(detail.value()->vectorscope_detail.counts ==
                cpu.value().vectorscope_detail.counts);
  std::size_t occupied_rows = 0;
  const auto &grid = detail.value()->waveform_detail[0];
  for (std::uint32_t y = 0; y < grid.height; ++y)
    if (grid.counts[std::size_t(y) * grid.width])
      ++occupied_rows;
  HDRSHOT_CHECK(occupied_rows >
                240); // full cache has only one row in this narrow interval
  r.scopes.amplitude_view.pan += .001;
  auto panned = b.port->analyze(input, r);
  HDRSHOT_CHECK(panned.has_value());
  HDRSHOT_CHECK(panned.value()->statistics_ms == 0 &&
                panned.value()->prepare_ms == 0);
  HDRSHOT_CHECK(panned.value()->detail_ms > 0);
  HDRSHOT_CHECK(panned.value()->vectorscope_detail.counts.shares_storage_with(
      detail.value()->vectorscope_detail.counts));
  HDRSHOT_CHECK(panned.value()->vectorscope.counts.shares_storage_with(
      base.value()->vectorscope.counts));
  auto again = b.port->analyze(input, r);
  HDRSHOT_CHECK(again.has_value());
  HDRSHOT_CHECK(again.value()->detail_ms == 0);
  HDRSHOT_CHECK(again.value()->waveform_detail[0].counts.shares_storage_with(
      panned.value()->waveform_detail[0].counts));
  const auto expected_projection = values.size() * 24;
  const auto measured_projection = detail.value()->retained_bytes -
                                   input.source->byte_count() * 2 -
                                   result_bytes(*detail.value());
  HDRSHOT_CHECK(measured_projection == expected_projection);
  std::cout << "deep zoom lazy projection bytes=" << measured_projection
            << " detail_ms=" << detail.value()->detail_ms
            << " cached_rebin_ms=" << panned.value()->detail_ms
            << " occupied_rows=" << occupied_rows << '\n';
}
void vector_detail_uses_real_viewport_without_repreparing_pixels() {
  auto b = backend();
  const PixelSize size{32, 32};
  FloatImage values;
  // Fixed colors away from bin boundaries: the separate boundary-precision
  // test records CPU/Metal FP32 differences rather than demanding bit equality
  // of arbitrary random samples exactly next to a quantization edge.
  const std::array<std::array<float, 4>, 4> palette{{
      {.18f, .18f, .18f, 1}, {.182f, .186f, .174f, 1},
      {.1f, .28f, .09f, 1}, {.22f, .11f, .3f, 1}}};
  for (unsigned n = 0; n < 1024; ++n)
    values.push_back(palette[n % palette.size()]);
  Input input{source(size, values), 1, true};
  for (auto space : {WorkingSpace::display_p3_pq, WorkingSpace::bt2020_pq,
                      WorkingSpace::srgb_sdr, WorkingSpace::display_p3_sdr})
    for (auto mode : {VectorMode::perceptual, VectorMode::ycbcr}) {
      Request r;
      r.settings.working_space = space;
      r.scopes.waveform_visible = r.scopes.histogram_visible = false;
      r.scopes.vector_width = r.scopes.vector_height = 65;
      r.scopes.vector_detail_width = 65;
      r.scopes.vector_detail_height = 49;
      r.scopes.vector_mode = mode;
      r.scopes.vector_zoom = 20.;
      const double pan_unit = mode == VectorMode::ycbcr || is_hdr(space) ? .017 : 6.;
      auto work = prepare_work(size, values, r.settings);
      HDRSHOT_CHECK(work.has_value());
      analysis::ResultRef previous;
      for (auto viewport : {std::array<unsigned, 2>{3712, 344}, {4312, 344},
                             {344, 3712}, {344, 4312}, {4312, 2744}}) {
        r.scopes.vector_viewport_width = viewport[0];
        r.scopes.vector_viewport_height = viewport[1];
        r.scopes.vector_pan = {pan_unit, -pan_unit};
        auto gpu = b.port->analyze(input, r);
        auto cpu = analyze_cpu(size, values, work.value(), r);
        HDRSHOT_CHECK(gpu.has_value() && cpu.has_value());
        if (gpu.value()->vectorscope_detail.counts != cpu.value().vectorscope_detail.counts) {
          const auto &a = gpu.value()->vectorscope_detail.counts;
          const auto &c = cpu.value().vectorscope_detail.counts;
          unsigned mismatches = 0;
          for (std::size_t i = 0; i < a.size(); ++i) mismatches += a[i] != c[i];
          std::cout << "detail grid mismatch space=" << unsigned(space)
                    << " mode=" << unsigned(mode) << " viewport=" << viewport[0] << 'x' << viewport[1]
                    << " bins=" << mismatches << '\n';
        }
        HDRSHOT_CHECK(gpu.value()->vectorscope_detail.counts == cpu.value().vectorscope_detail.counts);
        HDRSHOT_CHECK_NEAR(gpu.value()->vector_detail_x_extent,
                           cpu.value().vector_detail_x_extent, 1e-12);
        HDRSHOT_CHECK_NEAR(gpu.value()->vector_detail_y_extent,
                           cpu.value().vector_detail_y_extent, 1e-12);
        HDRSHOT_CHECK(gpu.value()->vector_detail_center == r.scopes.vector_pan);
        if (previous) {
          HDRSHOT_CHECK(gpu.value()->prepare_ms == 0 && gpu.value()->statistics_ms == 0);
          HDRSHOT_CHECK(gpu.value()->vectorscope.counts.shares_storage_with(previous->vectorscope.counts));
          HDRSHOT_CHECK(!gpu.value()->vectorscope_detail.counts.shares_storage_with(previous->vectorscope_detail.counts));
        }
        previous = gpu.value();
        r.scopes.vector_pan = {-pan_unit, pan_unit};
        auto moved = b.port->analyze(input, r);
        auto moved_cpu = analyze_cpu(size, values, work.value(), r);
        HDRSHOT_CHECK(moved.has_value() && moved_cpu.has_value());
        HDRSHOT_CHECK(moved.value()->vectorscope_detail.counts == moved_cpu.value().vectorscope_detail.counts);
        HDRSHOT_CHECK(moved.value()->vector_detail_center == r.scopes.vector_pan);
        HDRSHOT_CHECK(moved.value()->prepare_ms == 0 && moved.value()->statistics_ms == 0);
        HDRSHOT_CHECK(moved.value()->vectorscope.counts.shares_storage_with(previous->vectorscope.counts));
        HDRSHOT_CHECK(!moved.value()->vectorscope_detail.counts.shares_storage_with(previous->vectorscope_detail.counts));
        previous = moved.value();
      }
    }
}
void boundary_precision_evidence() {
  auto b = backend();
  FloatImage pixels{{5.f / 17.3f, 26.f / 31.7f, 2.f / 51.1f, 1}};
  Request r;
  r.scopes.waveform_visible = false;
  r.scopes.histogram_visible = false;
  r.scopes.vector_width = 48;
  r.scopes.vector_height = 40;
  auto result = b.port->analyze({source({1, 1}, pixels), 1, true}, r);
  HDRSHOT_CHECK(result.has_value());
  // Independent binary64 normative reference is P=-.025003376537322153.
  // CPU FP32/PQ evaluates -.024999866; Metal evaluates the neighboring low bin.
  // This explicitly records a numerical boundary, not a missing sample.
  auto raw = analysis_math::signals_from_work(
      {pixels[0][0], pixels[0][1], pixels[0][2]}, 2, 203);
  HDRSHOT_CHECK_NEAR(raw.perceptual.z, -.025003376537322153, 1e-5);
  HDRSHOT_CHECK(result.value()->valid_count == 1);
  auto sum = std::accumulate(result.value()->vectorscope.counts.begin(),
                             result.value()->vectorscope.counts.end(),
                             std::uint64_t{});
  HDRSHOT_CHECK(sum == 1);
  const auto &grid = result.value()->vectorscope;
  const double px =
      .5 + raw.perceptual.y / (2 * result.value()->vector_grid_x_extent);
  const double py =
      .5 + (-.025003376537322153) / (2 * result.value()->vector_grid_y_extent);
  const auto col = std::uint32_t(std::floor(px * grid.width));
  const auto row = std::uint32_t(std::floor(py * grid.height));
  HDRSHOT_CHECK(std::abs(px * grid.width - std::round(px * grid.width)) > .001);
  HDRSHOT_CHECK(row == 18);
  HDRSHOT_CHECK(grid.counts[std::size_t(row) * grid.width + col] == 1);
  std::cout << "Boundary evidence: double P=-0.0250033765373 CPU FP32 P="
            << std::setprecision(12) << raw.perceptual.z
            << " Metal vector row=18, total=1\n";
}
struct ProcessMemorySnapshot {
  std::uint64_t resident{}, physical_footprint{}, metal_allocated{};
};
ProcessMemorySnapshot process_memory_snapshot() {
  task_vm_info_data_t info{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  HDRSHOT_CHECK(task_info(mach_task_self(), TASK_VM_INFO,
                          reinterpret_cast<task_info_t>(&info),
                          &count) == KERN_SUCCESS);
  return {info.resident_size, info.phys_footprint,
          macos_gpu_device().currentAllocatedSize};
}
void print_memory(const char *label, PixelSize size) {
  const auto memory = process_memory_snapshot();
  std::cout << "BENCH memory " << size.width << 'x' << size.height << ' '
            << label << " resident=" << memory.resident
            << " physical_footprint=" << memory.physical_footprint
            << " metal_allocated=" << memory.metal_allocated << '\n'
            << std::flush;
}
void large_benchmark(bool retina, bool detail = false) {
  auto configure = [retina](Request &r) {
    if (retina) {
      r.scopes.wave_width = 1280;
      r.scopes.wave_height = 256;
      r.scopes.vector_width = 1400;
      r.scopes.vector_height = 300;
    }
  };
  std::cout
      << "BENCH device=" << macos_gpu_device().name.UTF8String << " grids="
      << (retina ? "wave1280x256_vector1400x300" : "wave512x256_vector384x384")
      << " detail=" << detail
      << " retained/peak=CPU+GPU payload estimate; device_delta=Metal "
         "allocation snapshot; RSS/footprint and Metal overlap on unified "
         "memory and must not be added\n";
  // Warm pipelines before taking the baseline. Pipeline/runtime allocation is
  // process-global, not an individual analysis window's retained image data.
  {
    auto warmed = backend();
  }
  for (auto size : {PixelSize{5120, 2880}, PixelSize{6016, 3384}}) {
    std::weak_ptr<const LinearSource> source_lifetime;
    print_memory("before_window", size);
    @autoreleasepool {
      auto b = backend();
      auto before = macos_gpu_device().currentAllocatedSize;
      auto values = fixture(size);
      Input input{source(size, values), 1, true};
      source_lifetime = input.source;
      HDRSHOT_CHECK(input.source->wait_until_ready().has_value());
      FloatImage{}.swap(values);
      Request request;
      configure(request);
      request.scopes.wave_mode = WaveMode::parade_intensity_rgb;
      auto run = [&](const char *label) {
        ++request.revision;
        auto started = std::chrono::steady_clock::now();
        auto result = b.port->analyze(input, request);
        HDRSHOT_CHECK(result.has_value());
        double wall = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - started)
                          .count();
        std::cout << "BENCH " << size.width << 'x' << size.height << ' '
                  << label << " wall_ms=" << wall
                  << " prepare_ms=" << result.value()->prepare_ms
                  << " statistics_ms=" << result.value()->statistics_ms
                  << " sampling_ms=" << result.value()->sampling_ms
                  << " prepare_gpu_ms=" << result.value()->prepare_gpu_ms
                  << " statistics_gpu_ms=" << result.value()->statistics_gpu_ms
                  << " sampling_gpu_ms=" << result.value()->sampling_gpu_ms
                  << " detail_ms=" << result.value()->detail_ms
                  << " detail_gpu_ms=" << result.value()->detail_gpu_ms
                  << " retained=" << result.value()->retained_bytes
                  << " projected_bytes="
                  << result.value()->retained_bytes -
                         input.source->byte_count() * 2 -
                         result_bytes(*result.value())
                  << " peak=" << result.value()->peak_bytes << " device_delta="
                  << macos_gpu_device().currentAllocatedSize - before
                  << " valid=" << result.value()->valid_count
                  << " invalid=" << result.value()->invalid_count
                  << " base_wave_count="
                  << std::accumulate(result.value()->waveform[0].counts.begin(),
                                     result.value()->waveform[0].counts.end(),
                                     std::uint64_t{})
                  << " detail_wave_count="
                  << std::accumulate(
                         result.value()->waveform_detail[0].counts.begin(),
                         result.value()->waveform_detail[0].counts.end(),
                         std::uint64_t{})
                  << " base_vector_count="
                  << std::accumulate(result.value()->vectorscope.counts.begin(),
                                     result.value()->vectorscope.counts.end(),
                                     std::uint64_t{})
                  << " detail_vector_count="
                  << std::accumulate(
                         result.value()->vectorscope_detail.counts.begin(),
                         result.value()->vectorscope_detail.counts.end(),
                         std::uint64_t{})
                  << '\n'
                  << std::flush;
        print_memory(label, size);
      };
      run("initial");
      if (detail) {
        run("base_cache_warm");
        request.scopes.amplitude_view = {64, .5};
        request.scopes.vector_zoom = 64;
        request.scopes.wave_detail_width = 512;
        request.scopes.wave_detail_height = 256;
        request.scopes.vector_detail_width = 384;
        request.scopes.vector_detail_height = 384;
        run("detail64_lazy_projection");
        run("detail64_warm");
        request.scopes.amplitude_view.pan = .505;
        run("detail64_wave_pan");
        request.scopes.vector_zoom = 65;
        run("detail65_vector_zoom");
        request.samples = {{0, size.width / 2, size.height / 2, 101, true}};
        run("detail65_hover101");
        // Exit this scope before the next memory snapshot. All source, W,
        // projection and statistics ownership belonging to this window dies.
      } else {
        request.mask = {true,
                        MaskShape::ellipse,
                        {size.width * .1, size.height * .1, size.width * .8,
                         size.height * .8}};
        run("mask");
        request.scopes.amplitude_view = {2, .2};
        run("wave_pan_zoom");
        request.scopes.vector_zoom = 2;
        run("vector_zoom");
        request.scopes.histogram_view = {2, .2};
        run("hist_pan");
        request.samples = {{0, size.width / 2, size.height / 2, 1, true}};
        run("hover1");
        request.samples[0].side = 101;
        run("hover101");
        request.settings.working_space = WorkingSpace::bt2020_pq;
        run("working_space");
        request.settings.blur_sigma_px = 1;
        run("blur1");
        request.settings.blur_sigma_px = 8;
        run("blur8");
      }
    }
    HDRSHOT_CHECK(source_lifetime.expired());
    // The queue is idle because analyze() waits for each own GPU command.
    // Driver cache allocations may remain; report the actual snapshot rather
    // than promising RSS falls by exactly the estimated payload size.
    print_memory("after_window_release", size);
  }
  if (detail)
    return;
  @autoreleasepool {
    auto b = backend();
    PixelSize size{6016, 3384};
    FloatImage values(std::size_t(size.width) * std::size_t(size.height),
                      {.5, .5, .5, 1});
    Input input{source(size, values), 1, true};
    HDRSHOT_CHECK(input.source->wait_until_ready().has_value());
    FloatImage{}.swap(values);
    Request r;
    configure(r);
    auto result = b.port->analyze(input, r);
    HDRSHOT_CHECK(result.has_value());
    auto &grid = result.value()->vectorscope;
    auto index =
        std::size_t(std::max_element(grid.counts.begin(), grid.counts.end()) -
                    grid.counts.begin());
    HDRSHOT_CHECK(grid.counts[index] ==
                  std::uint32_t(size.width * size.height));
    double actual = double(grid.color_sums[index][0]) / grid.counts[index];
    double expected =
        display_srgb({.5, .5, .5}, WorkingSpace::display_p3_pq)[0];
    HDRSHOT_CHECK_NEAR(actual, expected, 2e-5);
    std::cout << "BENCH 6016x3384 concentrated_gray stats_ms="
              << result.value()->statistics_ms << " color_average=" << actual
              << " expected=" << expected << " count=" << grid.counts[index]
              << '\n';
  }
}
} // namespace
int main(int argc, char **argv) {
  @autoreleasepool {
    if (!macos_gpu_device()) {
      std::cerr << "Metal device unavailable\n";
      return 77;
    }
    if (argc == 2 && (std::string_view(argv[1]) == "--benchmark" ||
                      std::string_view(argv[1]) == "--benchmark-retina" ||
                      std::string_view(argv[1]) == "--benchmark-detail"))
      return hdrshot::test::run({{"5K/6K full-pixel benchmarks", [argv] {
                                    large_benchmark(std::string_view(argv[1]) ==
                                                        "--benchmark-retina",
                                                    std::string_view(argv[1]) ==
                                                        "--benchmark-detail");
                                  }}});
    return hdrshot::test::run(
        {{"native Source/report HDR float POC", source_and_report_float_poc},
         {"report geometry and invalid requests",
          report_geometry_and_failure_boundaries},
         {"independent window source/cache release", window_resource_lifetimes},
         {"independent ROI and clean annotation", positioned_roi_clean},
         {"transparent operation overlay, linear blend and HDR preservation",
          operation_overlay_blends_once_and_preserves_hdr},
         {"all-pixel CPU/Metal exact statistics",
          gpu_matches_cpu_full_statistics},
         {"mask, fixed samples, stable bins, invalid input",
          mask_swatches_bins_and_invalid},
         {"full-domain cache and independent scope refinement",
          full_domain_cache_and_independent_scope_refinement},
         {"64x detail from retained FP32 projections",
          deep_zoom_refines_from_cached_fp32_projections},
         {"real vector viewport domain with cached projections",
          vector_detail_uses_real_viewport_without_repreparing_pixels},
         {"FP32 boundary precision evidence", boundary_precision_evidence},
         {"bounded resource and latency fixture", resources_and_latency}});
  }
}
