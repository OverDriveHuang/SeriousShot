#include "platform/windows/windows_color.hpp"
#include "application/analysis_workflow.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "domain/color/capture_edr_boundary.hpp"
#include "platform/cpu/cpu_analysis_port.hpp"
#include "test_support.hpp"
#include <limits>
#include <memory>
#include <bit>
#include <cstring>

using namespace hdrshot;
namespace {
class SpyLinearSource final : public LinearSource {
 public:
  PixelSize size_px() const override { return {1, 1}; }
  std::size_t byte_count() const override { return 4U * sizeof(float); }
  Result<bool, Error> wait_until_ready() const override {
    ++wait_count;
    return Result<bool, Error>::success(true);
  }
  Result<LinearFloatPixels, Error> read_region(PixelRect) const override {
    ++read_count;
    LinearFloatPixels samples;
    samples.assign({1.0040311F, 1.0F, 1.0F, 1.0F});
    return Result<LinearFloatPixels, Error>::success(std::move(samples));
  }
  mutable int wait_count{};
  mutable int read_count{};
};
void white_units() {
  HDRSHOT_CHECK_NEAR(WindowsColor::scrgb_to_edr_scale({true, 80}).value(), 1, 1e-7);
  const auto scale = WindowsColor::scrgb_to_edr_scale({true, 203}).value();
  const auto p3 = WindowsColor::scrgb_to_linear_p3({2.5375F, 2.5375F, 2.5375F}, scale);
  for (auto v : p3) HDRSHOT_CHECK_NEAR(v, 1, 2e-7);
  HDRSHOT_CHECK_NEAR(WindowsColor::scrgb_to_edr_scale({false, 400}).value(), 1, 0);
  HDRSHOT_CHECK(!WindowsColor::scrgb_to_edr_scale({true, 0}));
  HDRSHOT_CHECK(!WindowsColor::scrgb_to_edr_scale({true, std::numeric_limits<double>::quiet_NaN()}));
}
void gamut_and_extended_range() {
  const auto r = WindowsColor::scrgb_to_linear_p3({1, 0, 0}, 1);
  HDRSHOT_CHECK_NEAR(r[0], 0.82246197, 1e-7);
  HDRSHOT_CHECK_NEAR(r[1], 0.03319420, 1e-7);
  HDRSHOT_CHECK_NEAR(r[2], 0.01708263, 1e-7);
  for (const auto p3 : {std::array<float, 3>{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {-0.25F, 2, 12}}) {
    const auto raw = WindowsColor::linear_p3_to_scrgb(p3, 2.5F);
    const auto roundtrip = WindowsColor::scrgb_to_linear_p3(raw, 0.4F);
    for (std::size_t c = 0; c < 3; ++c) HDRSHOT_CHECK_NEAR(roundtrip[c], p3[c], 2e-6);
  }
  HDRSHOT_CHECK(WindowsColor::linear_p3_to_scrgb({1, 0, 0}, 1)[1] < 0);
}
void capture_does_not_gamma_encode() {
  const std::vector<std::uint16_t> raw{0x3800, 0x3800, 0x3800, 0x7e00};
  const auto converted = WindowsColor::normalize_capture(raw, {false, 0});
  HDRSHOT_CHECK(converted.has_value());
  for (int c = 0; c < 3; ++c) HDRSHOT_CHECK(converted.value()[c] == 0x3800);
  HDRSHOT_CHECK(converted.value()[3] == 0x3c00);
  auto invalid = raw;
  invalid[0] = 0x7c00;
  HDRSHOT_CHECK(!WindowsColor::normalize_capture(invalid, {false, 0}));
}
void classification_and_coverage() {
  CanonicalFrameView frame{};
  frame.size_px = {2, 1};
  frame.encoding = WindowsColor::linear_p3_encoding();
  frame.display_dynamic_range = DisplayDynamicRange::hdr;
  frame.rgba_half = {0xbc00, 0x3c00, 0, 0x3c00, 0x4000, 0x4000, 0x4000, 0x3c00};
  AnnotationPixelPlan plan{0, {2, 1}, {{0, 0, 2}}, {}};
  WindowsLinearP3RangeProbe probe;
  HDRSHOT_CHECK(!probe.probe(FrameCropper::view(frame), plan, {}).value().fits_sdr);
  plan.source_visible_spans = {{0, 0, 1}};
  plan.annotation_owned_spans = {{0, 1, 1, ObjectId{1}, 0xff0000, {{{0.4F, 0, 0, 0.5F}}}}};
  const auto fit = probe.probe(FrameCropper::view(frame), plan, {});
  HDRSHOT_CHECK(fit && fit.value().fits_sdr && fit.value().skipped_annotation_pixel_count == 1);
  auto roi = FrameCropper::view(frame);
  roi.first_sample_offset = std::numeric_limits<std::size_t>::max();
  HDRSHOT_CHECK(!probe.probe(roi, plan, {}));
  frame.encoding.transfer = TransferFunction::extended_srgb;
  HDRSHOT_CHECK(!probe.probe(FrameCropper::view(frame), plan, {}));
}
void capture_tolerance_preserves_fp32_and_source_ownership() {
  WindowsLinearP3RangeProbe probe;
  for (const bool tolerance : {false, true}) {
    for (const float maximum : {1.0F, 1.00033021F, 1.001F, 1.002F,
        kCaptureSdrMaximumEdr, std::bit_cast<float>(kCaptureSdrMaximumEdrBits + 1U)}) {
      CanonicalFrameView frame{};
      frame.size_px = {2, 1};
      frame.encoding = WindowsColor::linear_p3_encoding();
      frame.display_dynamic_range = DisplayDynamicRange::hdr;
      frame.capture_sdr_tolerance = tolerance;
      // A colored excursion must classify by RGB, even when Y is below one.
      frame.rgba_float.assign({maximum, 0.1F, -0.2F, 1.0F, 4.0F, 4.0F, 4.0F, 1.0F});
      const auto before = frame.rgba_float;
      AnnotationPixelPlan ownership{0, {2, 1}, {{0, 0, 1}},
          {{0, 1, 1, ObjectId{1}, 0xffffff, {{{0.4F, 0, 0, 0.5F}}}}}};
      auto fit = probe.probe(FrameCropper::view(frame), ownership, {});
      const bool expected_sdr = tolerance ?
          maximum != std::bit_cast<float>(kCaptureSdrMaximumEdrBits + 1U) : maximum == 1.0F;
      HDRSHOT_CHECK(fit && fit.value().fits_sdr == expected_sdr);
      HDRSHOT_CHECK(fit.value().scanned_component_count == 3);
      ownership = {0, {2, 1}, {{0, 0, 2}}, {}};
      fit = probe.probe(FrameCropper::view(frame), ownership, {});
      HDRSHOT_CHECK(fit && !fit.value().fits_sdr && fit.value().scanned_component_count == 6);
      HDRSHOT_CHECK(std::memcmp(before.data(), frame.rgba_float.data(), before.size()*sizeof(float)) == 0);
    }
  }
  HDRSHOT_CHECK(source_requires_hdr(kCaptureHdrMinimumEdr, true));
}
void frozen_sdr_skips_pixels_but_validates_structure() {
  WindowsLinearP3RangeProbe probe;
  auto native = std::make_shared<SpyLinearSource>();
  SelectionRoiView roi{};
  roi.size_px = {1, 1};
  roi.encoding = WindowsColor::linear_p3_encoding();
  roi.display_dynamic_range = DisplayDynamicRange::sdr;
  roi.row_stride_samples = 4;
  roi.linear_source = native;
  AnnotationPixelPlan ownership{0, {1, 1}, {{0, 0, 1}}, {}};
  const auto fit = probe.probe(roi, ownership, {});
  HDRSHOT_CHECK(fit && fit.value().fits_sdr);
  HDRSHOT_CHECK(fit.value().source_visible_pixel_count == 1);
  HDRSHOT_CHECK(fit.value().scanned_component_count == 0);
  HDRSHOT_CHECK(native->read_count == 0 && native->wait_count == 0);
  auto bad = roi;
  bad.row_stride_samples = 0;
  HDRSHOT_CHECK(!probe.probe(bad, ownership, {}));
  HDRSHOT_CHECK(!probe.probe(roi, AnnotationPixelPlan{}, {}));
  HDRSHOT_CHECK(native->read_count == 0 && native->wait_count == 0);
  roi.display_dynamic_range = DisplayDynamicRange::hdr;
  const auto hdr = probe.probe(roi, ownership, {});
  HDRSHOT_CHECK(hdr && !hdr.value().fits_sdr && hdr.value().scanned_component_count == 3);
  HDRSHOT_CHECK(native->read_count == 1);
}
void frozen_display_class_and_hdr_threshold() {
  WindowsLinearP3RangeProbe probe;
  CanonicalFrameView frame{};
  frame.size_px = {1, 1};
  frame.encoding = WindowsColor::linear_p3_encoding();
  AnnotationPixelPlan ownership{0, {1, 1}, {{0, 0, 1}}, {}};
  // Legacy ICC device white and an ACM gamut excursion both remain SDR.
  for (const float excursion : {1.0040311F, 1.25F}) {
    frame.display_dynamic_range = DisplayDynamicRange::sdr;
    frame.rgba_float.assign({1.0F, 1.0F, excursion, 1.0F});
    const auto fit = probe.probe(FrameCropper::view(frame), ownership, {});
    HDRSHOT_CHECK(fit && fit.value().fits_sdr && fit.value().scanned_component_count == 0);
  }
  frame.display_dynamic_range = DisplayDynamicRange::hdr;
  frame.rgba_float.assign({1.0F, 0.5F, 0.0F, 1.0F});
  auto fit = probe.probe(FrameCropper::view(frame), ownership, {});
  HDRSHOT_CHECK(fit && fit.value().fits_sdr && fit.value().scanned_component_count == 3);
  frame.rgba_float[0] = 1.0040311F;
  fit = probe.probe(FrameCropper::view(frame), ownership, {});
  HDRSHOT_CHECK(fit && !fit.value().fits_sdr && fit.value().scanned_component_count == 3);
}
void compact_analysis_snapshot_restores_capture_range() {
  // Match AnalysisWorkflow's compact original/report shape: it marks its new
  // independent resource HDR before the Windows adapter restores capture state.
  auto pixels = std::make_shared<SpyLinearSource>();
  auto desktop = std::make_shared<FrozenDesktop>();
  CanonicalFrameSegment segment{};
  segment.display_id = DisplayId{7};
  segment.size_px = {1, 1};
  segment.encoding = WindowsColor::linear_p3_encoding();
  segment.display_dynamic_range = DisplayDynamicRange::hdr;
  segment.linear_source = pixels;
  desktop->canonical_segments.push_back(segment);
  auto clean = std::make_shared<CleanContentSnapshot>();
  clean->source = desktop;
  clean->display_id = segment.display_id;
  ExportSnapshot compact{};
  compact.frozen_desktop = desktop;
  compact.target_display_id = segment.display_id;
  compact.clean_content = clean;
  const auto restored = windows_snapshot_with_capture_range(compact, DisplayDynamicRange::sdr);
  HDRSHOT_CHECK(restored.has_value());
  HDRSHOT_CHECK(restored.value().frozen_desktop != compact.frozen_desktop);
  HDRSHOT_CHECK(restored.value().clean_content != compact.clean_content);
  HDRSHOT_CHECK(restored.value().clean_content->source == restored.value().frozen_desktop);
  HDRSHOT_CHECK(restored.value().frozen_desktop->canonical_segments.front().linear_source == pixels);
  HDRSHOT_CHECK(restored.value().frozen_desktop->canonical_segments.front().display_dynamic_range == DisplayDynamicRange::sdr);
  HDRSHOT_CHECK(compact.frozen_desktop->canonical_segments.front().display_dynamic_range == DisplayDynamicRange::hdr);
  HDRSHOT_CHECK(compact.clean_content->source == compact.frozen_desktop);
  HDRSHOT_CHECK(pixels->read_count == 0 && pixels->wait_count == 0);
  const auto hdr = windows_snapshot_with_capture_range(compact, DisplayDynamicRange::hdr);
  HDRSHOT_CHECK(hdr && hdr.value().frozen_desktop->canonical_segments.front().display_dynamic_range == DisplayDynamicRange::hdr);
  auto invalid = compact;
  invalid.target_display_id = DisplayId{8};
  HDRSHOT_CHECK(!windows_snapshot_with_capture_range(invalid, DisplayDynamicRange::sdr));
  invalid = compact;
  auto multiple = std::make_shared<FrozenDesktop>(*desktop);
  multiple->canonical_segments.push_back(segment);
  invalid.frozen_desktop = multiple;
  HDRSHOT_CHECK(!windows_snapshot_with_capture_range(invalid, DisplayDynamicRange::sdr));
  invalid = compact;
  invalid.clean_content = std::make_shared<CleanContentSnapshot>();
  HDRSHOT_CHECK(!windows_snapshot_with_capture_range(invalid, DisplayDynamicRange::sdr));
}
void real_analysis_original_and_report_keep_capture_range() {
  for (const auto captured : {DisplayDynamicRange::sdr, DisplayDynamicRange::hdr})
      for (const float maximum : {1.001F, 1.0040311F})
      for (const bool omit_target : {false, true}) {
    CanonicalFrameSegment segment{};
    segment.display_id = DisplayId{7};
    segment.desktop_frame_points = {0, 0, 1, 1};
    segment.size_px = {1, 1};
    segment.pixel_format = PixelFormat::rgba32_float;
    segment.encoding = WindowsColor::linear_p3_encoding();
    segment.display_dynamic_range = captured;
    segment.capture_sdr_tolerance = captured == DisplayDynamicRange::hdr;
    segment.rgba_float.assign({maximum, 1.0F, 1.0F, 1.0F});
    auto desktop = std::make_shared<FrozenDesktop>();
    desktop->desktop_bounds_points = {0, 0, 1, 1};
    desktop->canonical_segments.push_back(std::move(segment));
    ExportSnapshot original{};
    original.frozen_desktop = desktop;
    original.target_display_id = omit_target ? DisplayId{} : DisplayId{7};
    original.selection = {SelectionRevision{1}, {0, 0, 1, 1}};
    original.annotations.revision = DocumentRevision{2};
    original.annotation_render_plan = std::make_shared<AnnotationRenderPlan>(
        AnnotationRenderPlan{original.annotations.revision, {1, 1}, {}, original.selection.desktop_rect});
    WindowsLinearP3RangeProbe probe;
    auto cpu = make_cpu_analysis_port();
    const auto prepared = AnalysisWorkflow::prepare(original, *cpu, &probe);
    HDRSHOT_CHECK(prepared.has_value());
    const bool expected_hdr = captured == DisplayDynamicRange::hdr && maximum == 1.0040311F;
    HDRSHOT_CHECK(prepared.value().input.is_hdr == expected_hdr);
    const auto& compact = prepared.value().original;
    HDRSHOT_CHECK(compact.frozen_desktop->canonical_segments.front().capture_sdr_tolerance ==
        (captured == DisplayDynamicRange::hdr));
    HDRSHOT_CHECK(compact.frozen_desktop->canonical_segments.front().display_dynamic_range == DisplayDynamicRange::hdr);
    const auto restored = windows_snapshot_with_capture_range(compact, captured);
    HDRSHOT_CHECK(restored.has_value());
    HDRSHOT_CHECK(restored.value().clean_content->source == restored.value().frozen_desktop);
    auto view = FrameCropper::view_display(*restored.value().frozen_desktop,
        restored.value().target_display_id, restored.value().selection);
    HDRSHOT_CHECK(view.has_value() && view.value().display_dynamic_range == captured);
    auto plan = restored.value().clean_content->roi_plan(
        restored.value().selection.desktop_rect, restored.value().annotations.revision);
    HDRSHOT_CHECK(plan.has_value());
    const auto fit = probe.probe(view.value(), plan.value(), {});
    HDRSHOT_CHECK(fit && fit.value().fits_sdr == !expected_hdr);
    const auto report = AnalysisWorkflow::report_snapshot(compact, prepared.value().input.source);
    HDRSHOT_CHECK(report.has_value());
    HDRSHOT_CHECK(!report.value().frozen_desktop->canonical_segments.front().capture_sdr_tolerance);
    const auto restored_report = windows_snapshot_with_capture_range(report.value(), captured);
    HDRSHOT_CHECK(restored_report.has_value());
    HDRSHOT_CHECK(restored_report.value().clean_content->source == restored_report.value().frozen_desktop);
    HDRSHOT_CHECK(restored_report.value().frozen_desktop->canonical_segments.front().display_dynamic_range == captured);
    HDRSHOT_CHECK(report.value().frozen_desktop->canonical_segments.front().display_dynamic_range == DisplayDynamicRange::hdr);
    HDRSHOT_CHECK(original.frozen_desktop->canonical_segments.front().display_dynamic_range == captured);
    auto report_roi = FrameCropper::view_display(*restored_report.value().frozen_desktop,
        restored_report.value().target_display_id, restored_report.value().selection);
    HDRSHOT_CHECK(report_roi && !report_roi.value().capture_sdr_tolerance);
    auto report_plan = restored_report.value().clean_content->roi_plan(
        restored_report.value().selection.desktop_rect, restored_report.value().annotations.revision);
    HDRSHOT_CHECK(report_plan.has_value());
    const auto report_fit = probe.probe(report_roi.value(), report_plan.value(), {});
    HDRSHOT_CHECK(report_fit && report_fit.value().fits_sdr == (captured == DisplayDynamicRange::sdr));
  }
}
}
int main() {
  return hdrshot::test::run({{"white units", white_units}, {"gamut and extended range", gamut_and_extended_range},
      {"linear capture", capture_does_not_gamma_encode}, {"classification and AA ownership", classification_and_coverage},
      {"frozen SDR validates without pixel reads", frozen_sdr_skips_pixels_but_validates_structure},
      {"frozen display class and HDR threshold", frozen_display_class_and_hdr_threshold},
      {"capture FP32 tolerance and source ownership", capture_tolerance_preserves_fp32_and_source_ownership},
      {"compact analysis restores captured range", compact_analysis_snapshot_restores_capture_range},
      {"real analysis original and report preserve capture range", real_analysis_original_and_report_keep_capture_range}});
}
