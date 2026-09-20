#include "application/clean_content_cache.hpp"
#include "application/export_workflow.hpp"
#include "domain/annotation/annotation_compositing.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "domain/frame/frame_pipeline.hpp"
#include "domain/output/output_row_producer.hpp"
#include "domain/output/source_range_probe.hpp"
#include "domain/output/ultra_hdr_input_renderer.hpp"
#include "platform/macos/macos_gpu_source.hpp"
#include "platform/macos/macos_linear_storage.hpp"
#include "platform/macos/macos_metal_export_pixel_processor.hpp"
#include "platform/macos/metal_edr_presenter.hpp"
#include "test_support.hpp"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <memory>
#include <vector>

namespace {
using namespace hdrshot;
constexpr DisplayId kDisplay{1};
constexpr PixelSize kSize{11, 7};
constexpr PixelRect kFull{0, 0, 11, 7};

std::shared_ptr<MacMetalExportPixelProcessor> processor() {
  auto result = MacMetalExportPixelProcessor::create();
  HDRSHOT_CHECK(result.has_value());
  return std::shared_ptr<MacMetalExportPixelProcessor>(std::move(result.value()));
}

std::unique_ptr<MacMetalEdrPresenter> presenter() {
  auto result = MacMetalEdrPresenter::create();
  HDRSHOT_CHECK(result.has_value());
  return std::move(result.value());
}

std::vector<std::uint16_t> source_pixels(PixelSize size = kSize) {
  // Distinct channels and rows expose stride/offset errors. Both signs, the
  // sRGB toe, SDR white, and HDR occur without relying on display headroom.
  constexpr std::array<std::uint16_t, 13> codes{
      0x0000, 0x8000, 0x0001, 0x8001, 0x2800, 0x3400, 0x3800,
      0x3bff, 0x3c00, 0x3c01, 0x4000, 0xb800, 0x4200};
  std::vector<std::uint16_t> result;
  result.reserve(static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height) * 4U);
  for (int y = 0; y < size.height; ++y) for (int x = 0; x < size.width; ++x) {
    const auto i = static_cast<std::size_t>(y * size.width + x);
    result.insert(result.end(), {codes[i % codes.size()], codes[(i * 5 + 3) % codes.size()],
        codes[(i * 7 + 6) % codes.size()], static_cast<std::uint16_t>(i % 2 ? 0x7e00 : 0xfc00)});
  }
  return result;
}

FrozenDesktopRef interpret(std::vector<std::uint16_t> pixels, bool native,
    PixelSize size = kSize) {
  NativeCaptureFrame frame{kDisplay, size, PixelFormat::rgba16_float,
      {ColorPrimaries::display_p3, TransferFunction::extended_srgb, AlphaMode::opaque, 0},
      std::move(pixels), {}, {}};
  if (native) frame.source_normalizer = macos_source_normalizer();
  else frame.linear_storage = macos_linear_storage(); // Retained T21 CPU reference.
  auto segment = SourceColorInterpreter::interpret(std::move(frame),
      {kDisplay, {0, 0, static_cast<double>(size.width), static_cast<double>(size.height)},
          1, size, DisplayDynamicRange::hdr});
  HDRSHOT_CHECK(segment.has_value());
  HDRSHOT_CHECK(segment.value().software_linearization_passes == 1);
  HDRSHOT_CHECK(segment.value().rgba_half.empty());
  HDRSHOT_CHECK(segment.value().pixel_format == PixelFormat::rgba32_float);
  HDRSHOT_CHECK(segment.value().encoding.transfer == TransferFunction::linear);
  HDRSHOT_CHECK(segment.value().encoding.alpha == AlphaMode::opaque);
  HDRSHOT_CHECK(static_cast<bool>(segment.value().linear_source) == native);
  HDRSHOT_CHECK(segment.value().rgba_float.empty() == native);
  FrozenDesktop desktop{FrameId{42}, 3,
      {0, 0, static_cast<double>(size.width), static_cast<double>(size.height)}, {}};
  desktop.canonical_segments.push_back(std::move(segment.value()));
  return std::make_shared<const FrozenDesktop>(std::move(desktop));
}

SelectionRoiView roi(const FrozenDesktopRef& source, PixelRect rect = kFull) {
  auto result = FrameCropper::view_display(*source, kDisplay, {7, rect});
  HDRSHOT_CHECK(result.has_value());
  return std::move(result.value());
}

AnnotationRenderPlan empty_render(PixelRect rect = kFull) {
  return {1, {rect.width, rect.height}, {}, rect};
}

AnnotationPixelPlan empty_pixels(PixelRect rect = kFull) {
  auto result = AnnotationRenderPlanner::build_pixel_plan(empty_render(rect));
  HDRSHOT_CHECK(result.has_value());
  return std::move(result.value());
}

AnnotationRenderPlan aa_render() {
  // Display-local coverage starts at x=2. Cropping at x=3 must increment the
  // native sample offset, including spans after the first row's samples.
  return {4, kSize, {{ObjectId{8}, AnnotationKind::text, {2, 2, 6, 2}, 0x3aee91,
      {{2, 2, {1, 64, 128, 192, 254, 255}}, {3, 2, {255, 192, 128, 64, 1, 255}}},
      "native-source-AA"}}, kFull};
}

MacMetalOverlayRequest preview_request(const FrozenDesktopRef& source,
    CleanContentRef clean = {}) {
  const auto& segment = source->canonical_segments.front();
  MacMetalOverlayRequest request;
  request.width_px = static_cast<std::size_t>(segment.size_px.width);
  request.height_px = static_cast<std::size_t>(segment.size_px.height);
  request.selection_px = {2, 1, segment.size_px.width - 3, segment.size_px.height - 2};
  request.outside_linear_dim_factor = 0.35F;
  request.ui_white_edr = 2.03F;
  request.ui_border_width_px = 1;
  request.source_frame_key = source->frame_id.value;
  request.source_is_linear = true;
  request.clean_content = std::move(clean);
  request.linear_source = segment.linear_source;
  if (!segment.rgba_float.empty()) request.rgba_float_linear_p3 =
      std::shared_ptr<const LinearFloatPixels>(source, &segment.rgba_float);
  return request;
}

void check_float_bits(std::span<const float> actual, std::span<const float> expected) {
  HDRSHOT_CHECK(actual.size() == expected.size());
  for (std::size_t i = 0; i < actual.size(); ++i) {
    HDRSHOT_CHECK(std::isfinite(actual[i]) && std::isfinite(expected[i]));
    if (std::bit_cast<std::uint32_t>(actual[i]) != std::bit_cast<std::uint32_t>(expected[i])) {
      std::cerr << "FP32 mismatch at sample " << i << '\n';
      HDRSHOT_CHECK(false);
    }
  }
}

void report_jpeg_half_difference(std::string_view label, PixelRect rect,
    std::span<const std::uint16_t> actual, std::span<const std::uint16_t> expected,
    const SelectionRoiView& cpu_source) {
  HDRSHOT_CHECK(actual.size() == expected.size());
  std::size_t differences = 0, maximum_index = 0;
  int maximum_code_delta = 0;
  double maximum_linear_delta = 0;
  for (std::size_t i = 0; i < actual.size(); ++i) {
    if (actual[i] == expected[i]) continue;
    ++differences;
    const auto a = ExtendedP3Mapper::decode_binary16(actual[i]);
    const auto b = ExtendedP3Mapper::decode_binary16(expected[i]);
    HDRSHOT_CHECK(a && b);
    const int delta = std::abs(int(actual[i]) - int(expected[i]));
    if (delta > maximum_code_delta) { maximum_code_delta = delta; maximum_index = i; }
    maximum_linear_delta = std::max(maximum_linear_delta, std::abs(double(a.value()) - double(b.value())));
    if (differences <= 6) {
      const auto x = (i / 4) % static_cast<std::size_t>(rect.width);
      const auto y = (i / 4) / static_cast<std::size_t>(rect.width);
      const auto source_value = cpu_source.sample(cpu_source.first_sample_offset +
          y * cpu_source.row_stride_samples + x * 4 + i % 4);
      HDRSHOT_CHECK(source_value.has_value());
      std::cerr << "[JPEG HALF] " << label << " ROI=" << rect.x << ',' << rect.y << ','
          << rect.width << ',' << rect.height << " sample=" << i << " local=" << x << ',' << y
          << " channel=" << i % 4 << " actual_bits=0x" << std::hex << actual[i]
          << " expected_bits=0x" << expected[i] << " source_FP32_bits=0x"
          << std::bit_cast<std::uint32_t>(source_value.value()) << std::dec
          << std::setprecision(10) << " actual=" << a.value() << " expected=" << b.value()
          << " source_FP32=" << source_value.value() << '\n';
    }
  }
  std::cerr << "[JPEG HALF SUMMARY] " << label << " ROI=" << rect.x << ',' << rect.y << ','
      << rect.width << ',' << rect.height << " differences=" << differences
      << " max_code_delta=" << maximum_code_delta << " max_code_index=" << maximum_index
      << " max_linear_delta=" << std::setprecision(10) << maximum_linear_delta << '\n';
}

bool jpeg_half_parity(PixelRect rect, const LinearDisplayP3HalfImage& native_gpu,
    const LinearDisplayP3HalfImage& cpu_reference, const SelectionRoiView& t21_cpu_source,
    const AnnotationPixelPlan& cpu_plan, MacMetalExportPixelProcessor& metal) {
  const auto cpu_source_gpu = metal.render({&t21_cpu_source, &cpu_plan, kUltraHdrReferenceWhiteNits});
  HDRSHOT_CHECK(cpu_source_gpu.has_value());
  HDRSHOT_CHECK(native_gpu.rgba_half.size() == cpu_reference.rgba_half.size());
  std::size_t signed_zero_differences = 0;
  bool reference_matches = true;
  for (std::size_t i = 0; i < native_gpu.rgba_half.size(); ++i) {
    const auto actual = native_gpu.rgba_half[i];
    const auto expected = cpu_reference.rgba_half[i];
    if (actual == expected) continue;
    // CPU std::clamp preserves -0; the existing Metal max(value, 0) emits +0.
    // These are the only distinct bit patterns allowed by this CPU comparison.
    if ((actual & 0x7fffU) == 0 && (expected & 0x7fffU) == 0) ++signed_zero_differences;
    else reference_matches = false;
  }
  // T22 must remain bit-for-bit equivalent to the production T21 GPU path,
  // even where the CPU oracle has a numerically equivalent signed zero.
  const bool t21_matches = native_gpu.rgba_half == cpu_source_gpu.value().rgba_half;
  if (!reference_matches || !t21_matches) {
    report_jpeg_half_difference("native GPU vs CPU reference", rect,
        native_gpu.rgba_half, cpu_reference.rgba_half, t21_cpu_source);
    report_jpeg_half_difference("T21 CPU-source GPU vs CPU reference", rect,
        cpu_source_gpu.value().rgba_half, cpu_reference.rgba_half, t21_cpu_source);
    report_jpeg_half_difference("native GPU vs T21 CPU-source GPU", rect,
        native_gpu.rgba_half, cpu_source_gpu.value().rgba_half, t21_cpu_source);
  } else if (signed_zero_differences) {
    std::cout << "JPEG ROI=" << rect.x << ',' << rect.y << ',' << rect.width << ',' << rect.height
        << " CPU signed-zero aliases=" << signed_zero_differences
        << "; native GPU equals T21 GPU bit-for-bit\n";
  }
  return reference_matches && t21_matches;
}

void check_clean_plan(const AnnotationPixelPlan& native, const AnnotationPixelPlan& cpu) {
  auto materialized = materialize_annotation_plan(native);
  HDRSHOT_CHECK(materialized.has_value());
  const auto& actual = materialized.value();
  HDRSHOT_CHECK(actual.output_size_px == cpu.output_size_px);
  HDRSHOT_CHECK(actual.source_visible_spans == cpu.source_visible_spans);
  HDRSHOT_CHECK(actual.annotation_owned_spans.size() == cpu.annotation_owned_spans.size());
  for (std::size_t i = 0; i < actual.annotation_owned_spans.size(); ++i) {
    const auto& a = actual.annotation_owned_spans[i];
    const auto& b = cpu.annotation_owned_spans[i];
    HDRSHOT_CHECK(a.x == b.x && a.y == b.y && a.length == b.length && a.object_id == b.object_id);
    HDRSHOT_CHECK(a.clean_composited && !a.native_samples && a.native_sample_offset == 0);
    HDRSHOT_CHECK(a.edge_samples.size() == b.edge_samples.size());
    for (std::size_t p = 0; p < a.edge_samples.size(); ++p) {
      HDRSHOT_CHECK(a.edge_samples[p][3] == 0.0F);
      for (std::size_t c = 0; c < 3; ++c) {
        HDRSHOT_CHECK(std::isfinite(a.edge_samples[p][c]));
        HDRSHOT_CHECK_NEAR(a.edge_samples[p][c], b.edge_samples[p][c], 2e-6);
      }
    }
  }
}

OutputPlan output_plan(bool hdr) {
  return {hdr ? OutputClass::hdr : OutputClass::wide_gamut_sdr,
      hdr ? EncodingIntent::hdr_pq : EncodingIntent::wide_gamut_sdr, 16, "native-test"};
}

void first_draw_consumes_new_native_source_before_cpu_readback() {
  // This test is intentionally first in main. Every new source's first
  // consumer is render_offscreen: no normalization wait/readback/warm draw.
  for (bool fused : {true, false}) {
    auto native_presenter = presenter();
    auto cpu_presenter = presenter();
    const auto input = source_pixels();
    const auto native = interpret(input, true);
    const auto actual = native_presenter->render_offscreen(preview_request(native), fused);
    HDRSHOT_CHECK(actual.has_value());
    const auto cpu = interpret(input, false);
    const auto expected = cpu_presenter->render_offscreen(preview_request(cpu), fused);
    HDRSHOT_CHECK(expected.has_value());
    HDRSHOT_CHECK(actual.value().rgba_linear_display_p3 == expected.value().rgba_linear_display_p3);
    HDRSHOT_CHECK(actual.value().texture_format == "MTLPixelFormatRGBA16Float");
    HDRSHOT_CHECK(actual.value().layer_color_space == "kCGColorSpaceExtendedLinearDisplayP3");
    HDRSHOT_CHECK(!actual.value().source_aliases_cpu);
    HDRSHOT_CHECK(native->canonical_segments.front().rgba_float.empty());
  }
}

void all_65536_half_codes_preserve_finite_inverse_float_bits() {
  constexpr PixelSize size{256, 256};
  std::vector<std::uint16_t> input;
  input.reserve(65536U * 4U);
  std::size_t finite_count = 0, negative_count = 0;
  for (std::uint32_t code = 0; code < 65536U; ++code) {
    const bool finite = (code & 0x7c00U) != 0x7c00U;
    const auto bits = static_cast<std::uint16_t>(finite ? code : 0U);
    finite_count += finite;
    negative_count += finite && (code & 0x8000U) != 0;
    // Nonfinite RGB must fail separately; every raw code is also exercised as
    // ignored alpha, which must become exact 1.0 even for NaN/Inf inputs.
    input.insert(input.end(), {bits, bits, bits, static_cast<std::uint16_t>(code)});
  }
  HDRSHOT_CHECK(finite_count == 63488 && negative_count == 31744);
  const auto native = interpret(input, true, size);
  const auto cpu = interpret(input, false, size);
  auto native_view = roi(native, {0, 0, size.width, size.height});
  HDRSHOT_CHECK(native_view.rgba_half.empty() && native_view.rgba_float.empty());
  HDRSHOT_CHECK(native_view.sample_data(0) == nullptr);
  HDRSHOT_CHECK(!native_view.sample(0));
  auto actual = FrameCropper::read_cpu_region(native_view);
  HDRSHOT_CHECK(actual.has_value());
  check_float_bits(actual.value().rgba_float, cpu->canonical_segments.front().rgba_float);
  for (std::uint32_t code = 0; code < 65536U; ++code) {
    if ((code & 0x7c00U) != 0x7c00U) {
      const auto decoded = ExtendedP3Mapper::decode_binary16(static_cast<std::uint16_t>(code));
      HDRSHOT_CHECK(decoded.has_value());
      HDRSHOT_CHECK(std::bit_cast<std::uint32_t>(actual.value().rgba_float[code * 4U]) ==
          std::bit_cast<std::uint32_t>(ExtendedP3Mapper::inverse_extended_srgb(decoded.value())));
    }
    HDRSHOT_CHECK(actual.value().rgba_float[code * 4U + 3U] == 1.0F);
  }
  HDRSHOT_CHECK(native->canonical_segments.front().rgba_float.empty());
  auto texture = macos_source_texture(native_view.linear_source);
  HDRSHOT_CHECK(texture && texture.pixelFormat == MTLPixelFormatRGBA32Float);
  HDRSHOT_CHECK(texture.storageMode == MTLStorageModePrivate);
  HDRSHOT_CHECK(native_view.linear_source->byte_count() == 65536U * 16U);
}

void nonzero_odd_single_pixel_and_boundary_rois_read_exactly() {
  const auto input = source_pixels();
  const auto native = interpret(input, true);
  const auto cpu = interpret(input, false);
  auto metal = processor();
  bool jpeg_matches = true;
  for (PixelRect rect : {PixelRect{3, 2, 5, 3}, PixelRect{10, 6, 1, 1},
           PixelRect{0, 0, 1, 1}, PixelRect{6, 3, 5, 4}, kFull}) {
    const auto source = roi(native, rect);
    HDRSHOT_CHECK(source.first_sample_offset == static_cast<std::size_t>((rect.y * kSize.width + rect.x) * 4));
    HDRSHOT_CHECK(source.row_stride_samples == static_cast<std::size_t>(kSize.width * 4));
    auto actual = FrameCropper::read_cpu_region(source);
    auto expected = FrameCropper::crop_display(*cpu, kDisplay, {7, rect});
    HDRSHOT_CHECK(actual && expected);
    HDRSHOT_CHECK(actual.value().source_rect_px == rect);
    HDRSHOT_CHECK(actual.value().size_px == (PixelSize{rect.width, rect.height}));
    HDRSHOT_CHECK(actual.value().rgba_float.size() == static_cast<std::size_t>(rect.width * rect.height * 4));
    check_float_bits(actual.value().rgba_float, expected.value().rgba_float);
    const auto plan = empty_pixels(rect);
    const auto gpu = metal->render({&source, &plan, kUltraHdrReferenceWhiteNits});
    const auto cpu_view = FrameCropper::view(actual.value());
    CpuUltraHdrInputRenderer renderer;
    const auto reference = renderer.render({&cpu_view, &plan, kUltraHdrReferenceWhiteNits});
    HDRSHOT_CHECK(gpu && reference);
    const auto t21_cpu_view = FrameCropper::view(expected.value());
    jpeg_matches = jpeg_half_parity(rect, gpu.value(), reference.value(), t21_cpu_view, plan, *metal)
        && jpeg_matches;
  }
  HDRSHOT_CHECK(jpeg_matches);
}

void native_clean_first_preview_roi_offsets_and_immutable_revisions() {
  auto native_presenter = presenter();
  auto cpu_presenter = presenter();
  auto metal = processor();
  const auto input = source_pixels();
  const auto native = interpret(input, true);
  auto render = aa_render();
  CleanContentCache native_cache(metal);
  auto clean = native_cache.get(native, kDisplay, render);
  HDRSHOT_CHECK(clean.has_value());
  for (const auto& span : clean.value()->owned) {
    HDRSHOT_CHECK(span.native_samples && span.edge_samples.empty() && span.clean_composited);
  }
  // No read_samples/materialize/wait before this first native clean preview.
  const auto shown = native_presenter->render_offscreen(preview_request(native, clean.value()), true);
  HDRSHOT_CHECK(shown.has_value());
  const auto cpu = interpret(input, false);
  CleanContentCache cpu_cache;
  const auto cpu_clean = cpu_cache.get(cpu, kDisplay, render);
  HDRSHOT_CHECK(cpu_clean.has_value());
  const auto reference = cpu_presenter->render_offscreen(preview_request(cpu, cpu_clean.value()), true);
  HDRSHOT_CHECK(reference.has_value());
  HDRSHOT_CHECK(shown.value().rgba_linear_display_p3 == reference.value().rgba_linear_display_p3);
  HDRSHOT_CHECK(native_cache.counters().composed_pixels == 12);
  HDRSHOT_CHECK(native_cache.counters().retained_sample_bytes == 12 * 16);

  const PixelRect clipped{3, 2, 3, 2};
  const auto partial = clean.value()->roi_plan(clipped, 8);
  const auto cpu_partial = cpu_clean.value()->roi_plan(clipped, 8);
  HDRSHOT_CHECK(partial && cpu_partial);
  check_clean_plan(partial.value(), cpu_partial.value());
  HDRSHOT_CHECK(partial.value().annotation_owned_spans.size() == 2);
  for (const auto& span : partial.value().annotation_owned_spans) {
    HDRSHOT_CHECK(span.x == 0 && span.length == 3);
    const auto original = std::find_if(clean.value()->owned.begin(), clean.value()->owned.end(),
        [&](const auto& value) { return value.y == span.y + clipped.y; });
    HDRSHOT_CHECK(original != clean.value()->owned.end());
    HDRSHOT_CHECK(span.native_samples == original->native_samples);
    HDRSHOT_CHECK(span.native_sample_offset == original->native_sample_offset + 1);
  }
  const auto old_pixels = materialize_annotation_plan(clean.value()->roi_plan(kFull, 4).value());
  HDRSHOT_CHECK(old_pixels.has_value());
  const auto old_storage = clean.value()->owned.front().native_samples;
  auto selection = AnnotationRenderPlanner::rebase(render, {1, 1, 8, 4});
  HDRSHOT_CHECK(selection.has_value());
  selection.value().source_document_revision = 100; // UI selection only.
  const auto reused = native_cache.get(native, kDisplay, selection.value());
  HDRSHOT_CHECK(reused && reused.value() == clean.value());
  HDRSHOT_CHECK(native_cache.counters().content_builds == 1);
  HDRSHOT_CHECK(native_cache.counters().composed_pixels == 12);
  HDRSHOT_CHECK(native_cache.counters().cache_hits == 1);
  render.ordered_layers.front().color_srgb_rgb = 0xff0000;
  const auto changed = native_cache.get(native, kDisplay, render);
  HDRSHOT_CHECK(changed && changed.value() != clean.value());
  HDRSHOT_CHECK(changed.value()->owned.front().native_samples != old_storage);
  HDRSHOT_CHECK(changed.value()->content_revision == 2);
  const auto old_after = materialize_annotation_plan(clean.value()->roi_plan(kFull, 4).value());
  HDRSHOT_CHECK(old_after && old_after.value() == old_pixels.value());
  const auto new_pixels = materialize_annotation_plan(changed.value()->roi_plan(kFull, 4).value());
  HDRSHOT_CHECK(new_pixels && new_pixels.value() != old_pixels.value());
}

void png_all_precisions_and_whites_and_jpeg_match_cpu_reference() {
  auto metal = processor();
  const auto native = interpret(source_pixels(), true);
  const auto cpu = interpret(source_pixels(), false);
  CleanContentCache native_cache(metal), cpu_cache;
  const auto native_clean = native_cache.get(native, kDisplay, aa_render());
  const auto cpu_clean = cpu_cache.get(cpu, kDisplay, aa_render());
  HDRSHOT_CHECK(native_clean && cpu_clean);
  bool jpeg_matches = true;
  // Includes a clipped native sample span, an odd-width row and a 1x1 corner.
  for (PixelRect rect : {PixelRect{3, 1, 5, 5}, PixelRect{10, 6, 1, 1}}) {
    const auto source = roi(native, rect);
    const auto gpu_plan = native_clean.value()->roi_plan(rect, 4);
    const auto cpu_plan = cpu_clean.value()->roi_plan(rect, 4);
    auto cpu_region = FrameCropper::read_cpu_region(source);
    HDRSHOT_CHECK(gpu_plan && cpu_plan && cpu_region);
    const auto cpu_view = FrameCropper::view(cpu_region.value());
    check_clean_plan(gpu_plan.value(), cpu_plan.value());
    for (bool hdr : {false, true})
      for (auto precision : {HdrPqPrecision::bits_10, HdrPqPrecision::bits_12, HdrPqPrecision::bits_16})
        for (auto white : {PqDiffuseWhite::nits_100, PqDiffuseWhite::nits_203}) {
          const auto output = output_plan(hdr);
          const auto expected = OutputRowProducer::produce_all(cpu_view, cpu_plan.value(), output,
              pq_diffuse_white_nits(white), precision);
          const auto actual = metal->process({&source, &gpu_plan.value(), output, white, precision});
          HDRSHOT_CHECK(expected && actual);
          HDRSHOT_CHECK(actual.value().rgb_u16.size() == expected.value().rgb_u16.size());
          // Existing T21 tolerance: at most one effective PQ step, or 2 RGB16
          // codes for 16-bit pow evaluation; SDR has only a one-code rounding gate.
          const int limit = !hdr ? 1 : precision == HdrPqPrecision::bits_10 ? 65
              : precision == HdrPqPrecision::bits_12 ? 17 : 2;
          for (std::size_t i = 0; i < expected.value().rgb_u16.size(); ++i)
            HDRSHOT_CHECK(std::abs(int(actual.value().rgb_u16[i]) - int(expected.value().rgb_u16[i])) <= limit);
          HDRSHOT_CHECK(actual.value().content_light.has_value() == hdr);
          HDRSHOT_CHECK(actual.value().luminance_clip.clipped_pixel_count == expected.value().clipped_pixel_count);
          HDRSHOT_CHECK(actual.value().luminance_clip.clipped_channel_count == expected.value().clipped_channel_count);
          if (hdr) {
            HDRSHOT_CHECK_NEAR(actual.value().content_light->max_cll_x10000,
                expected.value().content_light.max_cll_x10000, 16);
            HDRSHOT_CHECK(actual.value().content_light->pixel_count == expected.value().content_light.pixel_count);
            HDRSHOT_CHECK_NEAR(static_cast<double>(actual.value().content_light->luminance_sum_x10000),
                static_cast<double>(expected.value().content_light.luminance_sum_x10000),
                16.0 * static_cast<double>(expected.value().content_light.pixel_count));
          }
        }
    CpuUltraHdrInputRenderer renderer;
    const auto expected = renderer.render({&cpu_view, &cpu_plan.value(), kUltraHdrReferenceWhiteNits});
    const auto actual = metal->render({&source, &gpu_plan.value(), kUltraHdrReferenceWhiteNits});
    HDRSHOT_CHECK(expected && actual);
    const auto t21_cpu_view = roi(cpu, rect);
    jpeg_matches = jpeg_half_parity(rect, actual.value(), expected.value(), t21_cpu_view, cpu_plan.value(), *metal)
        && jpeg_matches;
    HDRSHOT_CHECK(jpeg_output_kind(actual.value()) == jpeg_output_kind(expected.value()));
    HDRSHOT_CHECK_NEAR(actual.value().maximum_linear_component, expected.value().maximum_linear_component, 2e-6);
    HDRSHOT_CHECK(actual.value().source_visible_maximum_linear_component.has_value());
    HDRSHOT_CHECK(expected.value().source_visible_maximum_linear_component.has_value());
    HDRSHOT_CHECK_NEAR(*actual.value().source_visible_maximum_linear_component,
        *expected.value().source_visible_maximum_linear_component, 2e-6);
  }
  HDRSHOT_CHECK(jpeg_matches);
}

void range_ignores_hdr_outside_roi_and_annotation_owned_aa() {
  auto metal = processor();
  const PixelRect selection{2, 2, 7, 3};
  for (int scenario = 0; scenario < 4; ++scenario) {
    std::vector<std::uint16_t> input(static_cast<std::size_t>(kSize.width * kSize.height * 4), 0x3800);
    // 0: all SDR, 1: HDR outside ROI, 2: HDR only beneath AA ownership,
    // 3: one additional genuinely source-visible HDR pixel inside ROI.
    const auto high = [&](int x, int y) {
      for (int c = 0; c < 3; ++c) input[static_cast<std::size_t>((y * kSize.width + x) * 4 + c)] = 0x4000;
    };
    if (scenario >= 1) high(10, 6);
    if (scenario >= 2) high(3, 2);
    if (scenario == 3) high(8, 4);
    const auto source = interpret(input, true);
    CleanContentCache cache(metal);
    const auto clean = cache.get(source, kDisplay, aa_render());
    HDRSHOT_CHECK(clean.has_value());
    const auto native_plan = clean.value()->roi_plan(selection, 4);
    const auto view = roi(source, selection);
    HDRSHOT_CHECK(native_plan.has_value());
    const auto actual = metal->probe(view, native_plan.value(), {});
    auto cpu_region = FrameCropper::read_cpu_region(view);
    auto cpu_plan = materialize_annotation_plan(native_plan.value());
    HDRSHOT_CHECK(actual && cpu_region && cpu_plan);
    const auto expected = SourceRangeProbe::probe(cpu_region.value(), cpu_plan.value());
    HDRSHOT_CHECK(expected.has_value());
    HDRSHOT_CHECK(actual.value().fits_sdr == (scenario != 3));
    HDRSHOT_CHECK(actual.value().fits_sdr == expected.value().fits_sdr);
    HDRSHOT_CHECK(actual.value().source_visible_pixel_count == expected.value().source_visible_pixel_count);
    HDRSHOT_CHECK(actual.value().skipped_annotation_pixel_count == 12);
    HDRSHOT_CHECK(actual.value().scanned_component_count == actual.value().source_visible_pixel_count * 3);
    const auto jpeg = metal->render({&view, &native_plan.value(), kUltraHdrReferenceWhiteNits});
    HDRSHOT_CHECK(jpeg.has_value());
    HDRSHOT_CHECK(jpeg_output_kind(jpeg.value()) == (scenario == 3
        ? JpegOutputKind::ultra_hdr : JpegOutputKind::display_p3_sdr));
    if (scenario == 2) HDRSHOT_CHECK(jpeg.value().maximum_linear_component > 1.0);
    if (scenario == 0) {
      const auto fast = metal->probe(view, native_plan.value(), {true});
      HDRSHOT_CHECK(fast && fast.value().fits_sdr && fast.value().scanned_component_count == 0);
    }
  }
}

void cancel_releases_owners_and_detached_export_retains_old_resources() {
  std::weak_ptr<const LinearSource> cancelled;
  std::weak_ptr<const LinearSampleStorage> cancelled_samples;
  @autoreleasepool {
    auto source = interpret(source_pixels(), true);
    cancelled = source->canonical_segments.front().linear_source;
    source.reset(); // No CPU wait or first preview is required to cancel.
    HDRSHOT_CHECK(cancelled.expired());
  }
  @autoreleasepool {
    auto source = interpret(source_pixels(), true);
    cancelled = source->canonical_segments.front().linear_source;
    const std::array<PositionedCleanSample, 1> sample{{{{0.25F, 0.25F, 0.25F, 0.5F}, 1, 0}}};
    auto clean = macos_compose_clean(source->canonical_segments.front().linear_source, sample);
    HDRSHOT_CHECK(clean.has_value());
    cancelled_samples = clean.value();
    source.reset();
    HDRSHOT_CHECK(!cancelled.expired()); // Queued clean owner still needs the source.
  }
  HDRSHOT_CHECK(cancelled.expired() && cancelled_samples.expired());
  // Complete previously submitted GPU work after the C++ owner was cancelled.
  // This catches a command retaining raw pointers into a destroyed capture.
  @autoreleasepool {
    auto fence = [macos_gpu_queue() commandBuffer];
    HDRSHOT_CHECK(fence != nil);
    [fence commit]; [fence waitUntilCompleted];
    HDRSHOT_CHECK(fence.status == MTLCommandBufferStatusCompleted);
  }

  auto metal = processor();
  std::weak_ptr<const FrozenDesktop> frame_lifetime;
  std::weak_ptr<const LinearSource> source_lifetime;
  std::weak_ptr<const LinearSampleStorage> sample_lifetime;
  ExportSnapshot detached;
  AnnotationPixelPlan saved_cpu_plan;
  {
    auto source = interpret(source_pixels(), true);
    frame_lifetime = source;
    source_lifetime = source->canonical_segments.front().linear_source;
    CleanContentCache cache(metal);
    auto render = aa_render();
    const auto clean = cache.get(source, kDisplay, render);
    HDRSHOT_CHECK(clean.has_value());
    sample_lifetime = clean.value()->owned.front().native_samples;
    detached.frozen_desktop = source;
    detached.clean_content = clean.value();
    detached.target_display_id = kDisplay;
    detached.selection = {7, {3, 1, 5, 5}};
    saved_cpu_plan = materialize_annotation_plan(clean.value()->roi_plan(detached.selection.desktop_rect, 4).value()).value();
    render.ordered_layers.front().color_srgb_rgb = 0;
    HDRSHOT_CHECK(cache.get(source, kDisplay, render).has_value());
  }
  HDRSHOT_CHECK(!frame_lifetime.expired() && !source_lifetime.expired() && !sample_lifetime.expired());
  {
    const auto source = roi(detached.frozen_desktop, detached.selection.desktop_rect);
    const auto plan = detached.clean_content->roi_plan(detached.selection.desktop_rect, 4);
    HDRSHOT_CHECK(plan.has_value());
    const auto still_saved = materialize_annotation_plan(plan.value());
    HDRSHOT_CHECK(still_saved && still_saved.value() == saved_cpu_plan);
    const auto jpeg = metal->render({&source, &plan.value(), kUltraHdrReferenceWhiteNits});
    HDRSHOT_CHECK(jpeg.has_value());
  }
  detached = {};
  HDRSHOT_CHECK(frame_lifetime.expired() && source_lifetime.expired() && sample_lifetime.expired());
}

void invalid_rgb_propagates_from_normalization_to_consumers() {
  auto metal = processor();
  auto display = presenter();
  for (auto invalid : {std::uint16_t{0x7c00}, std::uint16_t{0xfc00}, std::uint16_t{0x7e01}}) {
    auto input = source_pixels();
    input[4] = invalid;
    const auto source = interpret(std::move(input), true);
    // A normalizer may return an enqueued resource; consumers must surface its
    // asynchronous RGB validation failure instead of publishing black pixels.
    const auto shown = display->render_offscreen(preview_request(source), true);
    HDRSHOT_CHECK(!shown && shown.error().code == ErrorCode::invalid_color_contract);
    const auto view = roi(source);
    const auto plan = empty_pixels();
    const auto ready = view.linear_source->wait_until_ready();
    HDRSHOT_CHECK(!ready && ready.error().code == ErrorCode::invalid_color_contract);
    HDRSHOT_CHECK(!FrameCropper::read_cpu_region(view));
    HDRSHOT_CHECK(!metal->process({&view, &plan, output_plan(true), PqDiffuseWhite::nits_203}));
    HDRSHOT_CHECK(!metal->render({&view, &plan, kUltraHdrReferenceWhiteNits}));
    HDRSHOT_CHECK(!metal->probe(view, plan, {}));
    const std::array<PositionedCleanSample, 1> sample{{{{0.25F, 0.25F, 0.25F, 0.5F}, 1, 0}}};
    const auto clean = macos_compose_clean(view.linear_source, sample);
    HDRSHOT_CHECK(!clean || !clean.value()->wait_until_ready());
  }
}

class DescriptorTestSource final : public LinearSource {
 public:
  DescriptorTestSource(LinearSourceRef backing, PixelSize size, std::size_t bytes)
      : backing_(std::move(backing)), size_(size), bytes_(bytes) {}
  PixelSize size_px() const override { return size_; }
  std::size_t byte_count() const override { return bytes_; }
  Result<bool, Error> wait_until_ready() const override { return backing_->wait_until_ready(); }
  Result<LinearFloatPixels, Error> read_region(PixelRect rect) const override {
    return backing_->read_region(rect);
  }
 private:
  LinearSourceRef backing_;
  PixelSize size_;
  std::size_t bytes_;
};

void invalid_bounds_and_native_sample_offsets_fail_without_publication() {
  const auto source = interpret(source_pixels(), true);
  const auto native = source->canonical_segments.front().linear_source;
  for (PixelRect rect : {PixelRect{-1, 0, 1, 1}, PixelRect{10, 6, 2, 1},
           PixelRect{0, 0, 0, 1}, PixelRect{0, 7, 1, 1}}) {
    HDRSHOT_CHECK(!native->read_region(rect));
    HDRSHOT_CHECK(!FrameCropper::view_display(*source, kDisplay, {1, rect}));
  }
  HDRSHOT_CHECK(!macos_source_normalizer()->normalize_extended_p3({0, 1}, {}));
  HDRSHOT_CHECK(!macos_source_normalizer()->normalize_extended_p3({2, 1}, {0, 0, 0, 0}));
  std::array<PositionedCleanSample, 1> sample{{{{0.25F, 0.25F, 0.25F, 0.5F}, 11, 0}}};
  HDRSHOT_CHECK(!macos_compose_clean(native, sample));
  sample[0].x = 1;
  sample[0].annotation[3] = 1.01F;
  HDRSHOT_CHECK(!macos_compose_clean(native, sample));
  sample[0].annotation[3] = 0.5F;
  sample[0].annotation[0] = std::numeric_limits<float>::quiet_NaN();
  HDRSHOT_CHECK(!macos_compose_clean(native, sample));
  sample[0].annotation[0] = 0.25F;
  auto samples = macos_compose_clean(native, sample);
  HDRSHOT_CHECK(samples.has_value());
  HDRSHOT_CHECK(!samples.value()->read_samples(1, 1));
  HDRSHOT_CHECK(!samples.value()->read_samples(std::numeric_limits<std::size_t>::max(), 1));

  auto metal = processor();
  CleanContentCache cache(metal);
  auto clean = cache.get(source, kDisplay, aa_render());
  HDRSHOT_CHECK(clean.has_value());
  auto plan = clean.value()->roi_plan(kFull, 4).value();
  auto& span = plan.annotation_owned_spans.front();
  span.native_sample_offset = span.native_samples->sample_count();
  HDRSHOT_CHECK(!materialize_annotation_plan(plan));
  const auto view = roi(source);
  HDRSHOT_CHECK(!metal->process({&view, &plan, output_plan(true), PqDiffuseWhite::nits_203}));
  HDRSHOT_CHECK(!metal->render({&view, &plan, kUltraHdrReferenceWhiteNits}));
  HDRSHOT_CHECK(!metal->probe(view, plan, {}));
  auto malformed = view;
  malformed.first_sample_offset = view.sample_count();
  const auto empty = empty_pixels();
  HDRSHOT_CHECK(!FrameCropper::read_cpu_region(malformed));
  HDRSHOT_CHECK(!metal->process({&malformed, &empty, output_plan(true), PqDiffuseWhite::nits_203}));
  HDRSHOT_CHECK(!metal->render({&malformed, &empty, kUltraHdrReferenceWhiteNits}));
  HDRSHOT_CHECK(!metal->probe(malformed, empty, {}));

  const auto reject_native_view = [&](SelectionRoiView candidate, std::string_view label) {
    if (candidate.valid_storage()) std::cerr << "Unexpected valid native descriptor: " << label << '\n';
    HDRSHOT_CHECK(!candidate.valid_storage());
    const auto pixels = empty_pixels({0, 0, candidate.size_px.width, candidate.size_px.height});
    HDRSHOT_CHECK(!FrameCropper::read_cpu_region(candidate));
    HDRSHOT_CHECK(!metal->process({&candidate, &pixels, output_plan(true), PqDiffuseWhite::nits_203}));
    HDRSHOT_CHECK(!metal->render({&candidate, &pixels, kUltraHdrReferenceWhiteNits}));
    HDRSHOT_CHECK(!metal->probe(candidate, pixels, {}));
  };
  for (auto size : {PixelSize{0, 7}, PixelSize{11, -1}, PixelSize{12, 7}}) {
    malformed = view;
    malformed.linear_source = std::make_shared<DescriptorTestSource>(native, size, native->byte_count());
    reject_native_view(malformed, "source dimensions disagree with byte descriptor");
  }
  for (auto bytes : {native->byte_count() - 4U, native->byte_count() + 16U}) {
    malformed = view;
    malformed.linear_source = std::make_shared<DescriptorTestSource>(native, kSize, bytes);
    reject_native_view(malformed, "source byte count must equal width * height * 16");
  }
  malformed = view;
  malformed.encoding.primaries = ColorPrimaries::srgb_bt709;
  reject_native_view(malformed, "native source must be P3");
  malformed = view;
  malformed.encoding.transfer = TransferFunction::extended_srgb;
  reject_native_view(malformed, "native source must already be linear");
  malformed = view;
  malformed.encoding.source_reference_white_nits = 203.0;
  reject_native_view(malformed, "native source must retain relative EDR units");
  malformed = view;
  malformed.row_stride_samples += 4;
  reject_native_view(malformed, "stride cannot disagree with source width");
  malformed = view;
  malformed.row_stride_samples += 1;
  reject_native_view(malformed, "stride must contain complete RGBA pixels");
  malformed = view;
  malformed.first_sample_offset += 1;
  reject_native_view(malformed, "offset must contain complete RGBA pixels");
  malformed = view;
  malformed.source_rect_px = {9, 0, 5, 1};
  malformed.size_px = {5, 1};
  malformed.first_sample_offset = 9 * 4;
  reject_native_view(malformed, "flattened in-bounds ROI cannot cross a source row");
  malformed = view;
  malformed.source_rect_px = {0, 6, 1, 2};
  malformed.size_px = {1, 2};
  malformed.first_sample_offset = 6 * 11 * 4;
  reject_native_view(malformed, "ROI cannot cross source bottom");

  auto display = presenter();
  const auto cpu = interpret(source_pixels(), false);
  const auto cpu_preview = preview_request(cpu);
  const auto encoded = std::make_shared<const std::vector<std::uint16_t>>(source_pixels());
  const auto reject_preview = [&](const MacMetalOverlayRequest& request, std::string_view label) {
    for (bool fused : {false, true}) {
      const auto rendered = display->render_offscreen(request, fused);
      if (rendered) std::cerr << "Unexpected accepted preview source: " << label << '\n';
      HDRSHOT_CHECK(!rendered && rendered.error().code == ErrorCode::invalid_input);
    }
  };
  auto mixed = preview_request(source);
  mixed.rgba_float_linear_p3 = cpu_preview.rgba_float_linear_p3;
  reject_preview(mixed, "native and valid CPU FP32 sources are mutually exclusive");
  mixed = preview_request(source);
  mixed.rgba_half_extended_p3 = encoded;
  reject_preview(mixed, "native and valid CPU FP16 sources are mutually exclusive");
  mixed = cpu_preview;
  mixed.rgba_half_extended_p3 = encoded;
  reject_preview(mixed, "two valid CPU source formats are mutually exclusive");
  mixed = cpu_preview;
  mixed.linear_source = std::make_shared<DescriptorTestSource>(native, PixelSize{10, 7}, native->byte_count());
  reject_preview(mixed, "invalid native source cannot be bypassed by valid CPU FP32");
  mixed.rgba_float_linear_p3.reset();
  mixed.rgba_half_extended_p3 = encoded;
  reject_preview(mixed, "invalid native source cannot be bypassed by valid CPU FP16");
  mixed = preview_request(source);
  mixed.source_is_linear = false;
  reject_preview(mixed, "native source cannot be tagged nonlinear");
}
} // namespace

int main() {
  @autoreleasepool {
    // Only absent hardware may skip. A shared runtime/shader initialization
    // failure must not be mislabeled as an unavailable physical device.
    if (MTLCreateSystemDefaultDevice() == nil) {
      std::cout << "[SKIP] Metal device unavailable: native GPU source cases were not run\n";
      return 77;
    }
    const auto availability = MacMetalExportPixelProcessor::create();
    if (!availability) {
      std::cerr << "Native GPU source preflight failed: " << to_string(availability.error().code) << '\n';
      for (const auto& [key, value] : availability.error().safe_context)
        std::cerr << key << '=' << value << '\n';
      return 1;
    }
    return test::run({
        {"first native source draw precedes CPU readback", first_draw_consumes_new_native_source_before_cpu_readback},
        {"all 65536 half codes preserve finite inverse FP32 bits", all_65536_half_codes_preserve_finite_inverse_float_bits},
        {"nonzero odd 1x1 and boundary native ROIs", nonzero_odd_single_pixel_and_boundary_rois_read_exactly},
        {"first clean preview sparse ROI offsets reuse and immutable revisions", native_clean_first_preview_roi_offsets_and_immutable_revisions},
        {"native PNG 10/12/16 100/203 and JPEG reference matrix", png_all_precisions_and_whites_and_jpeg_match_cpu_reference},
        {"native source-visible range outside ROI AA-only HDR and SDR", range_ignores_hdr_outside_roi_and_annotation_owned_aa},
        {"cancelled native owners and detached export lifetime", cancel_releases_owners_and_detached_export_retains_old_resources},
        {"invalid RGB normalization failures reach every native consumer", invalid_rgb_propagates_from_normalization_to_consumers},
        {"native bounds and sample offset validation", invalid_bounds_and_native_sample_offsets_fail_without_publication},
    });
  }
}
