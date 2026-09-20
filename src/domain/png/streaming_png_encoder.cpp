#include "domain/png/streaming_png_encoder.hpp"
#include "core/image_software_metadata.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include <zlib.h>

namespace hdrshot {
namespace {

Error png_error(const ErrorCode code, const char* reason,
    std::source_location origin = std::source_location::current()) {
  return Error{code, "StreamingPngEncoder", Retryability::never, {{"reason", reason}}, origin};
}

class VectorByteSink final : public ByteSink {
 public:
  Result<std::size_t, Error> write(const std::span<const std::uint8_t> bytes) override {
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    return Result<std::size_t, Error>::success(bytes.size());
  }

  std::vector<std::uint8_t> take() { return std::move(bytes_); }

 private:
  std::vector<std::uint8_t> bytes_;
};

Result<std::size_t, Error> write_all(
    ByteSink& sink,
    const std::span<const std::uint8_t> bytes) {
  std::size_t written = 0U;
  while (written < bytes.size()) {
    auto result = sink.write(bytes.subspan(written));
    if (!result) {
      return Result<std::size_t, Error>::failure(result.error());
    }
    if (result.value() == 0U || result.value() > bytes.size() - written) {
      return Result<std::size_t, Error>::failure(
          png_error(ErrorCode::png_encoding_failed, "invalid_sink_write_count"));
    }
    written += result.value();
  }
  return Result<std::size_t, Error>::success(written);
}

Result<std::size_t, Error> write_chunk(
    ByteSink& sink,
    const std::array<std::uint8_t, 4> type,
    const std::span<const std::uint8_t> data) {
  if (data.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return Result<std::size_t, Error>::failure(
        png_error(ErrorCode::png_encoding_failed, "chunk_size_overflow"));
  }
  const auto size = static_cast<std::uint32_t>(data.size());
  const std::array<std::uint8_t, 4> length{
      static_cast<std::uint8_t>((size >> 24U) & 0xFFU),
      static_cast<std::uint8_t>((size >> 16U) & 0xFFU),
      static_cast<std::uint8_t>((size >> 8U) & 0xFFU),
      static_cast<std::uint8_t>(size & 0xFFU),
  };
  auto crc = crc32(0U, reinterpret_cast<const Bytef*>(type.data()),
                   static_cast<uInt>(type.size()));
  if (!data.empty()) {
    crc = crc32(crc, reinterpret_cast<const Bytef*>(data.data()),
                static_cast<uInt>(data.size()));
  }
  const auto crc_value = static_cast<std::uint32_t>(crc);
  const std::array<std::uint8_t, 4> crc_bytes{
      static_cast<std::uint8_t>((crc_value >> 24U) & 0xFFU),
      static_cast<std::uint8_t>((crc_value >> 16U) & 0xFFU),
      static_cast<std::uint8_t>((crc_value >> 8U) & 0xFFU),
      static_cast<std::uint8_t>(crc_value & 0xFFU),
  };
  std::size_t written = 0U;
  for (const auto part : {std::span<const std::uint8_t>(length),
                          std::span<const std::uint8_t>(type), data,
                          std::span<const std::uint8_t>(crc_bytes)}) {
    auto result = write_all(sink, part);
    if (!result) {
      return Result<std::size_t, Error>::failure(result.error());
    }
    written += result.value();
  }
  return Result<std::size_t, Error>::success(written);
}

std::uint32_t read_u32_be(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
      (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
      (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
      static_cast<std::uint32_t>(bytes[offset + 3U]);
}

bool valid_profile_name(const std::string_view name) {
  if (name.empty() || name.size() > 79U || name.front() == ' ' || name.back() == ' ') {
    return false;
  }
  bool previous_space = false;
  for (const char raw_character : name) {
    const auto character = static_cast<unsigned char>(raw_character);
    if (!((character >= 0x20U && character <= 0x7EU) || character >= 0xA1U)) {
      return false;
    }
    if (character == 0x20U && previous_space) {
      return false;
    }
    previous_space = character == 0x20U;
  }
  return true;
}

bool valid_rgb_icc(const std::span<const std::uint8_t> profile) {
  return profile.size() >= 132U &&
      profile.size() <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) &&
      read_u32_be(profile, 0U) == profile.size() &&
      std::string_view(reinterpret_cast<const char*>(profile.data() + 16U), 4U) == "RGB " &&
      std::string_view(reinterpret_cast<const char*>(profile.data() + 36U), 4U) == "acsp";
}

bool valid_content_light(const PngColorMetadata& metadata) {
  if (!metadata.content_light.has_value()) {
    return true;
  }
  if (!metadata.cicp.has_value() || metadata.cicp->transfer_function != 16U) {
    return false;
  }
  const auto& value = *metadata.content_light;
  return value.max_cll_x10000 > 0U && value.max_fall_x10000 > 0U &&
      value.max_fall_x10000 <= value.max_cll_x10000 &&
      value.max_cll_x10000 <= kMaximumContentLightUnits;
}

std::array<std::uint8_t, 8> serialize_content_light(
    const ContentLightLevelInfo value) {
  return {
      static_cast<std::uint8_t>((value.max_cll_x10000 >> 24U) & 0xFFU),
      static_cast<std::uint8_t>((value.max_cll_x10000 >> 16U) & 0xFFU),
      static_cast<std::uint8_t>((value.max_cll_x10000 >> 8U) & 0xFFU),
      static_cast<std::uint8_t>(value.max_cll_x10000 & 0xFFU),
      static_cast<std::uint8_t>((value.max_fall_x10000 >> 24U) & 0xFFU),
      static_cast<std::uint8_t>((value.max_fall_x10000 >> 16U) & 0xFFU),
      static_cast<std::uint8_t>((value.max_fall_x10000 >> 8U) & 0xFFU),
      static_cast<std::uint8_t>(value.max_fall_x10000 & 0xFFU),
  };
}

Result<std::vector<std::uint8_t>, Error> compress_profile(
    const std::span<const std::uint8_t> source) {
  if (source.size() > static_cast<std::size_t>(std::numeric_limits<uLong>::max())) {
    return Result<std::vector<std::uint8_t>, Error>::failure(
        png_error(ErrorCode::invalid_input, "icc_size_overflow"));
  }
  const auto source_size = static_cast<uLong>(source.size());
  auto compressed_size = compressBound(source_size);
  std::vector<std::uint8_t> compressed(static_cast<std::size_t>(compressed_size));
  const auto result = compress2(
      reinterpret_cast<Bytef*>(compressed.data()),
      &compressed_size,
      reinterpret_cast<const Bytef*>(source.data()),
      source_size,
      StreamingPngEncoder::kCompressionLevel);
  if (result != Z_OK) {
    return Result<std::vector<std::uint8_t>, Error>::failure(
        png_error(ErrorCode::png_encoding_failed, "zlib_icc_compression_failed"));
  }
  compressed.resize(static_cast<std::size_t>(compressed_size));
  return Result<std::vector<std::uint8_t>, Error>::success(std::move(compressed));
}

std::uint8_t paeth_predictor(
    const std::uint8_t left,
    const std::uint8_t above,
    const std::uint8_t upper_left) {
  const auto prediction = static_cast<int>(left) + static_cast<int>(above) -
      static_cast<int>(upper_left);
  const auto left_distance = std::abs(prediction - static_cast<int>(left));
  const auto above_distance = std::abs(prediction - static_cast<int>(above));
  const auto upper_left_distance = std::abs(prediction - static_cast<int>(upper_left));
  if (left_distance <= above_distance && left_distance <= upper_left_distance) {
    return left;
  }
  return above_distance <= upper_left_distance ? above : upper_left;
}

std::uint8_t filter_byte(
    const std::uint8_t filter,
    const std::uint8_t current,
    const std::uint8_t left,
    const std::uint8_t above,
    const std::uint8_t upper_left) {
  std::uint8_t predictor = 0U;
  switch (filter) {
    case 1U: predictor = left; break;
    case 2U: predictor = above; break;
    case 4U: predictor = paeth_predictor(left, above, upper_left); break;
    default: break;
  }
  return static_cast<std::uint8_t>(current - predictor);
}

std::uint64_t sampled_filter_score(
    const std::uint8_t filter,
    const std::span<const std::uint8_t> current,
    const std::span<const std::uint8_t> previous,
    const std::size_t bytes_per_pixel) {
  std::uint64_t score = 0U;
  for (std::size_t sample = 0U; sample < current.size();
       sample += StreamingPngEncoder::kFilterSampleStrideBytes) {
    const auto end = std::min(current.size(), sample + StreamingPngEncoder::kFilterSampleWindowBytes);
    for (auto column = sample; column < end; ++column) {
      const std::uint8_t left = column >= bytes_per_pixel
          ? current[column - bytes_per_pixel]
          : static_cast<std::uint8_t>(0U);
      const std::uint8_t upper_left = column >= bytes_per_pixel
          ? previous[column - bytes_per_pixel]
          : static_cast<std::uint8_t>(0U);
      const auto filtered = filter_byte(
          filter, current[column], left, previous[column], upper_left);
      const auto signed_value = static_cast<std::int8_t>(filtered);
      // High-byte predictability drives repeated RGB16 byte sequences; include
      // low bytes as well, without letting their quantization noise dominate.
      const auto weight = column % 2U == 0U ? 4U : 1U;
      score += weight * static_cast<std::uint64_t>(
          signed_value < 0 ? -static_cast<int>(signed_value) : signed_value);
    }
  }
  return score;
}

std::uint8_t choose_filter(
    const std::span<const std::uint8_t> current,
    const std::span<const std::uint8_t> previous,
    const std::uint8_t previous_filter) {
  static constexpr std::array<std::uint8_t, 4> kCandidates{0U, 1U, 2U, 4U};
  static constexpr std::size_t kBytesPerPixel = 6U;
  std::array<std::uint64_t, kCandidates.size()> scores{};
  auto best_index = std::size_t{0U};
  for (std::size_t index = 0U; index < kCandidates.size(); ++index) {
    scores[index] = sampled_filter_score(
        kCandidates[index], current, previous, kBytesPerPixel);
    if (scores[index] < scores[best_index]) {
      best_index = index;
    }
  }
  const auto previous_position = std::find(
      kCandidates.begin(), kCandidates.end(), previous_filter);
  if (previous_position != kCandidates.end()) {
    const auto index = static_cast<std::size_t>(previous_position - kCandidates.begin());
    const auto best = scores[best_index];
    const auto hysteresis = std::max<std::uint64_t>(
        1U, best * StreamingPngEncoder::kFilterHysteresisPercent / 100U);
    if (scores[index] <= best + hysteresis) {
      return previous_filter;
    }
  }
  return kCandidates[best_index];
}

void filter_full_row(
    const std::uint8_t filter,
    const std::span<const std::uint8_t> current,
    const std::span<const std::uint8_t> previous,
    std::vector<std::uint8_t>& filtered) {
  static constexpr std::size_t kBytesPerPixel = 6U;
  filtered[0] = filter;
  // Select once per row, not once per byte. Sub/Up become simple vectorizable
  // differences; None is a copy. This is byte-for-byte the PNG filter math.
  if (filter == 0U) {
    std::copy(current.begin(), current.end(), filtered.begin() + 1);
    return;
  }
  if (filter == 1U) {
    const auto prefix = std::min(kBytesPerPixel, current.size());
    std::copy_n(current.begin(), prefix, filtered.begin() + 1);
    for (std::size_t i = prefix; i < current.size(); ++i)
      filtered[i + 1] = static_cast<std::uint8_t>(current[i] - current[i - kBytesPerPixel]);
    return;
  }
  if (filter == 2U) {
    for (std::size_t i = 0; i < current.size(); ++i)
      filtered[i + 1] = static_cast<std::uint8_t>(current[i] - previous[i]);
    return;
  }
  for (std::size_t column = 0U; column < current.size(); ++column) {
    const std::uint8_t left = column >= kBytesPerPixel
        ? current[column - kBytesPerPixel]
        : static_cast<std::uint8_t>(0U);
    const std::uint8_t upper_left = column >= kBytesPerPixel
        ? previous[column - kBytesPerPixel]
        : static_cast<std::uint8_t>(0U);
    filtered[column + 1U] = static_cast<std::uint8_t>(
        current[column] - paeth_predictor(left, previous[column], upper_left));
  }
}

Result<bool, Error> append_deflated(
    z_stream& stream,
    ByteSink& sink,
    std::size_t& bytes_written,
    const std::span<const std::uint8_t> input,
    const int flush) {
  stream.next_in = input.empty()
      ? Z_NULL
      : const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());
  std::array<std::uint8_t, 64U * 1024U> output;
  int result = Z_OK;
  do {
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    result = deflate(&stream, flush);
    if (result != Z_OK && result != Z_STREAM_END) {
      return Result<bool, Error>::failure(
          png_error(ErrorCode::png_encoding_failed, "zlib_image_compression_failed"));
    }
    const auto produced = output.size() - static_cast<std::size_t>(stream.avail_out);
    if (produced > 0U) {
      auto written = write_chunk(
          sink, {'I', 'D', 'A', 'T'},
          std::span<const std::uint8_t>(output.data(), produced));
      if (!written) {
        return Result<bool, Error>::failure(written.error());
      }
      bytes_written += written.value();
    }
  } while (stream.avail_in > 0U || stream.avail_out == 0U ||
           (flush == Z_FINISH && result != Z_STREAM_END));
  return Result<bool, Error>::success(result == Z_STREAM_END);
}

struct RowCompressionSample {
  int match_budget;
  bool textured;
};

RowCompressionSample sample_compression(const std::span<const std::uint8_t> filtered) {
  // Residual magnitude is a cheap texture heuristic, not an HDR/range test.
  // Deep searches still help smooth gradients. No second full row traversal.
  std::uint64_t magnitude = 0, count = 0, high_magnitude = 0, high_count = 0;
  for (std::size_t sample = 1; sample < filtered.size(); sample += 96) {
    for (auto x = sample; x < std::min(sample + 6, filtered.size()); ++x) {
      const auto value = static_cast<int>(static_cast<std::int8_t>(filtered[x]));
      magnitude += static_cast<std::uint64_t>(std::abs(value));
      ++count;
      if ((x - 1U) % 2U == 0U) {
        high_magnitude += static_cast<std::uint64_t>(std::abs(value));
        ++high_count;
      }
    }
  }
  const bool noisy = magnitude > count * 16U;
  // Quantized smooth gradients can have noisy LOW bytes but almost constant
  // high bytes. Do not trade away their useful long repeated sequences.
  return {noisy ? StreamingPngEncoder::kMaximumMatchChain : 128,
          noisy && high_count != 0U && high_magnitude > high_count * 2U};
}

Result<bool, Error> change_compression_level(
    z_stream& stream, ByteSink& sink, std::size_t& bytes_written, const int level) {
  stream.next_in = Z_NULL;
  stream.avail_in = 0;
  std::array<std::uint8_t, 64U * 1024U> output;
  for (;;) {
    stream.next_out = output.data();
    stream.avail_out = static_cast<uInt>(output.size());
    // Changing fast/lazy matching can emit an old-level block. It is part of
    // the SAME zlib stream; never discard these bytes or reset the dictionary.
    const auto result = deflateParams(&stream, level, Z_DEFAULT_STRATEGY);
    const auto produced = output.size() - stream.avail_out;
    if (produced != 0U) {
      auto written = write_chunk(sink, {'I', 'D', 'A', 'T'},
          std::span<const std::uint8_t>(output.data(), produced));
      if (!written) return Result<bool, Error>::failure(written.error());
      bytes_written += written.value();
    }
    if (result == Z_OK) return Result<bool, Error>::success(true);
    if (result != Z_BUF_ERROR || produced == 0U) {
      return Result<bool, Error>::failure(
          png_error(ErrorCode::png_encoding_failed, "zlib_level_change_failed"));
    }
  }
}

}  // namespace

Result<EncodedPng, Error> StreamingPngEncoder::encode_16bit_rgb(
    const EncodeStreamingPng16Request& request) {
  VectorByteSink sink;
  auto receipt = encode_16bit_rgb_to_sink(request, sink);
  if (!receipt) {
    return Result<EncodedPng, Error>::failure(receipt.error());
  }
  return Result<EncodedPng, Error>::success(EncodedPng{sink.take()});
}

Result<StreamingPngReceipt, Error> StreamingPngEncoder::encode_16bit_rgb_to_sink(
    const EncodeStreamingPng16Request& request,
    ByteSink& sink) {
  if (!valid_software_metadata(request.software)) {
    return Result<StreamingPngReceipt, Error>::failure(
        png_error(ErrorCode::metadata_conflict, "invalid_software_metadata"));
  }
  if (request.width == 0U || request.height == 0U || !request.row_provider) {
    return Result<StreamingPngReceipt, Error>::failure(
        png_error(ErrorCode::invalid_input, "empty_dimensions_or_row_provider"));
  }
  if (request.sample_precision_bits != 10U && request.sample_precision_bits != 12U &&
      request.sample_precision_bits != 16U) {
    return Result<StreamingPngReceipt, Error>::failure(
        png_error(ErrorCode::invalid_input, "invalid_png_compression_precision_hint"));
  }
  if (!request.color_metadata.cicp.has_value() && !request.color_metadata.icc.has_value()) {
    return Result<StreamingPngReceipt, Error>::failure(
        png_error(ErrorCode::metadata_conflict, "missing_color_metadata"));
  }
  if (request.color_metadata.cicp.has_value() &&
      (request.color_metadata.cicp->matrix_coefficients != 0U ||
       request.color_metadata.cicp->video_full_range_flag > 1U)) {
    return Result<StreamingPngReceipt, Error>::failure(
        png_error(ErrorCode::metadata_conflict, "invalid_rgb_cicp"));
  }
  if (request.color_metadata.icc.has_value() &&
      (!valid_profile_name(request.color_metadata.icc->profile_name) ||
       !valid_rgb_icc(request.color_metadata.icc->profile_bytes))) {
    return Result<StreamingPngReceipt, Error>::failure(
        png_error(ErrorCode::metadata_conflict, "invalid_rgb_icc"));
  }
  if (!valid_content_light(request.color_metadata)) {
    return Result<StreamingPngReceipt, Error>::failure(
        png_error(ErrorCode::metadata_conflict, "invalid_content_light_metadata"));
  }
  const auto width = static_cast<std::size_t>(request.width);
  if (width > std::numeric_limits<std::size_t>::max() / 6U ||
      width * 6U > static_cast<std::size_t>(std::numeric_limits<uInt>::max()) - 1U) {
    return Result<StreamingPngReceipt, Error>::failure(
        png_error(ErrorCode::invalid_input, "row_size_overflow"));
  }

  std::size_t bytes_written = 0U;
  const std::array<std::uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
  auto signature_result = write_all(sink, signature);
  if (!signature_result) {
    return Result<StreamingPngReceipt, Error>::failure(signature_result.error());
  }
  bytes_written += signature_result.value();
  std::array<std::uint8_t, 13> ihdr{};
  ihdr[0] = static_cast<std::uint8_t>((request.width >> 24U) & 0xFFU);
  ihdr[1] = static_cast<std::uint8_t>((request.width >> 16U) & 0xFFU);
  ihdr[2] = static_cast<std::uint8_t>((request.width >> 8U) & 0xFFU);
  ihdr[3] = static_cast<std::uint8_t>(request.width & 0xFFU);
  ihdr[4] = static_cast<std::uint8_t>((request.height >> 24U) & 0xFFU);
  ihdr[5] = static_cast<std::uint8_t>((request.height >> 16U) & 0xFFU);
  ihdr[6] = static_cast<std::uint8_t>((request.height >> 8U) & 0xFFU);
  ihdr[7] = static_cast<std::uint8_t>(request.height & 0xFFU);
  ihdr[8] = 16U;
  ihdr[9] = static_cast<std::uint8_t>(PngColorType::rgb);
  auto ihdr_result = write_chunk(sink, {'I', 'H', 'D', 'R'}, ihdr);
  if (!ihdr_result) {
    return Result<StreamingPngReceipt, Error>::failure(ihdr_result.error());
  }
  bytes_written += ihdr_result.value();

  if (request.color_metadata.cicp.has_value()) {
    const auto& value = *request.color_metadata.cicp;
    const std::array<std::uint8_t, 4> cicp{
        value.color_primaries,
        value.transfer_function,
        value.matrix_coefficients,
        value.video_full_range_flag,
    };
    auto result = write_chunk(sink, {'c', 'I', 'C', 'P'}, cicp);
    if (!result) {
      return Result<StreamingPngReceipt, Error>::failure(result.error());
    }
    bytes_written += result.value();
  }
  if (request.color_metadata.icc.has_value()) {
    const auto& value = *request.color_metadata.icc;
    auto compressed = compress_profile(value.profile_bytes);
    if (!compressed) {
      return Result<StreamingPngReceipt, Error>::failure(compressed.error());
    }
    std::vector<std::uint8_t> iccp;
    iccp.reserve(value.profile_name.size() + 2U + compressed.value().size());
    iccp.insert(iccp.end(), value.profile_name.begin(), value.profile_name.end());
    iccp.push_back(0U);
    iccp.push_back(0U);
    iccp.insert(iccp.end(), compressed.value().begin(), compressed.value().end());
    auto result = write_chunk(sink, {'i', 'C', 'C', 'P'}, iccp);
    if (!result) {
      return Result<StreamingPngReceipt, Error>::failure(result.error());
    }
    bytes_written += result.value();
  }
  if (request.color_metadata.content_light.has_value()) {
    const auto clli = serialize_content_light(*request.color_metadata.content_light);
    auto result = write_chunk(sink, {'c', 'L', 'L', 'I'}, clli);
    if (!result) {
      return Result<StreamingPngReceipt, Error>::failure(result.error());
    }
    bytes_written += result.value();
  }

  if (!request.software.empty()) {
    auto result = write_chunk(sink, {'t', 'E', 'X', 't'}, png_software_text(request.software));
    if (!result) {
      return Result<StreamingPngReceipt, Error>::failure(result.error());
    }
    bytes_written += result.value();
  }

  z_stream stream{};
  struct DeflateCleanup {
    z_stream& stream;
    ~DeflateCleanup() { if (stream.state != nullptr) deflateEnd(&stream); }
  } cleanup{stream};
#ifdef HDRSHOT_PNG_FIXED_COMPRESSION_PROBE
  // Offline diagnostic target only; keep fixed-parameter P5 measurements reproducible.
  const bool allow_fast = false;
#else
  const bool allow_fast = std::uint64_t(request.width) * request.height >= kMinimumFastPixels;
#endif
  int level = kCompressionLevel;
  int memory_level = 8;
  int previous_budget = -1;
  std::uint32_t pending_rows = 0, fast_rows = 0, level_changes = 0;
  const auto row_bytes = width * 6U;
  std::vector<std::uint8_t> previous(row_bytes, 0U);
  std::vector<std::uint8_t> current(row_bytes, 0U);
  std::vector<std::uint8_t> filtered(row_bytes + 1U, 0U);
  std::uint8_t previous_filter = 1U;
  for (std::uint32_t y = 0U; y < request.height; ++y) {
    auto samples = request.row_provider(y);
    if (!samples) {
      return Result<StreamingPngReceipt, Error>::failure(samples.error());
    }
    if (samples.value().size() != width * 3U) {
      return Result<StreamingPngReceipt, Error>::failure(
          png_error(ErrorCode::invalid_input, "row_sample_count_mismatch"));
    }
    for (std::size_t index = 0U; index < samples.value().size(); ++index) {
      current[index * 2U] = static_cast<std::uint8_t>((samples.value()[index] >> 8U) & 0xFFU);
      current[index * 2U + 1U] = static_cast<std::uint8_t>(samples.value()[index] & 0xFFU);
    }
    const auto filter = y == 0U
        ? static_cast<std::uint8_t>(1U)
        : choose_filter(current, previous, previous_filter);
    filter_full_row(filter, current, previous, filtered);
    const auto sampled = sample_compression(filtered);
    const auto budget = sampled.match_budget;
    const int desired_level = allow_fast && sampled.textured ? kFastCompressionLevel : kCompressionLevel;
    if (y == 0U) {
      level = desired_level;
      if (level == kFastCompressionLevel) {
        memory_level = request.sample_precision_bits == 10U ? 5 :
            request.sample_precision_bits == 12U ? 8 : 9;
      }
      if (deflateInit2(&stream, level, Z_DEFLATED, 15, memory_level, Z_DEFAULT_STRATEGY) != Z_OK) {
        return Result<StreamingPngReceipt, Error>::failure(
            png_error(ErrorCode::png_encoding_failed, "zlib_init_failed"));
      }
    } else if (desired_level == level) {
      pending_rows = 0U;
    } else if (++pending_rows >= kCompressionSwitchRows) {
      auto changed = change_compression_level(stream, sink, bytes_written, desired_level);
      if (!changed) return Result<StreamingPngReceipt, Error>::failure(changed.error());
      level = desired_level;
      pending_rows = 0U;
      previous_budget = -1; // deflateParams restores level-specific defaults.
      ++level_changes;
    }
    if (budget != previous_budget) {
      // Preserve the measured fine-tuning for both matching modes. Tuning
      // itself does not flush/reset the dictionary or change pixel values.
      if (deflateTune(&stream, 8, 16, 128, budget) != Z_OK) {
        return Result<StreamingPngReceipt, Error>::failure(
            png_error(ErrorCode::png_encoding_failed, "zlib_tuning_failed"));
      }
      previous_budget = budget;
    }
    auto compressed = append_deflated(
        stream, sink, bytes_written, filtered, Z_NO_FLUSH);
    if (!compressed) {
      return Result<StreamingPngReceipt, Error>::failure(compressed.error());
    }
    if (level == kFastCompressionLevel) ++fast_rows;
    previous_filter = filter;
    previous.swap(current);
  }
  auto finished = append_deflated(stream, sink, bytes_written, {}, Z_FINISH);
  if (!finished || !finished.value()) {
    return Result<StreamingPngReceipt, Error>::failure(
        finished ? png_error(ErrorCode::png_encoding_failed, "zlib_stream_not_finished")
                 : finished.error());
  }
  auto iend_result = write_chunk(sink, {'I', 'E', 'N', 'D'}, {});
  if (!iend_result) {
    return Result<StreamingPngReceipt, Error>::failure(iend_result.error());
  }
  bytes_written += iend_result.value();
  return Result<StreamingPngReceipt, Error>::success(
      StreamingPngReceipt{bytes_written, fast_rows, level_changes, memory_level});
}

}  // namespace hdrshot
