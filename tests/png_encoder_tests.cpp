#include "domain/color/bt2020_srgb_icc_profile.hpp"
#include "domain/png/png_encoder.hpp"
#include "domain/png/streaming_png_encoder.hpp"
#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <zlib.h>

namespace {

using namespace hdrshot;

struct Chunk {
  std::string type;
  std::vector<std::uint8_t> data;
  std::uint32_t stored_crc{};
};

class PartialVectorSink final : public ByteSink {
 public:
  explicit PartialVectorSink(const std::size_t maximum_write)
      : maximum_write_(maximum_write) {}

  Result<std::size_t, Error> write(
      const std::span<const std::uint8_t> bytes) override {
    ++write_calls;
    const auto count = std::min(maximum_write_, bytes.size());
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.begin() +
        static_cast<std::ptrdiff_t>(count));
    return Result<std::size_t, Error>::success(count);
  }

  std::size_t write_calls{};
  std::vector<std::uint8_t> bytes_;

 private:
  std::size_t maximum_write_{};
};

std::uint32_t read_u32_be(const std::vector<std::uint8_t>& bytes, const std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
      (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
      (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
      static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::vector<Chunk> parse_chunks(const std::vector<std::uint8_t>& bytes) {
  HDRSHOT_CHECK(bytes.size() >= 8);
  const std::array<std::uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
  HDRSHOT_CHECK(std::equal(signature.begin(), signature.end(), bytes.begin()));
  std::vector<Chunk> chunks;
  std::size_t offset = 8;
  while (offset < bytes.size()) {
    HDRSHOT_CHECK(offset + 12 <= bytes.size());
    const auto length = static_cast<std::size_t>(read_u32_be(bytes, offset));
    HDRSHOT_CHECK(offset + 12 + length <= bytes.size());
    const auto type_offset = offset + 4;
    std::string type(reinterpret_cast<const char*>(bytes.data() + type_offset), 4);
    const auto data_begin = bytes.begin() + static_cast<std::ptrdiff_t>(offset + 8);
    const auto data_end = data_begin + static_cast<std::ptrdiff_t>(length);
    const auto stored_crc = read_u32_be(bytes, offset + 8 + length);
    const auto actual_crc = crc32(
        0U,
        reinterpret_cast<const Bytef*>(bytes.data() + type_offset),
        static_cast<uInt>(4 + length));
    HDRSHOT_CHECK(stored_crc == static_cast<std::uint32_t>(actual_crc));
    chunks.push_back(Chunk{std::move(type), {data_begin, data_end}, stored_crc});
    offset += 12 + length;
  }
  return chunks;
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

std::vector<std::uint8_t> unfilter(
    const std::vector<std::uint8_t>& filtered,
    const std::size_t row_bytes,
    const std::size_t height,
    const std::size_t bytes_per_pixel) {
  HDRSHOT_CHECK(filtered.size() == (row_bytes + 1U) * height);
  std::vector<std::uint8_t> decoded(row_bytes * height, 0U);
  for (std::size_t row = 0U; row < height; ++row) {
    const auto filter = filtered[row * (row_bytes + 1U)];
    HDRSHOT_CHECK(filter <= 4U);
    for (std::size_t column = 0U; column < row_bytes; ++column) {
      const auto source = filtered[row * (row_bytes + 1U) + 1U + column];
      const std::uint8_t left = column >= bytes_per_pixel
          ? decoded[row * row_bytes + column - bytes_per_pixel]
          : static_cast<std::uint8_t>(0U);
      const std::uint8_t above = row > 0U
          ? decoded[(row - 1U) * row_bytes + column]
          : static_cast<std::uint8_t>(0U);
      const std::uint8_t upper_left = row > 0U && column >= bytes_per_pixel
          ? decoded[(row - 1U) * row_bytes + column - bytes_per_pixel]
          : static_cast<std::uint8_t>(0U);
      std::uint8_t predictor = 0U;
      if (filter == 1U) predictor = left;
      if (filter == 2U) predictor = above;
      if (filter == 3U) predictor = static_cast<std::uint8_t>(
          (static_cast<unsigned int>(left) + static_cast<unsigned int>(above)) / 2U);
      if (filter == 4U) predictor = paeth_predictor(left, above, upper_left);
      decoded[row * row_bytes + column] = static_cast<std::uint8_t>(source + predictor);
    }
  }
  return decoded;
}

EncodedPng encode_reference_rgb() {
  static constexpr std::array<std::uint16_t, 9> samples{
      0, 0, 0,
      38055, 42871, 47785,
      65535, 65535, 65535,
  };
  const auto result = PngEncoder::encode_16bit(EncodePng16Request{
      3,
      1,
      PngColorType::rgb,
      samples,
      PngColorMetadata{kBt2100PqFullRange, std::nullopt},
  });
  HDRSHOT_CHECK(result.has_value());
  return result.value();
}

void structure_crc_and_cicp_match_reference() {
  const auto encoded = encode_reference_rgb();
  const auto chunks = parse_chunks(encoded.bytes);
  HDRSHOT_CHECK(chunks.size() == 4);
  HDRSHOT_CHECK(chunks[0].type == "IHDR");
  HDRSHOT_CHECK(chunks[1].type == "cICP");
  HDRSHOT_CHECK(chunks[2].type == "IDAT");
  HDRSHOT_CHECK(chunks[3].type == "IEND");
  HDRSHOT_CHECK(chunks[0].data.size() == 13);
  HDRSHOT_CHECK(chunks[0].data[8] == 16);
  HDRSHOT_CHECK(chunks[0].data[9] == static_cast<std::uint8_t>(PngColorType::rgb));
  HDRSHOT_CHECK(chunks[1].data == (std::vector<std::uint8_t>{9, 16, 0, 1}));
}

void decode_back_preserves_all_u16_samples() {
  const auto encoded = encode_reference_rgb();
  const auto chunks = parse_chunks(encoded.bytes);
  const auto& compressed = chunks[2].data;
  std::vector<std::uint8_t> raw(1 + (3 * 3 * 2));
  auto raw_size = static_cast<uLongf>(raw.size());
  const auto result = uncompress(
      reinterpret_cast<Bytef*>(raw.data()),
      &raw_size,
      reinterpret_cast<const Bytef*>(compressed.data()),
      static_cast<uLong>(compressed.size()));
  HDRSHOT_CHECK(result == Z_OK);
  HDRSHOT_CHECK(raw_size == raw.size());
  const auto decoded = unfilter(raw, 3U * 3U * 2U, 1U, 3U * 2U);
  const std::array<std::uint16_t, 9> expected{
      0, 0, 0,
      38055, 42871, 47785,
      65535, 65535, 65535,
  };
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const auto offset = index * 2;
    const auto actual = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(decoded[offset]) << 8U) |
        static_cast<std::uint16_t>(decoded[offset + 1]));
    HDRSHOT_CHECK(actual == expected[index]);
  }
}

void invalid_sample_count_is_rejected() {
  const std::array<std::uint16_t, 2> samples{0, 0};
  const auto result = PngEncoder::encode_16bit(
      EncodePng16Request{
          1,
          1,
          PngColorType::rgb,
          samples,
          PngColorMetadata{kBt2100PqFullRange, std::nullopt}});
  HDRSHOT_CHECK(!result.has_value());
  HDRSHOT_CHECK(result.error().code == ErrorCode::invalid_input);
}

void conflicting_rgb_cicp_is_rejected() {
  const std::array<std::uint16_t, 3> samples{0, 0, 0};
  const auto result = PngEncoder::encode_16bit(
      EncodePng16Request{
          1,
          1,
          PngColorType::rgb,
          samples,
          PngColorMetadata{CicpPayload{9, 16, 1, 1}, std::nullopt}});
  HDRSHOT_CHECK(!result.has_value());
  HDRSHOT_CHECK(result.error().code == ErrorCode::metadata_conflict);
}

void matching_cicp_and_iccp_round_trip_preserve_both() {
  const std::array<std::uint16_t, 3> samples{0, 32768, 65535};
  const auto profile = Bt2020SrgbIccProfile::bytes();
  const auto result = PngEncoder::encode_16bit(EncodePng16Request{
      1,
      1,
      PngColorType::rgb,
      samples,
      PngColorMetadata{
          kBt2020SrgbFullRange,
          IccProfilePayload{"HDRShot BT.2020 sRGB", profile},
      },
  });
  HDRSHOT_CHECK(result.has_value());
  const auto chunks = parse_chunks(result.value().bytes);
  HDRSHOT_CHECK(chunks.size() == 5);
  HDRSHOT_CHECK(chunks[1].type == "cICP");
  HDRSHOT_CHECK(chunks[1].data == (std::vector<std::uint8_t>{9, 13, 0, 1}));
  HDRSHOT_CHECK(chunks[2].type == "iCCP");

  const auto separator = std::find(chunks[2].data.begin(), chunks[2].data.end(), 0U);
  HDRSHOT_CHECK(separator != chunks[2].data.end());
  HDRSHOT_CHECK(std::string(chunks[2].data.begin(), separator) == "HDRShot BT.2020 sRGB");
  HDRSHOT_CHECK(separator + 1 != chunks[2].data.end());
  HDRSHOT_CHECK(*(separator + 1) == 0U);
  const auto compressed_begin = separator + 2;
  HDRSHOT_CHECK(compressed_begin < chunks[2].data.end());
  std::vector<std::uint8_t> decoded(profile.size());
  auto decoded_size = static_cast<uLongf>(decoded.size());
  const auto zlib_result = uncompress(
      reinterpret_cast<Bytef*>(decoded.data()),
      &decoded_size,
      reinterpret_cast<const Bytef*>(&*compressed_begin),
      static_cast<uLong>(std::distance(compressed_begin, chunks[2].data.end())));
  HDRSHOT_CHECK(zlib_result == Z_OK);
  HDRSHOT_CHECK(decoded_size == profile.size());
  HDRSHOT_CHECK(std::equal(decoded.begin(), decoded.end(), profile.begin(), profile.end()));
}

void missing_color_metadata_is_rejected() {
  const std::array<std::uint16_t, 3> samples{0, 0, 0};
  const auto result = PngEncoder::encode_16bit(EncodePng16Request{
      1,
      1,
      PngColorType::rgb,
      samples,
      PngColorMetadata{
          std::nullopt,
          std::nullopt,
      },
  });
  HDRSHOT_CHECK(!result.has_value());
  HDRSHOT_CHECK(result.error().code == ErrorCode::metadata_conflict);
}

void clli_is_eight_byte_big_endian_and_precedes_idat() {
  const std::array<std::uint16_t, 3> samples{100U, 200U, 300U};
  const auto metadata = PngColorMetadata{
      kDisplayP3PqFullRange,
      std::nullopt,
      ContentLightLevelInfo{12345678U, 3456789U},
  };
  const auto reference = PngEncoder::encode_16bit(EncodePng16Request{
      1U, 1U, PngColorType::rgb, samples, metadata});
  const auto streaming = StreamingPngEncoder::encode_16bit_rgb(
      EncodeStreamingPng16Request{
          1U,
          1U,
          metadata,
          [&samples](std::uint32_t) -> Result<std::vector<std::uint16_t>, Error> {
            return Result<std::vector<std::uint16_t>, Error>::success(
                {samples.begin(), samples.end()});
          },
      });
  HDRSHOT_CHECK(reference.has_value());
  HDRSHOT_CHECK(streaming.has_value());
  for (const auto* encoded : {&reference.value(), &streaming.value()}) {
    const auto chunks = parse_chunks(encoded->bytes);
    const auto clli = std::find_if(chunks.begin(), chunks.end(),
        [](const Chunk& chunk) { return chunk.type == "cLLI"; });
    const auto idat = std::find_if(chunks.begin(), chunks.end(),
        [](const Chunk& chunk) { return chunk.type == "IDAT"; });
    HDRSHOT_CHECK(clli != chunks.end());
    HDRSHOT_CHECK(idat != chunks.end());
    HDRSHOT_CHECK(clli < idat);
    HDRSHOT_CHECK(clli->data == (std::vector<std::uint8_t>{
        0x00U, 0xBCU, 0x61U, 0x4EU,
        0x00U, 0x34U, 0xBFU, 0x15U}));
  }
}

void clli_requires_pq_and_valid_content_levels() {
  const std::array<std::uint16_t, 3> samples{100U, 200U, 300U};
  for (const auto metadata : {
           PngColorMetadata{
               kDisplayP3SrgbFullRange,
               std::nullopt,
               ContentLightLevelInfo{100U, 50U}},
           PngColorMetadata{
               kDisplayP3PqFullRange,
               std::nullopt,
               ContentLightLevelInfo{50U, 100U}},
       }) {
    const auto result = PngEncoder::encode_16bit(EncodePng16Request{
        1U, 1U, PngColorType::rgb, samples, metadata});
    HDRSHOT_CHECK(!result.has_value());
    HDRSHOT_CHECK(result.error().code == ErrorCode::metadata_conflict);
  }
}

void adaptive_filters_reduce_structured_rgba16_data() {
  constexpr std::uint32_t width = 256U;
  constexpr std::uint32_t height = 128U;
  std::vector<std::uint16_t> samples;
  samples.reserve(static_cast<std::size_t>(width) * height * 4U);
  for (std::uint32_t row = 0U; row < height; ++row) {
    for (std::uint32_t column = 0U; column < width; ++column) {
      samples.insert(samples.end(), {
          static_cast<std::uint16_t>(column * 257U),
          static_cast<std::uint16_t>(row * 509U),
          static_cast<std::uint16_t>((column + row) * 171U),
          65535U,
      });
    }
  }
  const auto result = PngEncoder::encode_16bit(EncodePng16Request{
      width,
      height,
      PngColorType::rgba,
      samples,
      PngColorMetadata{kBt2100PqFullRange, std::nullopt},
  });
  HDRSHOT_CHECK(result.has_value());
  const auto chunks = parse_chunks(result.value().bytes);
  const auto idat = std::find_if(
      chunks.begin(), chunks.end(), [](const Chunk& chunk) { return chunk.type == "IDAT"; });
  HDRSHOT_CHECK(idat != chunks.end());
  const auto raw_bytes = static_cast<std::size_t>(width) * height * 8U;
  HDRSHOT_CHECK(idat->data.size() < raw_bytes / 20U);

  std::vector<std::uint8_t> filtered((static_cast<std::size_t>(width) * 8U + 1U) * height);
  auto filtered_size = static_cast<uLongf>(filtered.size());
  HDRSHOT_CHECK(uncompress(
      reinterpret_cast<Bytef*>(filtered.data()),
      &filtered_size,
      reinterpret_cast<const Bytef*>(idat->data.data()),
      static_cast<uLong>(idat->data.size())) == Z_OK);
  bool has_nonzero_filter = false;
  const auto stride = static_cast<std::size_t>(width) * 8U + 1U;
  for (std::size_t row = 0U; row < height; ++row) {
    has_nonzero_filter = has_nonzero_filter || filtered[row * stride] != 0U;
  }
  HDRSHOT_CHECK(has_nonzero_filter);
}

void streaming_encoder_decodes_exactly_and_never_uses_average_filter() {
  constexpr std::uint32_t width = 4U;
  constexpr std::uint32_t height = 3U;
  const std::array<std::array<std::uint16_t, width * 3U>, height> rows{{
      {0U, 100U, 200U, 300U, 400U, 500U, 600U, 700U, 800U, 900U, 1000U, 1100U},
      {1U, 101U, 201U, 301U, 401U, 501U, 601U, 701U, 801U, 901U, 1001U, 1101U},
      {2U, 102U, 202U, 302U, 402U, 502U, 602U, 702U, 802U, 902U, 1002U, 1102U},
  }};
  std::size_t provider_calls = 0U;
  const auto encoded = StreamingPngEncoder::encode_16bit_rgb(
      EncodeStreamingPng16Request{
          width,
          height,
          PngColorMetadata{kBt2100PqFullRange, std::nullopt},
          [&](const std::uint32_t y) -> Result<std::vector<std::uint16_t>, Error> {
            ++provider_calls;
            return Result<std::vector<std::uint16_t>, Error>::success(
                {rows[y].begin(), rows[y].end()});
          },
      });
  HDRSHOT_CHECK(encoded.has_value());
  HDRSHOT_CHECK(provider_calls == height);
  const auto chunks = parse_chunks(encoded.value().bytes);
  std::vector<std::uint8_t> compressed;
  for (const auto& chunk : chunks) {
    if (chunk.type == "IDAT") {
      compressed.insert(compressed.end(), chunk.data.begin(), chunk.data.end());
    }
  }
  HDRSHOT_CHECK(!compressed.empty());
  const auto row_bytes = static_cast<std::size_t>(width) * 6U;
  std::vector<std::uint8_t> filtered((row_bytes + 1U) * height);
  auto filtered_size = static_cast<uLongf>(filtered.size());
  HDRSHOT_CHECK(uncompress(
      reinterpret_cast<Bytef*>(filtered.data()),
      &filtered_size,
      reinterpret_cast<const Bytef*>(compressed.data()),
      static_cast<uLong>(compressed.size())) == Z_OK);
  HDRSHOT_CHECK(filtered_size == filtered.size());
  HDRSHOT_CHECK(filtered[0] == 1U);
  for (std::size_t y = 0U; y < height; ++y) {
    const auto filter = filtered[y * (row_bytes + 1U)];
    HDRSHOT_CHECK(filter == 0U || filter == 1U || filter == 2U || filter == 4U);
  }
  const auto decoded = unfilter(filtered, row_bytes, height, 6U);
  for (std::size_t y = 0U; y < height; ++y) {
    for (std::size_t index = 0U; index < rows[y].size(); ++index) {
      const auto offset = y * row_bytes + index * 2U;
      const auto actual = static_cast<std::uint16_t>(
          (static_cast<std::uint16_t>(decoded[offset]) << 8U) |
          static_cast<std::uint16_t>(decoded[offset + 1U]));
      HDRSHOT_CHECK(actual == rows[y][index]);
    }
  }
}

void filter_sampling_includes_low_bytes_and_all_rgb_components() {
  constexpr std::uint32_t width = 257;
  std::vector<std::uint16_t> row(width * 3);
  std::uint32_t state = 123;
  for (auto& sample : row) {
    state = state * 1664525U + 1013904223U;
    sample = static_cast<std::uint16_t>((state >> 24U) & 255U);
  }
  const auto result = StreamingPngEncoder::encode_16bit_rgb({width, 2,
      PngColorMetadata{kDisplayP3SrgbFullRange, std::nullopt},
      [&](std::uint32_t) { return Result<std::vector<std::uint16_t>, Error>::success(row); }});
  HDRSHOT_CHECK(result.has_value());
  std::vector<std::uint8_t> compressed;
  for (const auto& chunk : parse_chunks(result.value().bytes)) if (chunk.type == "IDAT")
    compressed.insert(compressed.end(), chunk.data.begin(), chunk.data.end());
  std::vector<std::uint8_t> filtered((width * 6U + 1U) * 2U);
  uLongf size = filtered.size();
  HDRSHOT_CHECK(uncompress(filtered.data(), &size, compressed.data(), compressed.size()) == Z_OK);
  // Identical rows: Up is perfect. High-byte-only sampling wrongly kept Sub.
  HDRSHOT_CHECK(filtered[width * 6U + 1U] == 2U);
  const auto decoded = unfilter(filtered, width * 6U, 2, 6);
  for (std::size_t i = 0; i < row.size() * 2; ++i)
    HDRSHOT_CHECK(((std::uint16_t(decoded[2*i]) << 8U) | decoded[2*i+1]) == row[i % row.size()]);
}

void streaming_encoder_supports_partial_sink_writes_without_final_blob() {
  constexpr std::uint32_t width = 8U;
  constexpr std::uint32_t height = 5U;
  const auto request = EncodeStreamingPng16Request{
      width,
      height,
      PngColorMetadata{kBt2100PqFullRange, std::nullopt},
      [](const std::uint32_t y) -> Result<std::vector<std::uint16_t>, Error> {
        std::vector<std::uint16_t> row(width * 3U);
        for (std::size_t index = 0U; index < row.size(); ++index) {
          row[index] = static_cast<std::uint16_t>(y * 100U + index);
        }
        return Result<std::vector<std::uint16_t>, Error>::success(std::move(row));
      }};
  const auto collected = StreamingPngEncoder::encode_16bit_rgb(request);
  HDRSHOT_CHECK(collected.has_value());

  PartialVectorSink sink{7U};
  const auto streamed = StreamingPngEncoder::encode_16bit_rgb_to_sink(request, sink);
  HDRSHOT_CHECK(streamed.has_value());
  HDRSHOT_CHECK(streamed.value().bytes_written == sink.bytes_.size());
  HDRSHOT_CHECK(sink.write_calls > 10U);
  HDRSHOT_CHECK(sink.bytes_ == collected.value().bytes);
}

std::vector<std::uint16_t> adaptive_row(std::uint32_t y, unsigned bits, bool first_texture) {
  std::vector<std::uint16_t> row(1024U * 3U);
  auto seed = 0xC001D00DU ^ (y * 1664525U);
  const bool texture = ((y / 128U) % 2U == 0U) == first_texture;
  const auto levels = (1U << bits) - 1U;
  for (std::size_t i = 0; i < row.size(); ++i) {
    seed = seed * 1664525U + 1013904223U;
    const auto value = texture ? seed >> 16U : static_cast<std::uint32_t>(i * 17U);
    const auto q = (value * levels + 32767U) / 65535U;
    row[i] = static_cast<std::uint16_t>((q * 65535U + levels / 2U) / levels);
  }
  return row;
}

void adaptive_compression_preserves_pixels_metadata_and_single_pass() {
  for (const auto bits : {10U, 12U, 16U}) for (const bool first_texture : {false, true}) {
    std::uint32_t calls = 0;
    PartialVectorSink sink{16381U};
    const auto result = StreamingPngEncoder::encode_16bit_rgb_to_sink({1024, 1024,
        PngColorMetadata{kDisplayP3PqFullRange, std::nullopt, ContentLightLevelInfo{10000000,4000000}},
        [&](std::uint32_t y) {
          HDRSHOT_CHECK(y == calls++);
          return Result<std::vector<std::uint16_t>, Error>::success(adaptive_row(y, bits, first_texture));
        }, "SeriousShot; compression test", static_cast<std::uint8_t>(bits)}, sink);
    HDRSHOT_CHECK(result.has_value());
    HDRSHOT_CHECK(calls == 1024U);
    HDRSHOT_CHECK(result.value().bytes_written == sink.bytes_.size());
    HDRSHOT_CHECK(result.value().fast_rows > 400U && result.value().fast_rows < 650U);
    HDRSHOT_CHECK(result.value().compression_level_changes >= 7U);
    HDRSHOT_CHECK(result.value().compression_memory_level ==
        (first_texture ? (bits == 10U ? 5 : bits == 12U ? 8 : 9) : 8));
    const auto chunks = parse_chunks(sink.bytes_); // validates every chunk CRC
    std::vector<std::uint8_t> compressed;
    bool cicp = false, clli = false, software = false;
    for (const auto& chunk : chunks) {
      if (chunk.type == "IDAT") compressed.insert(compressed.end(), chunk.data.begin(), chunk.data.end());
      if (chunk.type == "cICP") cicp = chunk.data == std::vector<std::uint8_t>{12,16,0,1};
      if (chunk.type == "cLLI") clli = chunk.data.size() == 8U && read_u32_be(chunk.data,0) == 10000000U;
      if (chunk.type == "tEXt") software = true;
    }
    HDRSHOT_CHECK(cicp && clli && software);
    std::vector<std::uint8_t> raw((1024U * 6U + 1U) * 1024U);
    uLongf size = raw.size();
    HDRSHOT_CHECK(uncompress(raw.data(), &size, compressed.data(), compressed.size()) == Z_OK);
    HDRSHOT_CHECK(size == raw.size());
    const auto decoded = unfilter(raw,1024U * 6U,1024U,6U);
    for (std::uint32_t y = 0; y < 1024; ++y) {
      const auto row = adaptive_row(y,bits,first_texture);
      for (std::size_t i = 0; i < row.size(); ++i) {
        const auto p = (std::size_t(y) * row.size() + i) * 2U;
        HDRSHOT_CHECK(((std::uint16_t(decoded[p]) << 8U) | decoded[p+1]) == row[i]);
      }
    }
  }
}

void adaptive_compression_validates_hint_and_propagates_provider_failure() {
  unsigned calls = 0;
  EncodeStreamingPng16Request request{1024,1024,
      PngColorMetadata{kDisplayP3PqFullRange, std::nullopt},
      [&](std::uint32_t y) -> Result<std::vector<std::uint16_t>, Error> {
        ++calls;
        if (y == 256U) return Result<std::vector<std::uint16_t>, Error>::failure(
            Error{ErrorCode::invalid_input,"test_row_failure",Retryability::never,{}});
        return Result<std::vector<std::uint16_t>, Error>::success(adaptive_row(y,10,true));
      }, {}, 8};
  const auto invalid = StreamingPngEncoder::encode_16bit_rgb(request);
  HDRSHOT_CHECK(!invalid && invalid.error().code == ErrorCode::invalid_input && calls == 0U);
  request.sample_precision_bits = 10;
  const auto failed = StreamingPngEncoder::encode_16bit_rgb(request);
  HDRSHOT_CHECK(!failed && failed.error().module == "test_row_failure" && calls == 257U);
}

void low_byte_noise_does_not_select_fast_mode_or_requantize_samples() {
  std::vector<std::uint8_t> baseline;
  for (const auto bits : {10U,12U,16U}) {
    PartialVectorSink sink{65536U};
    const auto result = StreamingPngEncoder::encode_16bit_rgb_to_sink({1024,1024,
        PngColorMetadata{kDisplayP3SrgbFullRange,std::nullopt},
        [](std::uint32_t y) {
          std::vector<std::uint16_t> row(3072);
          auto seed = y + 123U;
          for (auto& value : row) {
            seed = seed * 1664525U + 1013904223U;
            value = static_cast<std::uint16_t>(0x4000U | (seed >> 24U));
          }
          return Result<std::vector<std::uint16_t>,Error>::success(std::move(row));
        }, {}, static_cast<std::uint8_t>(bits)},sink);
    HDRSHOT_CHECK(result.has_value());
    HDRSHOT_CHECK(result.value().fast_rows == 0U && result.value().compression_level_changes == 0U);
    HDRSHOT_CHECK(result.value().compression_memory_level == 8);
    if (bits == 10U) baseline = sink.bytes_;
    else HDRSHOT_CHECK(sink.bytes_ == baseline); // hint does not touch the arbitrary input codes
  }
}

void compression_level_switch_preserves_sink_errors() {
  unsigned calls = 0;
  class FailingSink final : public ByteSink {
   public:
    explicit FailingSink(unsigned& rows) : rows_(rows) {}
    Result<std::size_t,Error> write(std::span<const std::uint8_t> bytes) override {
      if (rows_ >= 136U) return Result<std::size_t,Error>::failure(
          Error{ErrorCode::storage_full,"test_sink_failure",Retryability::never,{}});
      return Result<std::size_t,Error>::success(bytes.size());
    }
    unsigned& rows_;
  } sink(calls);
  const auto result = StreamingPngEncoder::encode_16bit_rgb_to_sink({1024,1024,
      PngColorMetadata{kDisplayP3PqFullRange,std::nullopt},
      [&](std::uint32_t y) {
        ++calls;
        return Result<std::vector<std::uint16_t>,Error>::success(adaptive_row(y,10,true));
      },{},10},sink);
  HDRSHOT_CHECK(!result && result.error().code == ErrorCode::storage_full);
  HDRSHOT_CHECK(result.error().module == "test_sink_failure");
  HDRSHOT_CHECK(calls == 136U); // rows 128…135: eight smooth rows, first fast→smooth block
}

}  // namespace

int main() {
  return hdrshot::test::run({
      {"D8-STRUCT-001 and D8-PQ-CICP-001", structure_crc_and_cicp_match_reference},
      {"D8-DECODE-001", decode_back_preserves_all_u16_samples},
      {"D8-INVALID-001 sample count", invalid_sample_count_is_rejected},
      {"D8-METADATA-002 conflict", conflicting_rgb_cicp_is_rejected},
      {"D8-ICC-001 matching cICP+iCCP round trip", matching_cicp_and_iccp_round_trip_preserve_both},
      {"D8-METADATA-003 metadata is required", missing_color_metadata_is_rejected},
      {"D8-CLLI-001 cLLI structure and order", clli_is_eight_byte_big_endian_and_precedes_idat},
      {"D8-CLLI-002 cLLI validation", clli_requires_pq_and_valid_content_levels},
      {"D8-COMPRESS-001 adaptive PNG filtering", adaptive_filters_reduce_structured_rgba16_data},
      {"D8-STREAM-001 BalancedRealtime streaming decode-back", streaming_encoder_decodes_exactly_and_never_uses_average_filter},
      {"D8-STREAM-002 partial byte sink writes", streaming_encoder_supports_partial_sink_writes_without_final_blob},
      {"D8-STREAM-003 low-byte-aware filter sampling", filter_sampling_includes_low_bytes_and_all_rgb_components},
      {"D8-STREAM-004 adaptive compression single stream", adaptive_compression_preserves_pixels_metadata_and_single_pass},
      {"D8-STREAM-005 compression hint and provider failure", adaptive_compression_validates_hint_and_propagates_provider_failure},
      {"D8-STREAM-006 low-byte noise keeps smooth policy", low_byte_noise_does_not_select_fast_mode_or_requantize_samples},
      {"D8-STREAM-007 block transition sink failure", compression_level_switch_preserves_sink_errors},
  });
}
