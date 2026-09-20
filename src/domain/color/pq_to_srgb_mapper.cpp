#include "domain/color/pq_to_srgb_mapper.hpp"

#include "domain/color/pq_reference_white_mapper.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace hdrshot {
namespace {

double linear_to_srgb(const double linear) {
  return linear <= 0.0031308
      ? 12.92 * linear
      : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

std::uint16_t quantize_u16(const double encoded) {
  return static_cast<std::uint16_t>(
      std::floor(std::clamp(encoded, 0.0, 1.0) * 65535.0 + 0.5));
}

}  // namespace

Result<SrgbTransferMapResult, Error> PqToSrgbTransferMapper::map_rgba16(
    const std::span<const std::uint16_t> rgba_pq_bt2020_u16,
    const double pq_diffuse_white_nits) {
  if (rgba_pq_bt2020_u16.empty() || rgba_pq_bt2020_u16.size() % 4U != 0U ||
      !std::isfinite(pq_diffuse_white_nits) || pq_diffuse_white_nits <= 0.0) {
    return Result<SrgbTransferMapResult, Error>::failure(Error{
        ErrorCode::invalid_input,
        "PqToSrgbTransferMapper",
        Retryability::never,
        {{"reason", "invalid_rgba_or_diffuse_white"}},
    });
  }

  SrgbTransferMapResult result;
  result.rgba_bt2020_srgb_u16.resize(rgba_pq_bt2020_u16.size());
  const auto pixel_count = rgba_pq_bt2020_u16.size() / 4U;
  for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
    bool pixel_clipped = false;
    for (std::size_t channel = 0; channel < 3U; ++channel) {
      const auto pq = static_cast<double>(rgba_pq_bt2020_u16[pixel * 4U + channel]) /
          65535.0;
      const auto linear_bt2020 =
          PqReferenceWhiteMapper::st2084_eotf(pq) / pq_diffuse_white_nits;
      const bool clipped = linear_bt2020 > 1.0;
      if (clipped) {
        ++result.clipped_channel_count;
        pixel_clipped = true;
      }
      result.rgba_bt2020_srgb_u16[pixel * 4U + channel] = quantize_u16(
          linear_to_srgb(std::clamp(linear_bt2020, 0.0, 1.0)));
    }
    if (pixel_clipped) {
      ++result.clipped_pixel_count;
    }
    result.rgba_bt2020_srgb_u16[pixel * 4U + 3U] =
        rgba_pq_bt2020_u16[pixel * 4U + 3U];
  }
  return Result<SrgbTransferMapResult, Error>::success(std::move(result));
}

}  // namespace hdrshot
