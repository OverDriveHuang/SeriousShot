#include "domain/frame/frame_pipeline.hpp"
#include "domain/frame/frame_cropper.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace hdrshot;

DisplaySnapshot display(
    const std::uint64_t id,
    const LogicalRect frame,
    const PixelSize pixels,
    const DisplayDynamicRange range = DisplayDynamicRange::sdr) {
  return DisplaySnapshot{DisplayId{id}, frame, 1.0, pixels, range};
}

NativeCaptureFrame native_frame(const std::uint64_t id, const PixelSize pixels) {
  return NativeCaptureFrame{
      DisplayId{id},
      pixels,
      PixelFormat::rgba16_float,
      ColorEncoding{ColorPrimaries::bt2020, TransferFunction::pq, AlphaMode::straight, 100.0},
      std::vector<std::uint16_t>(
          static_cast<std::size_t>(pixels.width * pixels.height * 4), 0x3800),
  };
}

class TestLinearSource final : public LinearSource {
public:
  PixelSize size{2, 2};
  std::size_t bytes{2U * 2U * 4U * sizeof(float)};
  PixelSize size_px() const override { return size; }
  std::size_t byte_count() const override { return bytes; }
  Result<bool, Error> wait_until_ready() const override {
    return Result<bool, Error>::success(true);
  }
  Result<LinearFloatPixels, Error> read_region(PixelRect rect) const override {
    return Result<LinearFloatPixels, Error>::success(LinearFloatPixels(
        static_cast<std::size_t>(rect.width) * rect.height * 4U, 1.0F));
  }
};

void interpreter_accepts_only_unambiguous_native_linear_source() {
  const auto snapshot = display(7, LogicalRect{0, 0, 2, 2}, {2, 2});
  auto native = [] {
    NativeCaptureFrame frame{};
    frame.display_id = DisplayId{7};
    frame.size_px = {2, 2};
    frame.pixel_format = PixelFormat::rgba32_float;
    frame.encoding = {ColorPrimaries::display_p3, TransferFunction::linear,
                      AlphaMode::opaque, 0.0};
    frame.linear_source = std::make_shared<TestLinearSource>();
    return frame;
  };
  auto valid = SourceColorInterpreter::interpret(native(), snapshot);
  HDRSHOT_CHECK(valid.has_value());
  HDRSHOT_CHECK(static_cast<bool>(valid.value().linear_source));
  HDRSHOT_CHECK(valid.value().rgba_half.empty() &&
                valid.value().rgba_float.empty());
  HDRSHOT_CHECK(!valid.value().capture_sdr_tolerance);
  auto capture = native();
  capture.capture_sdr_tolerance = true;
  auto opted = SourceColorInterpreter::interpret(std::move(capture), snapshot);
  HDRSHOT_CHECK(opted && opted.value().capture_sdr_tolerance);
  HDRSHOT_CHECK(opted.value().software_linearization_passes == 0);
  const auto source = opted.value().linear_source;
  auto desktop = std::make_shared<FrozenDesktop>();
  desktop->canonical_segments.push_back(std::move(opted.value()));
  const auto roi = FrameCropper::view_display(*desktop, DisplayId{7},
      {SelectionRevision{1}, {0, 0, 1, 1}});
  HDRSHOT_CHECK(roi && roi.value().capture_sdr_tolerance);
  const auto read = FrameCropper::read_cpu_region(roi.value());
  HDRSHOT_CHECK(read && read.value().capture_sdr_tolerance);
  HDRSHOT_CHECK(FrameCropper::view(read.value()).capture_sdr_tolerance);
  HDRSHOT_CHECK(desktop->canonical_segments.front().linear_source == source);
  auto wrong = native();
  wrong.rgba_half = {0x3c00};
  HDRSHOT_CHECK(!SourceColorInterpreter::interpret(std::move(wrong), snapshot));
  wrong = native();
  wrong.pixel_format = PixelFormat::rgba16_float;
  HDRSHOT_CHECK(!SourceColorInterpreter::interpret(std::move(wrong), snapshot));
  wrong = native();
  wrong.encoding.transfer = TransferFunction::extended_srgb;
  HDRSHOT_CHECK(!SourceColorInterpreter::interpret(std::move(wrong), snapshot));
  wrong = native();
  wrong.encoding.primaries = ColorPrimaries::srgb_bt709;
  HDRSHOT_CHECK(!SourceColorInterpreter::interpret(std::move(wrong), snapshot));
  wrong = native();
  wrong.encoding.alpha = AlphaMode::straight;
  HDRSHOT_CHECK(!SourceColorInterpreter::interpret(std::move(wrong), snapshot));
  wrong = native();
  wrong.encoding.source_reference_white_nits = 80.0;
  HDRSHOT_CHECK(!SourceColorInterpreter::interpret(std::move(wrong), snapshot));
  wrong = native();
  auto sized_source = std::make_shared<TestLinearSource>();
  sized_source->size = {1, 2};
  wrong.linear_source = sized_source;
  HDRSHOT_CHECK(!SourceColorInterpreter::interpret(std::move(wrong), snapshot));
  wrong = native();
  auto short_source = std::make_shared<TestLinearSource>();
  short_source->bytes -= sizeof(float);
  wrong.linear_source = short_source;
  HDRSHOT_CHECK(!SourceColorInterpreter::interpret(std::move(wrong), snapshot));
}

void interpreter_preserves_explicit_source_semantics() {
  const auto snapshot = display(
      7,
      LogicalRect{-100.0, 0.0, 2.0, 2.0},
      PixelSize{2, 2},
      DisplayDynamicRange::hdr);
  auto result = SourceColorInterpreter::interpret(native_frame(7, {2, 2}), snapshot);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value().display_id == DisplayId{7});
  HDRSHOT_CHECK(result.value().desktop_frame_points.x == -100.0);
  HDRSHOT_CHECK(result.value().encoding.transfer == TransferFunction::pq);
  HDRSHOT_CHECK(result.value().encoding.source_reference_white_nits == 100.0);
  HDRSHOT_CHECK(result.value().display_dynamic_range == DisplayDynamicRange::hdr);
  HDRSHOT_CHECK(result.value().rgba_half.size() == 16);
}

void interpreter_rejects_size_and_color_mismatch() {
  const auto snapshot = display(7, LogicalRect{}, PixelSize{2, 2});
  auto frame = native_frame(7, {2, 2});
  frame.rgba_half.pop_back();
  const auto bad_size = SourceColorInterpreter::interpret(std::move(frame), snapshot);
  HDRSHOT_CHECK(!bad_size.has_value());
  HDRSHOT_CHECK(bad_size.error().code == ErrorCode::invalid_color_contract);

  frame = native_frame(7, {2, 2});
  frame.encoding.source_reference_white_nits = 0.0;
  const auto bad_white = SourceColorInterpreter::interpret(std::move(frame), snapshot);
  HDRSHOT_CHECK(!bad_white.has_value());
  HDRSHOT_CHECK(bad_white.error().code == ErrorCode::invalid_color_contract);
}

void assembler_preserves_segments_and_signed_bounds() {
  DisplaySnapshotSet displays{
      9,
      {
          display(1, LogicalRect{-2.0, 0.0, 2.0, 2.0}, PixelSize{2, 2}),
          display(2, LogicalRect{0.0, -1.0, 3.0, 3.0}, PixelSize{3, 3}),
      },
  };
  std::vector<CanonicalFrameSegment> segments;
  for (const auto& item : displays.displays) {
    auto interpreted = SourceColorInterpreter::interpret(
        native_frame(item.id.value, item.capture_size_px), item);
    HDRSHOT_CHECK(interpreted.has_value());
    segments.push_back(std::move(interpreted.value()));
  }
  const auto result = DisplayFrameAssembler::assemble(FrameId{12}, 9, displays, std::move(segments));
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value().canonical_segments.size() == 2);
  const auto expected_bounds = LogicalRect{-2.0, -1.0, 5.0, 3.0};
  HDRSHOT_CHECK(result.value().desktop_bounds_points == expected_bounds);
}

void assembler_rejects_generation_and_geometry_mismatch() {
  DisplaySnapshotSet displays{9, {display(1, LogicalRect{}, PixelSize{2, 2})}};
  auto interpreted = SourceColorInterpreter::interpret(native_frame(1, {2, 2}), displays.displays[0]);
  HDRSHOT_CHECK(interpreted.has_value());
  std::vector<CanonicalFrameSegment> segments;
  segments.push_back(std::move(interpreted.value()));
  const auto wrong_generation =
      DisplayFrameAssembler::assemble(FrameId{1}, 8, displays, std::move(segments));
  HDRSHOT_CHECK(!wrong_generation.has_value());
  HDRSHOT_CHECK(wrong_generation.error().code == ErrorCode::display_configuration_changed);
}

}  // namespace

int main() {
  using hdrshot::test::TestCase;
  return hdrshot::test::run(std::vector<TestCase>{
      {"interpreter preserves source semantics", interpreter_preserves_explicit_source_semantics},
      {"interpreter rejects invalid frame", interpreter_rejects_size_and_color_mismatch},
      {"interpreter validates native linear source", interpreter_accepts_only_unambiguous_native_linear_source},
      {"assembler preserves segments and signed bounds", assembler_preserves_segments_and_signed_bounds},
      {"assembler rejects generation mismatch", assembler_rejects_generation_and_geometry_mismatch},
  });
}
