#pragma once

#include "core/byte_sink.hpp"
#include "core/error.hpp"
#include "core/result.hpp"
#include "domain/png/png_encoder.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace hdrshot {

using Rgb16RowProvider =
    std::function<Result<std::vector<std::uint16_t>, Error>(std::uint32_t y)>;

struct EncodeStreamingPng16Request {
  std::uint32_t width{};
  std::uint32_t height{};
  PngColorMetadata color_metadata{CicpPayload{kBt2100PqFullRange}, std::nullopt};
  Rgb16RowProvider row_provider;
  std::string_view software{};
  // Compression hint only: samples are already quantized. This never changes
  // the RGB16 samples, IHDR bit depth, transfer function or metadata.
  std::uint8_t sample_precision_bits{16};
};

struct StreamingPngReceipt {
  std::size_t bytes_written{};
  std::uint32_t fast_rows{};
  std::uint32_t compression_level_changes{};
  int compression_memory_level{8};
};

class StreamingPngEncoder {
 public:
  static constexpr int kCompressionLevel = 6;
  static constexpr int kFastCompressionLevel = 3;
  static constexpr std::uint32_t kCompressionSwitchRows = 8U;
  static constexpr std::uint64_t kMinimumFastPixels = 1024U * 1024U;
  static constexpr int kMaximumMatchChain = 32;
  // One complete RGB16 pixel per 16 pixels. A stride of 16 *bytes* from
  // column zero samples only high bytes and misses all low-byte entropy.
  static constexpr std::size_t kFilterSampleStrideBytes = 96U;
  static constexpr std::size_t kFilterSampleWindowBytes = 6U;
  static constexpr std::uint32_t kFilterHysteresisPercent = 5U;

  [[nodiscard]] static Result<EncodedPng, Error> encode_16bit_rgb(
      const EncodeStreamingPng16Request& request);

  [[nodiscard]] static Result<StreamingPngReceipt, Error> encode_16bit_rgb_to_sink(
      const EncodeStreamingPng16Request& request,
      ByteSink& sink);
};

}  // namespace hdrshot
