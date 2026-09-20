#include "domain/frame/frame_pipeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include "domain/color/extended_p3_mapper.hpp"
#include <limits>
#include <string>

namespace hdrshot {
namespace {

Error frame_error(const ErrorCode code, const char* module, const DisplayId id = {},
    std::source_location origin = std::source_location::current()) {
  return Error{
      code,
      module,
      Retryability::never,
      id.value == 0 ? std::map<std::string, std::string>{}
                    : std::map<std::string, std::string>{{"displayId", std::to_string(id.value)}},
      origin,
  };
}

const DisplaySnapshot* find_display(
    const DisplaySnapshotSet& displays,
    const DisplayId id) {
  const auto found = std::find_if(
      displays.displays.begin(), displays.displays.end(),
      [id](const DisplaySnapshot& display) { return display.id == id; });
  return found == displays.displays.end() ? nullptr : &*found;
}

}  // namespace

Result<CanonicalFrameSegment, Error> SourceColorInterpreter::interpret(
    NativeCaptureFrame frame,
    const DisplaySnapshot& display) {
  if (frame.display_id != display.id || frame.size_px != display.capture_size_px ||
      frame.size_px.width <= 0 || frame.size_px.height <= 0) {
    return Result<CanonicalFrameSegment, Error>::failure(
        frame_error(ErrorCode::invalid_input, "SourceColorInterpreter", frame.display_id));
  }
  if (frame.pixel_format != PixelFormat::rgba16_float) {
    return Result<CanonicalFrameSegment, Error>::failure(frame_error(
        ErrorCode::unsupported_pixel_format, "SourceColorInterpreter", frame.display_id));
  }
  const auto width = static_cast<std::size_t>(frame.size_px.width);
  const auto height = static_cast<std::size_t>(frame.size_px.height);
  if (height != 0 && width > std::numeric_limits<std::size_t>::max() / height / 4U) {
    return Result<CanonicalFrameSegment, Error>::failure(
        frame_error(ErrorCode::invalid_input, "SourceColorInterpreter", frame.display_id));
  }
  const bool valid_pq = frame.encoding.transfer != TransferFunction::pq ||
      frame.encoding.source_reference_white_nits > 0.0;
  const bool valid_extended_p3 =
      frame.encoding.transfer != TransferFunction::extended_srgb ||
      (frame.encoding.primaries == ColorPrimaries::display_p3 &&
       frame.encoding.source_reference_white_nits == 0.0);
  if (frame.rgba_half.size() != width * height * 4U ||
      !valid_pq || !valid_extended_p3) {
    return Result<CanonicalFrameSegment, Error>::failure(frame_error(
        ErrorCode::invalid_color_contract, "SourceColorInterpreter", frame.display_id));
  }
  LinearFloatPixels linear{AlignedPixelAllocator<float>{std::move(frame.linear_storage)}};
  LinearSourceRef native_linear;
  if (frame.encoding.transfer == TransferFunction::extended_srgb && frame.source_normalizer) {
    auto normalized = frame.source_normalizer->normalize_extended_p3(
        frame.size_px, std::move(frame.rgba_half));
    if (!normalized) return Result<CanonicalFrameSegment, Error>::failure(normalized.error());
    native_linear = std::move(normalized.value());
    if (!native_linear || native_linear->size_px() != frame.size_px ||
        native_linear->byte_count() != width * height * 4U * sizeof(float))
      return Result<CanonicalFrameSegment, Error>::failure(frame_error(
          ErrorCode::invalid_color_contract, "SourceColorInterpreter", frame.display_id));
    frame.pixel_format = PixelFormat::rgba32_float;
    frame.encoding.transfer = TransferFunction::linear;
    frame.encoding.alpha = AlphaMode::opaque;
  } else if (frame.encoding.transfer == TransferFunction::extended_srgb) {
    // A table is exact for every finite binary16 input, with FP32 inverse math.
    // This is source normalization, not a half-precision linear cache.
    static const auto inverse = [] {
      std::array<float, 65536> values{};
      for (std::size_t i = 0; i < values.size(); ++i) {
        const auto decoded = ExtendedP3Mapper::decode_binary16(static_cast<std::uint16_t>(i));
        values[i] = decoded ? ExtendedP3Mapper::inverse_extended_srgb(decoded.value())
                            : std::numeric_limits<float>::quiet_NaN();
      }
      return values;
    }();
    // Capacity padding permits page-aligned native zero-copy bindings while the
    // logical sample count stays exact. At most 64 KiB extra, no platform types.
    constexpr std::size_t alignment_samples = 65536 / sizeof(float);
    const auto count = frame.rgba_half.size();
    if (count > std::numeric_limits<std::size_t>::max() - alignment_samples)
      return Result<CanonicalFrameSegment, Error>::failure(
          frame_error(ErrorCode::invalid_input, "SourceColorInterpreter", frame.display_id));
    linear.reserve((count + alignment_samples - 1) / alignment_samples * alignment_samples);
    linear.resize(count);
    for (std::size_t i = 0; i < linear.size(); i += 4) {
      for (std::size_t c = 0; c < 3; ++c) linear[i + c] = inverse[frame.rgba_half[i + c]];
      linear[i + 3] = 1.0F;
    }
    std::vector<std::uint16_t>().swap(frame.rgba_half);
    frame.pixel_format = PixelFormat::rgba32_float;
    frame.encoding.transfer = TransferFunction::linear;
    frame.encoding.alpha = AlphaMode::opaque;
  }
  return Result<CanonicalFrameSegment, Error>::success(CanonicalFrameSegment{
      frame.display_id,
      display.desktop_frame_points,
      display.point_pixel_scale,
      frame.size_px,
      frame.pixel_format,
      frame.encoding,
      display.dynamic_range,
      std::move(frame.rgba_half),
      std::move(linear),
      frame.pixel_format == PixelFormat::rgba32_float ? 1U : 0U,
      std::move(native_linear),
  });
}

Result<FrozenDesktop, Error> DisplayFrameAssembler::assemble(
    const FrameId frame_id,
    const DisplayGeneration generation,
    const DisplaySnapshotSet& displays,
    std::vector<CanonicalFrameSegment> segments) {
  if (generation == 0 || displays.generation != generation || segments.empty()) {
    return Result<FrozenDesktop, Error>::failure(frame_error(
        ErrorCode::display_configuration_changed, "DisplayFrameAssembler"));
  }

  double left = std::numeric_limits<double>::infinity();
  double top = std::numeric_limits<double>::infinity();
  double right = -std::numeric_limits<double>::infinity();
  double bottom = -std::numeric_limits<double>::infinity();
  for (const auto& segment : segments) {
    const auto* display = find_display(displays, segment.display_id);
    if (display == nullptr || segment.desktop_frame_points != display->desktop_frame_points ||
        segment.point_pixel_scale != display->point_pixel_scale) {
      return Result<FrozenDesktop, Error>::failure(frame_error(
          ErrorCode::display_configuration_changed, "DisplayFrameAssembler", segment.display_id));
    }
    left = std::min(left, segment.desktop_frame_points.x);
    top = std::min(top, segment.desktop_frame_points.y);
    right = std::max(right, segment.desktop_frame_points.x + segment.desktop_frame_points.width);
    bottom = std::max(bottom, segment.desktop_frame_points.y + segment.desktop_frame_points.height);
  }

  return Result<FrozenDesktop, Error>::success(FrozenDesktop{
      frame_id,
      generation,
      LogicalRect{left, top, right - left, bottom - top},
      std::move(segments),
  });
}

}  // namespace hdrshot
