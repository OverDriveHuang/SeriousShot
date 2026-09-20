#pragma once

#include "test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <zlib.h>

namespace hdrshot::test {

inline std::uint32_t metadata_u32(std::span<const std::uint8_t> bytes,
                                std::size_t at, bool little = false) {
  HDRSHOT_CHECK(at <= bytes.size() && bytes.size() - at >= 4U);
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i)
    value = (value << 8U) | bytes[at + (little ? 3U - i : i)];
  return value;
}

struct SoftwareReadback {
  std::string software;
  std::vector<std::uint8_t> without_software;
};

inline SoftwareReadback read_png_software(std::span<const std::uint8_t> bytes) {
  HDRSHOT_CHECK(bytes.size() >= 8 && bytes[0] == 137 && bytes[1] == 'P');
  SoftwareReadback result{"", {bytes.begin(), bytes.begin() + 8}};
  for (std::size_t at = 8; at < bytes.size();) {
    const auto size = metadata_u32(bytes, at);
    HDRSHOT_CHECK(bytes.size() - at >= 12U && size <= bytes.size() - at - 12U);
    const auto type_and_data = bytes.subspan(at + 4, size + 4);
    HDRSHOT_CHECK(crc32(0, type_and_data.data(), static_cast<uInt>(size + 4)) ==
                  metadata_u32(bytes, at + size + 8));
    const std::string_view type{reinterpret_cast<const char*>(bytes.data() + at + 4), 4};
    const auto data = bytes.subspan(at + 8, size);
    constexpr std::string_view key{"Software\0", 9};
    if (type == "tEXt" && data.size() >= key.size() &&
        std::equal(key.begin(), key.end(), data.begin())) {
      HDRSHOT_CHECK(result.software.empty()); // No duplicate Software.
      result.software.assign(reinterpret_cast<const char*>(data.data() + 9), data.size() - 9);
    } else {
      result.without_software.insert(result.without_software.end(),
          bytes.begin() + static_cast<std::ptrdiff_t>(at),
          bytes.begin() + static_cast<std::ptrdiff_t>(at + size + 12));
    }
    at += size + 12;
  }
  return result;
}

// Independent IFD walk: verify tag, type, count, TIFF-relative offset and NUL.
inline std::string read_exif_software(std::span<const std::uint8_t> payload) {
  constexpr std::string_view signature{"Exif\0\0", 6};
  HDRSHOT_CHECK(payload.size() >= 14 &&
      std::equal(signature.begin(), signature.end(), payload.begin()));
  const auto tiff = payload.subspan(6);
  HDRSHOT_CHECK(tiff[0] == 'I' && tiff[1] == 'I' && tiff[2] == 42 && tiff[3] == 0);
  const auto u16 = [&](std::size_t at) {
    HDRSHOT_CHECK(at + 2 <= tiff.size());
    return std::uint16_t(tiff[at] | (std::uint16_t{tiff[at + 1]} << 8U));
  };
  const auto ifd = metadata_u32(tiff, 4, true);
  const auto count = u16(ifd);
  HDRSHOT_CHECK(count == 1); // No invented camera/OS/colour tags.
  const auto at = ifd + 2U;
  HDRSHOT_CHECK(u16(at) == 0x0131 && u16(at + 2) == 2);
  const auto size = metadata_u32(tiff, at + 4, true);
  const auto offset = size <= 4 ? at + 8 : metadata_u32(tiff, at + 8, true);
  HDRSHOT_CHECK(size > 0 && offset <= tiff.size() && size <= tiff.size() - offset);
  HDRSHOT_CHECK(tiff[offset + size - 1] == 0);
  HDRSHOT_CHECK(metadata_u32(tiff, at + 12, true) == 0); // No further IFDs.
  return {reinterpret_cast<const char*>(tiff.data() + offset), size - 1};
}

// Strip only EXIF and the MPF index (whose offsets necessarily change), then
// retain the entire entropy stream and secondary JPEG verbatim for comparison.
inline SoftwareReadback read_jpeg_software(std::span<const std::uint8_t> bytes) {
  HDRSHOT_CHECK(bytes.size() >= 4 && bytes[0] == 0xff && bytes[1] == 0xd8);
  SoftwareReadback result{"", {bytes.begin(), bytes.begin() + 2}};
  std::size_t at = 2;
  for (; at + 4 <= bytes.size();) {
    HDRSHOT_CHECK(bytes[at] == 0xff);
    const auto marker = bytes[at + 1];
    if (marker == 0xda || marker == 0xd9) break;
    const auto length = (std::size_t{bytes[at + 2]} << 8U) | bytes[at + 3];
    HDRSHOT_CHECK(length >= 2 && length <= bytes.size() - at - 2);
    const auto payload = bytes.subspan(at + 4, length - 2);
    const auto starts = [&](std::string_view signature) {
      return payload.size() >= signature.size() &&
          std::equal(signature.begin(), signature.end(), payload.begin());
    };
    if (marker == 0xe1 && starts(std::string_view{"Exif\0\0", 6})) {
      HDRSHOT_CHECK(result.software.empty());
      result.software = read_exif_software(payload);
    } else if (!(marker == 0xe2 && starts(std::string_view{"MPF\0", 4}))) {
      result.without_software.insert(result.without_software.end(),
          bytes.begin() + static_cast<std::ptrdiff_t>(at),
          bytes.begin() + static_cast<std::ptrdiff_t>(at + length + 2));
    }
    at += length + 2;
  }
  result.without_software.insert(result.without_software.end(),
      bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.end());
  return result;
}

}  // namespace hdrshot::test
