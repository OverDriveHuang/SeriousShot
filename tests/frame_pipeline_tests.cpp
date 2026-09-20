#include "domain/frame/frame_pipeline.hpp"
#include "test_support.hpp"

#include <cstdint>
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
      {"assembler preserves segments and signed bounds", assembler_preserves_segments_and_signed_bounds},
      {"assembler rejects generation mismatch", assembler_rejects_generation_and_geometry_mismatch},
  });
}
