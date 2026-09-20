#pragma once

#include <algorithm>
#include <cstdint>

namespace hdrshot {

struct PixelPoint {
  std::int32_t x{};
  std::int32_t y{};
  friend bool operator==(const PixelPoint&, const PixelPoint&) = default;
};

struct PixelSize {
  std::int32_t width{};
  std::int32_t height{};
  friend bool operator==(const PixelSize&, const PixelSize&) = default;
};

struct PixelRect {
  std::int32_t x{};
  std::int32_t y{};
  std::int32_t width{};
  std::int32_t height{};

  [[nodiscard]] constexpr std::int32_t right() const noexcept { return x + width; }
  [[nodiscard]] constexpr std::int32_t bottom() const noexcept { return y + height; }
  [[nodiscard]] constexpr bool empty() const noexcept { return width <= 0 || height <= 0; }

  friend bool operator==(const PixelRect&, const PixelRect&) = default;
};

[[nodiscard]] inline PixelPoint clamp_point(const PixelPoint point, const PixelRect bounds) {
  return PixelPoint{
      std::clamp(point.x, bounds.x, bounds.right()),
      std::clamp(point.y, bounds.y, bounds.bottom()),
  };
}

}  // namespace hdrshot
