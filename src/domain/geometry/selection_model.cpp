#include "domain/geometry/selection_model.hpp"

#include <algorithm>

namespace hdrshot {

PixelRect SelectionModel::normalize_drag(
    const PixelPoint anchor,
    const PixelPoint current,
    const PixelRect desktop_bounds) {
  if (desktop_bounds.empty()) {
    return {};
  }

  const auto clamped_anchor = clamp_point(anchor, desktop_bounds);
  const auto clamped_current = clamp_point(current, desktop_bounds);
  const auto left = std::min(clamped_anchor.x, clamped_current.x);
  const auto top = std::min(clamped_anchor.y, clamped_current.y);
  const auto right = std::max(clamped_anchor.x, clamped_current.x);
  const auto bottom = std::max(clamped_anchor.y, clamped_current.y);
  return PixelRect{left, top, right - left, bottom - top};
}

SelectionSnapshot SelectionModel::update_drag(
    const SelectionSnapshot& previous,
    const PixelPoint anchor,
    const PixelPoint current,
    const PixelRect desktop_bounds) {
  const auto next_rect = normalize_drag(anchor, current, desktop_bounds);
  if (next_rect == previous.desktop_rect) {
    return previous;
  }
  return SelectionSnapshot{previous.revision + 1, next_rect};
}

SelectionSnapshot SelectionModel::adjust(
    const SelectionSnapshot& previous,
    const SelectionAdjustment adjustment,
    const PixelPoint anchor,
    const PixelPoint current,
    const PixelRect desktop_bounds,
    const std::int32_t minimum_size_px,
    const PixelRect required_contents_px) {
  if (previous.desktop_rect.empty() || desktop_bounds.empty()) {
    return previous;
  }

  const auto minimum_size = std::max(1, minimum_size_px);
  const auto delta_x = current.x - anchor.x;
  const auto delta_y = current.y - anchor.y;
  auto left = previous.desktop_rect.x;
  auto top = previous.desktop_rect.y;
  auto right = previous.desktop_rect.right();
  auto bottom = previous.desktop_rect.bottom();

  const bool adjust_left = adjustment == SelectionAdjustment::west ||
      adjustment == SelectionAdjustment::north_west ||
      adjustment == SelectionAdjustment::south_west;
  const bool adjust_right = adjustment == SelectionAdjustment::east ||
      adjustment == SelectionAdjustment::north_east ||
      adjustment == SelectionAdjustment::south_east;
  const bool adjust_top = adjustment == SelectionAdjustment::north ||
      adjustment == SelectionAdjustment::north_west ||
      adjustment == SelectionAdjustment::north_east;
  const bool adjust_bottom = adjustment == SelectionAdjustment::south ||
      adjustment == SelectionAdjustment::south_west ||
      adjustment == SelectionAdjustment::south_east;

  if (adjust_left) {
    const auto maximum_left = !required_contents_px.empty()
        ? std::min(right - minimum_size, required_contents_px.x)
        : right - minimum_size;
    left = std::clamp(previous.desktop_rect.x + delta_x, desktop_bounds.x, maximum_left);
  }
  if (adjust_right) {
    const auto minimum_right = !required_contents_px.empty()
        ? std::max(left + minimum_size, required_contents_px.right())
        : left + minimum_size;
    right = std::clamp(
        previous.desktop_rect.right() + delta_x, minimum_right, desktop_bounds.right());
  }
  if (adjust_top) {
    const auto maximum_top = !required_contents_px.empty()
        ? std::min(bottom - minimum_size, required_contents_px.y)
        : bottom - minimum_size;
    top = std::clamp(previous.desktop_rect.y + delta_y, desktop_bounds.y, maximum_top);
  }
  if (adjust_bottom) {
    const auto minimum_bottom = !required_contents_px.empty()
        ? std::max(top + minimum_size, required_contents_px.bottom())
        : top + minimum_size;
    bottom = std::clamp(
        previous.desktop_rect.bottom() + delta_y, minimum_bottom, desktop_bounds.bottom());
  }

  const auto next_rect = PixelRect{left, top, right - left, bottom - top};
  if (next_rect == previous.desktop_rect) {
    return previous;
  }
  return SelectionSnapshot{previous.revision + 1, next_rect};
}

}  // namespace hdrshot
