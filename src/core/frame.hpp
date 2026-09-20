#pragma once

#include "core/geometry.hpp"
#include "core/linear_pixel_storage.hpp"
#include "core/linear_source.hpp"
#include "core/ids.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hdrshot {

struct LogicalRect {
  double x{};
  double y{};
  double width{};
  double height{};

  friend bool operator==(const LogicalRect&, const LogicalRect&) = default;
};

enum class PixelFormat : std::uint8_t { rgba16_float, rgba32_float };
enum class ColorPrimaries : std::uint8_t { bt2020, display_p3, srgb_bt709 };
enum class TransferFunction : std::uint8_t { pq, linear, srgb, extended_srgb };
enum class AlphaMode : std::uint8_t { straight, premultiplied, opaque };
enum class DisplayDynamicRange : std::uint8_t { sdr, hdr };

struct ColorEncoding {
  ColorPrimaries primaries{ColorPrimaries::bt2020};
  TransferFunction transfer{TransferFunction::pq};
  AlphaMode alpha{AlphaMode::straight};
  double source_reference_white_nits{};

  friend bool operator==(const ColorEncoding&, const ColorEncoding&) = default;
};

struct DisplaySnapshot {
  DisplayId id{};
  LogicalRect desktop_frame_points{};
  double point_pixel_scale{1.0};
  PixelSize capture_size_px{};
  DisplayDynamicRange dynamic_range{DisplayDynamicRange::sdr};

  friend bool operator==(const DisplaySnapshot&, const DisplaySnapshot&) = default;
};

struct DisplaySnapshotSet {
  DisplayGeneration generation{};
  std::vector<DisplaySnapshot> displays;

  friend bool operator==(const DisplaySnapshotSet&, const DisplaySnapshotSet&) = default;
};

struct NativeCaptureFrame {
  DisplayId display_id{};
  PixelSize size_px{};
  PixelFormat pixel_format{PixelFormat::rgba16_float};
  ColorEncoding encoding{};
  std::vector<std::uint16_t> rgba_half;
  std::shared_ptr<LinearStoragePort> linear_storage;
  std::shared_ptr<SourceNormalizerPort> source_normalizer{};
};

struct NativeFrameBatch {
  SessionId session_id{};
  OperationId operation_id{};
  DisplayGeneration display_generation{};
  std::vector<NativeCaptureFrame> frames;
};

struct CanonicalFrameSegment {
  DisplayId display_id{};
  LogicalRect desktop_frame_points{};
  double point_pixel_scale{1.0};
  PixelSize size_px{};
  PixelFormat pixel_format{PixelFormat::rgba16_float};
  ColorEncoding encoding{};
  DisplayDynamicRange display_dynamic_range{DisplayDynamicRange::sdr};
  std::vector<std::uint16_t> rgba_half;
  // Exactly one of rgba_half, rgba_float, linear_source is populated.
  // FP32 preserves software inverse-transfer precision; native storage is not a CPU array.
  LinearFloatPixels rgba_float;
  std::uint32_t software_linearization_passes{};
  LinearSourceRef linear_source{};
};

struct FrozenDesktop {
  FrameId frame_id{};
  DisplayGeneration display_generation{};
  LogicalRect desktop_bounds_points{};
  std::vector<CanonicalFrameSegment> canonical_segments;
};

using FrozenDesktopRef = std::shared_ptr<const FrozenDesktop>;

}  // namespace hdrshot
