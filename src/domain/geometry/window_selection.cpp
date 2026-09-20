#include "domain/geometry/window_selection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hdrshot {
namespace {
bool valid_rect(WindowRectF r) {
  return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) &&
      std::isfinite(r.height) && std::isfinite(r.x + r.width) &&
      std::isfinite(r.y + r.height) && r.width > 0 && r.height > 0;
}
bool usable(PixelRect r) { return r.width >= 2 && r.height >= 2; }
bool finite_pointer(SelectionPointer p) {
  return std::isfinite(p.logical_x) && std::isfinite(p.logical_y);
}
} // namespace

std::optional<PixelRect> map_window_bounds(
    WindowRectF window, WindowRectF display, PixelSize size) {
  if (!valid_rect(window) || !valid_rect(display) || size.width <= 0 || size.height <= 0)
    return std::nullopt;
  const double left = std::max(window.x, display.x), top = std::max(window.y, display.y);
  const double right = std::min(window.x + window.width, display.x + display.width);
  const double bottom = std::min(window.y + window.height, display.y + display.height);
  if (right <= left || bottom <= top) return std::nullopt;
  const auto edge = [](double point, double origin, double extent, int pixels, bool upper) {
    const double value = std::clamp((point - origin) / extent, 0.0, 1.0) * pixels;
    return static_cast<std::int32_t>(upper ? std::ceil(value) : std::floor(value));
  };
  const auto x = edge(left, display.x, display.width, size.width, false);
  const auto y = edge(top, display.y, display.height, size.height, false);
  const auto x2 = edge(right, display.x, display.width, size.width, true);
  const auto y2 = edge(bottom, display.y, display.height, size.height, true);
  const PixelRect result{x, y, x2 - x, y2 - y};
  return result.empty() ? std::nullopt : std::optional{result};
}

std::optional<PixelRect> window_at(
    const WindowSnapshot& snapshot, DisplayId display, PixelPoint point) {
  const WindowCandidate* front = nullptr;
  for (const auto& window : snapshot.candidates) {
    const auto r = window.bounds_px;
    if (window.token == 0 || window.display_id != display || !usable(r) || r.x < 0 || r.y < 0)
      continue;
    // Wider arithmetic also makes malformed adapter records harmless here.
    if (point.x < r.x || point.y < r.y ||
        point.x >= static_cast<std::int64_t>(r.x) + r.width ||
        point.y >= static_cast<std::int64_t>(r.y) + r.height) continue;
    if (!front || window.front_to_back_order < front->front_to_back_order) front = &window;
  }
  if (!front || front->role == WindowHitRole::blocker) return std::nullopt;
  return front->bounds_px;
}

WindowSelectionGesture::WindowSelectionGesture(
    WindowSnapshotRef windows, DisplayId display, PixelSize size)
    : windows_(std::move(windows)), display_(display), bounds_{0, 0, size.width, size.height} {}

void WindowSelectionGesture::set_preview(PreviewHighlightKind kind, PixelRect rect) {
  if (rect.empty()) { kind = PreviewHighlightKind::none; rect = {}; }
  if (kind != preview_.kind || rect != preview_.rect)
    preview_ = {kind, rect, preview_.revision + 1};
}

void WindowSelectionGesture::move(SelectionPointer pointer) {
  if (confirmed_ || !finite_pointer(pointer)) return;
  if (pressed_) {
    if (std::hypot(pointer.logical_x - anchor_.logical_x,
                   pointer.logical_y - anchor_.logical_y) >= drag_threshold_logical_px)
      manual_ = true;
    if (manual_) set_preview(PreviewHighlightKind::manual_drag,
        SelectionModel::normalize_drag(anchor_.pixel, pointer.pixel, bounds_));
    return;
  }
  const auto candidate = pointer.inside && windows_ ? window_at(*windows_, display_, pointer.pixel)
                                                   : std::nullopt;
  set_preview(PreviewHighlightKind::window_candidate, candidate.value_or(PixelRect{}));
}

void WindowSelectionGesture::press(SelectionPointer pointer) {
  if (confirmed_ || pressed_ || !pointer.inside || !finite_pointer(pointer)) return;
  move(pointer);
  anchor_ = pointer;
  pressed_candidate_ = preview_.rect.empty() ? std::nullopt : std::optional{preview_.rect};
  pressed_ = true;
  manual_ = false;
}

std::optional<PixelRect> WindowSelectionGesture::release(SelectionPointer pointer) {
  if (!pressed_ || confirmed_) return std::nullopt;
  if (!finite_pointer(pointer)) { cancel(); return std::nullopt; }
  move(pointer); // Release may arrive without any intervening move events.
  auto selected = manual_ ? std::optional{preview_.rect} : pressed_candidate_;
  pressed_ = false;
  manual_ = false;
  pressed_candidate_.reset();
  set_preview(PreviewHighlightKind::none, {});
  if (selected && usable(*selected)) {
    confirmed_ = true;
    return selected;
  }
  move(pointer);
  return std::nullopt;
}

void WindowSelectionGesture::leave() {
  if (!pressed_) set_preview(PreviewHighlightKind::none, {});
}
void WindowSelectionGesture::cancel() {
  pressed_ = manual_ = false;
  pressed_candidate_.reset();
  set_preview(PreviewHighlightKind::none, {});
}
} // namespace hdrshot
