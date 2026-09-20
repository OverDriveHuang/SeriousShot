#pragma once

#include "core/error.hpp"
#include "core/result.hpp"
#include "domain/output/content_light_statistics.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace hdrshot {

enum class PngColorType : std::uint8_t { rgb = 2, rgba = 6 };

struct CicpPayload {
  std::uint8_t color_primaries{};
  std::uint8_t transfer_function{};
  std::uint8_t matrix_coefficients{};
  std::uint8_t video_full_range_flag{};

  friend bool operator==(const CicpPayload&, const CicpPayload&) = default;
};

inline constexpr CicpPayload kBt2100PqFullRange{9, 16, 0, 1};
inline constexpr CicpPayload kBt2020SrgbFullRange{9, 13, 0, 1};
inline constexpr CicpPayload kDisplayP3PqFullRange{12, 16, 0, 1};
inline constexpr CicpPayload kDisplayP3SrgbFullRange{12, 13, 0, 1};

struct IccProfilePayload {
  std::string_view profile_name;
  std::span<const std::uint8_t> profile_bytes;
};

struct PngColorMetadata {
  std::optional<CicpPayload> cicp;
  std::optional<IccProfilePayload> icc;
  std::optional<ContentLightLevelInfo> content_light;

  PngColorMetadata(
      std::optional<CicpPayload> cicp_value = CicpPayload{kBt2100PqFullRange},
      std::optional<IccProfilePayload> icc_value = std::nullopt,
      std::optional<ContentLightLevelInfo> content_light_value = std::nullopt)
      : cicp(std::move(cicp_value)),
        icc(std::move(icc_value)),
        content_light(std::move(content_light_value)) {}
};

struct EncodePng16Request {
  std::uint32_t width{};
  std::uint32_t height{};
  PngColorType color_type{PngColorType::rgb};
  std::span<const std::uint16_t> samples;
  PngColorMetadata color_metadata{CicpPayload{kBt2100PqFullRange}, std::nullopt};
  std::string_view software{};
};

struct EncodedPng {
  std::vector<std::uint8_t> bytes;
};

class PngEncoder {
 public:
  [[nodiscard]] static Result<EncodedPng, Error> encode_16bit(const EncodePng16Request& request);
};

}  // namespace hdrshot
