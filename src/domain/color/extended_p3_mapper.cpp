#include "domain/color/extended_p3_mapper.hpp"

#include <algorithm>
#include <cmath>

namespace hdrshot {
namespace {

Error invalid_half() {
  return Error{
      ErrorCode::invalid_color_contract,
      "ExtendedP3Mapper",
      Retryability::never,
      {{"reason", "non_finite_half"}},
  };
}

float srgb_to_linear(const float encoded) noexcept {
  return encoded <= 0.04045F
      ? encoded / 12.92F
      : std::pow((encoded + 0.055F) / 1.055F, 2.4F);
}

}  // namespace

Result<float, Error> ExtendedP3Mapper::decode_binary16(const std::uint16_t bits) {
  const auto sign = (bits & 0x8000U) != 0U;
  const auto exponent = static_cast<std::uint16_t>((bits >> 10U) & 0x1FU);
  const auto mantissa = static_cast<std::uint16_t>(bits & 0x03FFU);
  if (exponent == 0x1FU) {
    return Result<float, Error>::failure(invalid_half());
  }
  float magnitude = 0.0F;
  if (exponent == 0U) {
    magnitude = std::ldexp(static_cast<float>(mantissa), -24);
  } else {
    magnitude = std::ldexp(
        1.0F + static_cast<float>(mantissa) / 1024.0F,
        static_cast<int>(exponent) - 15);
  }
  return Result<float, Error>::success(sign ? -magnitude : magnitude);
}

std::uint16_t ExtendedP3Mapper::encode_binary16(const float value) noexcept {
  if (std::isnan(value)) return 0x7E00U;
  if (std::isinf(value)) return std::signbit(value) ? 0xFC00U : 0x7C00U;
  const auto sign = std::signbit(value) ? 0x8000U : 0U;
  const float magnitude = std::abs(value);
  if (magnitude == 0.0F) return static_cast<std::uint16_t>(sign);
  if (magnitude >= 65504.0F) {
    return static_cast<std::uint16_t>(sign | 0x7BFFU);
  }
  if (magnitude < std::ldexp(1.0F, -14)) {
    const auto mantissa = static_cast<std::uint32_t>(std::nearbyint(
        std::ldexp(magnitude, 24)));
    return static_cast<std::uint16_t>(sign | std::min(mantissa, 0x03FFU));
  }
  int exponent = 0;
  const float normalized = std::frexp(magnitude, &exponent);
  auto half_exponent = exponent + 14;
  auto mantissa = static_cast<std::uint32_t>(std::nearbyint(
      (normalized * 2.0F - 1.0F) * 1024.0F));
  if (mantissa == 1024U) {
    mantissa = 0U;
    ++half_exponent;
  }
  return static_cast<std::uint16_t>(
      sign | (static_cast<std::uint32_t>(half_exponent) << 10U) | mantissa);
}

float ExtendedP3Mapper::inverse_extended_srgb(const float encoded) noexcept {
  const auto magnitude = std::abs(encoded);
  const auto linear = magnitude <= 0.04045F
      ? magnitude / 12.92F
      : std::pow((magnitude + 0.055F) / 1.055F, 2.4F);
  return std::copysign(linear, encoded);
}

float ExtendedP3Mapper::encode_extended_srgb(const float linear) noexcept {
  const auto magnitude = std::abs(linear);
  const auto encoded = magnitude <= 0.0031308F
      ? 12.92F * magnitude
      : 1.055F * std::pow(magnitude, 1.0F / 2.4F) - 0.055F;
  return std::copysign(encoded, linear);
}

std::uint16_t ExtendedP3Mapper::quantize_unorm16(const float encoded) noexcept {
  return static_cast<std::uint16_t>(std::floor(
      std::clamp(encoded, 0.0F, 1.0F) * 65535.0F + 0.5F));
}

std::array<float, 3> ExtendedP3Mapper::annotation_linear_display_p3(
    const std::uint32_t srgb_rgb) noexcept {
  const auto red = srgb_to_linear(
      static_cast<float>((srgb_rgb >> 16U) & 0xFFU) / 255.0F);
  const auto green = srgb_to_linear(
      static_cast<float>((srgb_rgb >> 8U) & 0xFFU) / 255.0F);
  const auto blue = srgb_to_linear(
      static_cast<float>(srgb_rgb & 0xFFU) / 255.0F);
  return {
      0.82246197F * red + 0.17753803F * green,
      0.03319420F * red + 0.96680580F * green,
      0.01708263F * red + 0.07239744F * green + 0.91051993F * blue,
  };
}

std::array<std::uint16_t, 3> ExtendedP3Mapper::annotation_display_p3_u16(
    const std::uint32_t srgb_rgb) noexcept {
  const auto linear = annotation_linear_display_p3(srgb_rgb);
  return {
      quantize_unorm16(encode_extended_srgb(linear[0])),
      quantize_unorm16(encode_extended_srgb(linear[1])),
      quantize_unorm16(encode_extended_srgb(linear[2])),
  };
}

}  // namespace hdrshot
