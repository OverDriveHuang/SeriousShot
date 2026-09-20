#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace hdrshot {

inline constexpr double kContentLightUnitsPerNit = 10000.0;
inline constexpr std::uint32_t kMaximumContentLightUnits = 100000000U;

struct ContentLightStatistics {
  std::uint32_t max_cll_x10000{};
  std::uint64_t luminance_sum_x10000{};
  std::uint64_t pixel_count{};

  friend bool operator==(const ContentLightStatistics&,
                         const ContentLightStatistics&) = default;
};

struct ContentLightLevelInfo {
  std::uint32_t max_cll_x10000{};
  std::uint32_t max_fall_x10000{};

  friend bool operator==(const ContentLightLevelInfo&,
                         const ContentLightLevelInfo&) = default;
};

[[nodiscard]] inline std::uint32_t content_light_units(
    const double nits) noexcept {
  const auto bounded = std::clamp(nits, 0.0, 10000.0);
  return static_cast<std::uint32_t>(
      std::floor(bounded * kContentLightUnitsPerNit + 0.5));
}

inline void accumulate_content_light(
    ContentLightStatistics& statistics,
    const double pixel_max_nits) noexcept {
  const auto value = content_light_units(pixel_max_nits);
  statistics.max_cll_x10000 = std::max(statistics.max_cll_x10000, value);
  statistics.luminance_sum_x10000 += value;
  ++statistics.pixel_count;
}

[[nodiscard]] inline std::optional<ContentLightLevelInfo>
finalize_content_light(const ContentLightStatistics& statistics) noexcept {
  if (statistics.pixel_count == 0U || statistics.max_cll_x10000 == 0U) {
    return std::nullopt;
  }
  const auto rounded_average =
      (statistics.luminance_sum_x10000 + statistics.pixel_count / 2U) /
      statistics.pixel_count;
  if (rounded_average == 0U ||
      rounded_average > statistics.max_cll_x10000 ||
      statistics.max_cll_x10000 > kMaximumContentLightUnits ||
      rounded_average > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return ContentLightLevelInfo{
      statistics.max_cll_x10000,
      static_cast<std::uint32_t>(rounded_average),
  };
}

}  // namespace hdrshot
