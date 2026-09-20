#pragma once

#include "core/geometry.hpp"
#include "core/ids.hpp"

#include <cstdint>

namespace hdrshot {

struct SelectionSnapshot {
  SelectionRevision revision{};
  PixelRect desktop_rect{};

  friend bool operator==(const SelectionSnapshot&, const SelectionSnapshot&) = default;
};

enum class SelectionAdjustment : std::uint8_t {
  north,
  north_east,
  east,
  south_east,
  south,
  south_west,
  west,
  north_west,
};

class SelectionModel {
 public:
  [[nodiscard]] static PixelRect normalize_drag(
      PixelPoint anchor,
      PixelPoint current,
      PixelRect desktop_bounds);

  [[nodiscard]] static SelectionSnapshot update_drag(
      const SelectionSnapshot& previous,
      PixelPoint anchor,
      PixelPoint current,
      PixelRect desktop_bounds);

  [[nodiscard]] static SelectionSnapshot adjust(
      const SelectionSnapshot& previous,
      SelectionAdjustment adjustment,
      PixelPoint anchor,
      PixelPoint current,
      PixelRect desktop_bounds,
      std::int32_t minimum_size_px = 2,
      PixelRect required_contents_px = {});
};

}  // namespace hdrshot
