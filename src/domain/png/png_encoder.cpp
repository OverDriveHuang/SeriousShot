#include "domain/png/png_encoder.hpp"
#include "core/image_software_metadata.hpp"

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

#include <zlib.h>

namespace hdrshot {
namespace {

void append_u32_be(std::vector<std::uint8_t>& output, const std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
  output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
  output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void append_chunk(
    std::vector<std::uint8_t>& output,
    const std::array<std::uint8_t, 4> type,
    const std::span<const std::uint8_t> data) {
  append_u32_be(output, static_cast<std::uint32_t>(data.size()));
  const auto type_offset = output.size();
  output.insert(output.end(), type.begin(), type.end());
  output.insert(output.end(), data.begin(), data.end());
  const auto crc = crc32(
      0U,
      reinterpret_cast<const Bytef*>(output.data() + type_offset),
      static_cast<uInt>(type.size() + data.size()));
  append_u32_be(output, static_cast<std::uint32_t>(crc));
}

[[nodiscard]] Error png_error(const ErrorCode code, const char* reason,
    std::source_location origin = std::source_location::current()) {
  return Error{code, "PngEncoder", Retryability::never, {{"reason", reason}}, origin};
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

Result<std::vector<std::uint8_t>, Error> compress_bytes(
    const std::span<const std::uint8_t> source,
    const char* failure_reason) {
  if (source.size() > static_cast<std::size_t>(std::numeric_limits<uLong>::max())) {
    return Result<std::vector<std::uint8_t>, Error>::failure(
        png_error(ErrorCode::invalid_input, "zlib_size_overflow"));
  }
  const auto source_size = static_cast<uLong>(source.size());
  auto compressed_size = compressBound(source_size);
  std::vector<std::uint8_t> compressed(static_cast<std::size_t>(compressed_size));
  const auto zlib_result = compress2(
      reinterpret_cast<Bytef*>(compressed.data()),
      &compressed_size,
      reinterpret_cast<const Bytef*>(source.data()),
      source_size,
      Z_BEST_COMPRESSION);
  if (zlib_result != Z_OK) {
    return Result<std::vector<std::uint8_t>, Error>::failure(
        png_error(ErrorCode::png_encoding_failed, failure_reason));
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
    case 3U:
      predictor = static_cast<std::uint8_t>(
          (static_cast<unsigned int>(left) + static_cast<unsigned int>(above)) / 2U);
      break;
    case 4U: predictor = paeth_predictor(left, above, upper_left); break;
    default: break;
  }
  return static_cast<std::uint8_t>(current - predictor);
}

std::uint64_t filter_score(const std::span<const std::uint8_t> filtered) {
  std::uint64_t score = 0U;
  for (const auto value : filtered) {
    const auto signed_value = static_cast<std::int8_t>(value);
    score += static_cast<std::uint64_t>(
        signed_value < 0 ? -static_cast<int>(signed_value) : signed_value);
  }
  return score;
}

}  // namespace

Result<EncodedPng, Error> PngEncoder::encode_16bit(const EncodePng16Request& request) {
  if (!valid_software_metadata(request.software)) {
    return Result<EncodedPng, Error>::failure(
        png_error(ErrorCode::metadata_conflict, "invalid_software_metadata"));
  }
  if (request.width == 0U || request.height == 0U) {
    return Result<EncodedPng, Error>::failure(png_error(ErrorCode::invalid_input, "empty_dimensions"));
  }
  if (!request.color_metadata.cicp.has_value() && !request.color_metadata.icc.has_value()) {
    return Result<EncodedPng, Error>::failure(
        png_error(ErrorCode::metadata_conflict, "missing_color_metadata"));
  }
  if (request.color_metadata.cicp.has_value() &&
      (request.color_metadata.cicp->matrix_coefficients != 0U ||
       request.color_metadata.cicp->video_full_range_flag > 1U)) {
    return Result<EncodedPng, Error>::failure(
        png_error(ErrorCode::metadata_conflict, "invalid_rgb_cicp"));
  }
  if (request.color_metadata.icc.has_value() &&
      (!valid_profile_name(request.color_metadata.icc->profile_name) ||
       !valid_rgb_icc(request.color_metadata.icc->profile_bytes))) {
    return Result<EncodedPng, Error>::failure(
        png_error(ErrorCode::metadata_conflict, "invalid_rgb_icc"));
  }
  if (!valid_content_light(request.color_metadata)) {
    return Result<EncodedPng, Error>::failure(
        png_error(ErrorCode::metadata_conflict, "invalid_content_light_metadata"));
  }

  const std::size_t channels = request.color_type == PngColorType::rgba ? 4U : 3U;
  const auto width = static_cast<std::size_t>(request.width);
  const auto height = static_cast<std::size_t>(request.height);
  if (width > std::numeric_limits<std::size_t>::max() / channels ||
      (width * channels) > std::numeric_limits<std::size_t>::max() / height) {
    return Result<EncodedPng, Error>::failure(png_error(ErrorCode::invalid_input, "size_overflow"));
  }
  const auto expected_samples = width * channels * height;
  if (request.samples.size() != expected_samples) {
    return Result<EncodedPng, Error>::failure(
        png_error(ErrorCode::invalid_input, "sample_count_mismatch"));
  }

  const auto row_bytes = width * channels * 2U;
  if (row_bytes > std::numeric_limits<std::size_t>::max() - 1U ||
      (row_bytes + 1U) > std::numeric_limits<std::size_t>::max() / height) {
    return Result<EncodedPng, Error>::failure(png_error(ErrorCode::invalid_input, "size_overflow"));
  }
  const auto scanline_size = (row_bytes + 1U) * height;
  if (scanline_size > static_cast<std::size_t>(std::numeric_limits<uLong>::max())) {
    return Result<EncodedPng, Error>::failure(png_error(ErrorCode::invalid_input, "zlib_size_overflow"));
  }
  std::vector<std::uint8_t> scanlines(scanline_size);
  std::vector<std::uint8_t> previous_row(row_bytes, 0U);
  std::vector<std::uint8_t> current_row(row_bytes, 0U);
  std::vector<std::uint8_t> candidate(row_bytes, 0U);
  std::vector<std::uint8_t> best(row_bytes, 0U);
  std::size_t sample_index = 0;
  std::size_t byte_index = 0;
  for (std::size_t row = 0; row < height; ++row) {
    for (std::size_t column_byte = 0; column_byte < row_bytes; column_byte += 2U) {
      const auto sample = request.samples[sample_index++];
      current_row[column_byte] = static_cast<std::uint8_t>((sample >> 8U) & 0xFFU);
      current_row[column_byte + 1U] = static_cast<std::uint8_t>(sample & 0xFFU);
    }
    std::uint8_t best_filter = 0U;
    auto best_score = std::numeric_limits<std::uint64_t>::max();
    const auto bytes_per_pixel = channels * 2U;
    for (std::uint8_t filter = 0U; filter <= 4U; ++filter) {
      for (std::size_t column = 0U; column < row_bytes; ++column) {
        const std::uint8_t left = column >= bytes_per_pixel
            ? current_row[column - bytes_per_pixel]
            : static_cast<std::uint8_t>(0U);
        const std::uint8_t upper_left = column >= bytes_per_pixel
            ? previous_row[column - bytes_per_pixel]
            : static_cast<std::uint8_t>(0U);
        candidate[column] = filter_byte(
            filter, current_row[column], left, previous_row[column], upper_left);
      }
      const auto score = filter_score(candidate);
      if (score < best_score) {
        best_score = score;
        best_filter = filter;
        best = candidate;
      }
    }
    scanlines[byte_index++] = best_filter;
    std::copy(best.begin(), best.end(), scanlines.begin() + static_cast<std::ptrdiff_t>(byte_index));
    byte_index += row_bytes;
    previous_row.swap(current_row);
  }

  auto compressed_scanlines = compress_bytes(scanlines, "zlib_image_compression_failed");
  if (!compressed_scanlines) {
    return Result<EncodedPng, Error>::failure(compressed_scanlines.error());
  }
  if (compressed_scanlines.value().size() >
          static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
      compressed_scanlines.value().size() + 4U >
          static_cast<std::size_t>(std::numeric_limits<uInt>::max())) {
    return Result<EncodedPng, Error>::failure(
        png_error(ErrorCode::png_encoding_failed, "idat_chunk_too_large"));
  }

  EncodedPng encoded;
  encoded.bytes.insert(encoded.bytes.end(), {137, 80, 78, 71, 13, 10, 26, 10});

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
  ihdr[9] = static_cast<std::uint8_t>(request.color_type);
  append_chunk(encoded.bytes, {'I', 'H', 'D', 'R'}, ihdr);

  if (request.color_metadata.cicp.has_value()) {
    const auto& value = *request.color_metadata.cicp;
    const std::array<std::uint8_t, 4> cicp{
        value.color_primaries,
        value.transfer_function,
        value.matrix_coefficients,
        value.video_full_range_flag,
    };
    append_chunk(encoded.bytes, {'c', 'I', 'C', 'P'}, cicp);
  }
  if (request.color_metadata.icc.has_value()) {
    const auto& value = *request.color_metadata.icc;
    auto compressed_profile = compress_bytes(
        value.profile_bytes, "zlib_icc_compression_failed");
    if (!compressed_profile) {
      return Result<EncodedPng, Error>::failure(compressed_profile.error());
    }
    std::vector<std::uint8_t> iccp;
    iccp.reserve(value.profile_name.size() + 2U + compressed_profile.value().size());
    iccp.insert(iccp.end(), value.profile_name.begin(), value.profile_name.end());
    iccp.push_back(0U);
    iccp.push_back(0U);
    iccp.insert(
        iccp.end(), compressed_profile.value().begin(), compressed_profile.value().end());
    append_chunk(encoded.bytes, {'i', 'C', 'C', 'P'}, iccp);
  }
  if (request.color_metadata.content_light.has_value()) {
    const auto clli = serialize_content_light(*request.color_metadata.content_light);
    append_chunk(encoded.bytes, {'c', 'L', 'L', 'I'}, clli);
  }
  if (!request.software.empty()) {
    append_chunk(encoded.bytes, {'t', 'E', 'X', 't'}, png_software_text(request.software));
  }
  append_chunk(encoded.bytes, {'I', 'D', 'A', 'T'}, compressed_scanlines.value());
  append_chunk(encoded.bytes, {'I', 'E', 'N', 'D'}, {});
  return Result<EncodedPng, Error>::success(std::move(encoded));
}

}  // namespace hdrshot
