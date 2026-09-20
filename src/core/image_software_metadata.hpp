#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

namespace hdrshot {

// The product identifier is printable ASCII. Empty opts out for standalone
// codec callers; the production workflow always supplies the compiled value.
[[nodiscard]] inline bool valid_software_metadata(std::string_view software) {
  return software.size() <= 1024U &&
      std::all_of(software.begin(), software.end(), [](unsigned char c) {
        return c >= 0x20U && c <= 0x7EU;
      });
}

[[nodiscard]] inline std::vector<std::uint8_t> png_software_text(
    std::string_view software) {
  std::vector<std::uint8_t> payload{'S', 'o', 'f', 't', 'w', 'a', 'r', 'e', 0};
  payload.insert(payload.end(), software.begin(), software.end());
  return payload;
}

// APP1 payload, NOT an APP1 marker/length: Exif identifier + little-endian
// TIFF with just IFD0.Software (0x0131, ASCII). Offsets are TIFF-relative.
// Callers validate the bounded ASCII value before serializing.
[[nodiscard]] inline std::vector<std::uint8_t> jpeg_software_exif(
    std::string_view software) {
  std::vector<std::uint8_t> payload(32U, 0U);
  payload[0] = 'E'; payload[1] = 'x'; payload[2] = 'i'; payload[3] = 'f';
  payload[6] = 'I'; payload[7] = 'I'; payload[8] = 42U; payload[10] = 8U;
  payload[14] = 1U;  // One IFD0 entry.
  payload[16] = 0x31U; payload[17] = 0x01U; payload[18] = 2U;
  const auto count = static_cast<std::uint32_t>(software.size() + 1U);
  for (unsigned int i = 0; i < 4U; ++i) {
    payload[20U + i] = static_cast<std::uint8_t>(count >> (8U * i));
  }
  if (count <= 4U) {
    std::copy(software.begin(), software.end(), payload.begin() + 24);
  } else {
    payload[24] = 26U;  // TIFF header + IFD count/entry/next-IFD pointer.
    payload.insert(payload.end(), software.begin(), software.end());
    payload.push_back(0U);
  }
  return payload;
}

}  // namespace hdrshot
