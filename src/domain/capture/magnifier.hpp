#pragma once
#include "core/geometry.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace hdrshot {
// All geometry is display-local physical source pixels. The destination is in
// the same coordinates, so the native surface needs no second image allocation.
struct MagnifierPreview {
  bool visible{};
  PixelPoint center{};
  PixelRect viewport{};
  int zoom{8};
  std::uint64_t revision{};
  friend bool operator==(const MagnifierPreview &,
                         const MagnifierPreview &) = default;
};
inline bool valid_magnifier_preview(const MagnifierPreview &m, PixelSize size) {
  if (!m.visible)
    return true;
  return (m.zoom == 4 || m.zoom == 8 || m.zoom == 12 || m.zoom == 16) &&
         m.viewport.x >= 0 && m.viewport.y >= 0 && !m.viewport.empty() &&
         std::int64_t(m.viewport.x) + m.viewport.width <= size.width &&
         std::int64_t(m.viewport.y) + m.viewport.height <= size.height &&
         m.viewport.width % m.zoom == 0 && m.viewport.height % m.zoom == 0 &&
         (m.viewport.width / m.zoom) % 2 == 1 &&
         (m.viewport.height / m.zoom) % 2 == 1 && m.center.x >= 0 &&
         m.center.y >= 0 && m.center.x < size.width && m.center.y < size.height;
}
inline PixelPoint magnifier_source_point(double x, double y, double scale,
                                         PixelSize size) {
  return {
      std::clamp(int(std::floor(x * scale)), 0, std::max(0, size.width - 1)),
      std::clamp(int(std::floor(y * scale)), 0, std::max(0, size.height - 1))};
}
inline int magnifier_odd_pixels(int width, int zoom) {
  return std::max(1, ((width / zoom - 1) / 2) * 2 + 1);
}
inline PixelPoint magnifier_texel(const MagnifierPreview &m,
                                  PixelPoint destination) {
  return {m.center.x + (destination.x - m.viewport.x) / m.zoom -
              m.viewport.width / m.zoom / 2,
          m.center.y + (destination.y - m.viewport.y) / m.zoom -
              m.viewport.height / m.zoom / 2};
}
} // namespace hdrshot
