#include "domain/geometry/window_selection.hpp"
#include "test_support.hpp"

#include <limits>

using namespace hdrshot;
namespace {
WindowSnapshotRef windows() {
  return std::make_shared<const WindowSnapshot>(WindowSnapshot{{1}, {2}, 3, {
      {10, {1}, {10, 10, 500, 300}, 4},
      {20, {1}, {100, 100, 200, 100}, 2},
      {30, {2}, {0, 0, 600, 400}, 0},
      {40, {1}, {110, 110, 30, 30}, 1, WindowHitRole::blocker}}});
}
SelectionPointer p(int x, int y, double scale = 1) {
  return {{x, y}, x / scale, y / scale, x >= 0 && y >= 0 && x < 600 && y < 400};
}
void mapping_covers_retina_negative_origins_and_fractional_edges() {
  HDRSHOT_CHECK(map_window_bounds({-990, -490, 100, 50}, {-1000, -500, 1000, 500}, {2000, 1000}) ==
      (PixelRect{20, 20, 200, 100}));
  HDRSHOT_CHECK(map_window_bounds({0.25, 0.25, 10, 10}, {0, 0, 100, 100}, {150, 150}) ==
      (PixelRect{0, 0, 16, 16}));
  // The same physical Windows rectangle must not be divided by window DPI.
  HDRSHOT_CHECK(map_window_bounds({1920, 100, 300, 200}, {1920, 0, 2560, 1440}, {2560, 1440}) ==
      (PixelRect{0, 100, 300, 200}));
}
void mapping_clips_spanning_windows_without_cross_seam_output() {
  HDRSHOT_CHECK(map_window_bounds({900, 50, 200, 100}, {0, 0, 1000, 500}, {2000, 1000}) ==
      (PixelRect{1800, 100, 200, 200}));
  HDRSHOT_CHECK(map_window_bounds({900, 50, 200, 100}, {1000, 0, 1000, 500}, {1000, 500}) ==
      (PixelRect{0, 50, 100, 100}));
  HDRSHOT_CHECK(!map_window_bounds({1000, 0, 20, 20}, {0, 0, 1000, 500}, {1000, 500}));
  HDRSHOT_CHECK(!map_window_bounds({0, 0, std::numeric_limits<double>::infinity(), 20},
      {0, 0, 1000, 500}, {1000, 500}));
  HDRSHOT_CHECK(!map_window_bounds({0, 0, 20, 20}, {0, 0, 0, 500}, {1000, 500}));
}
void hit_test_uses_order_display_and_blocker_not_array_order() {
  auto snapshot = windows();
  HDRSHOT_CHECK(window_at(*snapshot, {1}, {200, 150}) == (PixelRect{100, 100, 200, 100}));
  HDRSHOT_CHECK(!window_at(*snapshot, {1}, {120, 120}));
  HDRSHOT_CHECK(window_at(*snapshot, {1}, {300, 150}) == (PixelRect{10, 10, 500, 300}));
  HDRSHOT_CHECK(window_at(*snapshot, {2}, {200, 150}) == (PixelRect{0, 0, 600, 400}));
  HDRSHOT_CHECK(!window_at(*snapshot, {1}, {550, 350}));
}
void hover_click_jitter_and_frozen_candidate() {
  WindowSelectionGesture gesture(windows(), {1}, {600, 400});
  gesture.move(p(50, 50));
  HDRSHOT_CHECK(gesture.preview().kind == PreviewHighlightKind::window_candidate);
  const auto revision = gesture.preview().revision;
  gesture.move(p(51, 50));
  HDRSHOT_CHECK(gesture.preview().revision == revision);
  HDRSHOT_CHECK(!gesture.confirmed());
  gesture.press(p(99, 150));
  gesture.move(p(103, 150)); // Pointer crosses into a different window below threshold.
  HDRSHOT_CHECK(!gesture.confirmed());
  HDRSHOT_CHECK(gesture.release(p(103, 150)) == (PixelRect{10, 10, 500, 300}));
  HDRSHOT_CHECK(gesture.confirmed() && gesture.preview().rect.empty());
  gesture.move(p(200, 150));
  HDRSHOT_CHECK(gesture.preview().rect.empty());
}
void drag_threshold_is_logical_and_release_checks_missing_moves() {
  WindowSelectionGesture gesture(windows(), {1}, {600, 400});
  gesture.press(p(50, 50, 2));
  gesture.move(p(64, 50, 2));
  HDRSHOT_CHECK(gesture.preview().kind == PreviewHighlightKind::window_candidate);
  gesture.move(p(66, 50, 2));
  HDRSHOT_CHECK(gesture.preview().kind == PreviewHighlightKind::none); // zero-height manual
  HDRSHOT_CHECK(gesture.release(p(66, 80, 2)) == (PixelRect{50, 50, 16, 30}));
  WindowSelectionGesture coalesced(windows(), {1}, {600, 400});
  coalesced.press(p(50, 50));
  HDRSHOT_CHECK(coalesced.release(p(150, 120)) == (PixelRect{50, 50, 100, 70}));
}
void crossed_threshold_stays_manual_and_invalid_drag_recovers() {
  WindowSelectionGesture gesture(windows(), {1}, {600, 400});
  gesture.press(p(50, 50));
  gesture.move(p(100, 100));
  HDRSHOT_CHECK(gesture.release(p(52, 53)) == (PixelRect{50, 50, 2, 3}));
  WindowSelectionGesture empty(windows(), {1}, {600, 400});
  empty.press(p(50, 50)); empty.move(p(100, 100));
  HDRSHOT_CHECK(!empty.release(p(50, 50)));
  HDRSHOT_CHECK(!empty.confirmed() && !empty.pressed());
  empty.press(p(50, 50));
  HDRSHOT_CHECK(empty.release(p(50, 50)).has_value());
}
void four_directions_clip_and_manual_fallback() {
  for (auto end : {PixelPoint{100, 100}, PixelPoint{500, 100},
                   PixelPoint{100, 350}, PixelPoint{500, 350}, PixelPoint{-50, 500}}) {
    WindowSelectionGesture gesture({}, {1}, {600, 400});
    gesture.press(p(300, 200));
    HDRSHOT_CHECK(gesture.release(p(end.x, end.y)) ==
        SelectionModel::normalize_drag({300, 200}, end, {0, 0, 600, 400}));
  }
  WindowSelectionGesture no_window({}, {1}, {600, 400});
  no_window.press(p(50, 50));
  HDRSHOT_CHECK(!no_window.release(p(50, 50)));
  HDRSHOT_CHECK(!no_window.confirmed());
}
void leave_and_cancel_clear_candidate_without_spurious_confirmation() {
  WindowSelectionGesture gesture(windows(), {1}, {600, 400});
  gesture.move(p(50, 50)); gesture.leave();
  HDRSHOT_CHECK(gesture.preview().rect.empty());
  gesture.press(p(50, 50)); gesture.move(p(100, 100)); gesture.leave();
  HDRSHOT_CHECK(gesture.pressed());
  gesture.cancel();
  HDRSHOT_CHECK(!gesture.pressed() && gesture.preview().rect.empty());
  HDRSHOT_CHECK(!gesture.release(p(100, 100)));
  gesture.press(p(50, 50));
  HDRSHOT_CHECK(gesture.release(p(50, 50)).has_value());
}
} // namespace
int main() {
  return hdrshot::test::run({
      {"coordinate origins, Retina and fractional edges", mapping_covers_retina_negative_origins_and_fractional_edges},
      {"clipped fragments and invalid geometry", mapping_clips_spanning_windows_without_cross_seam_output},
      {"frontmost eligible rectangle and blockers", hit_test_uses_order_display_and_blocker_not_array_order},
      {"hover is not a selection; press freezes candidate", hover_click_jitter_and_frozen_candidate},
      {"logical threshold and coalesced release", drag_threshold_is_logical_and_release_checks_missing_moves},
      {"manual intent is latched; invalid drag recovers", crossed_threshold_stays_manual_and_invalid_drag_recovers},
      {"four directions, clipping and no-window fallback", four_directions_clip_and_manual_fallback},
      {"leave and cancellation", leave_and_cancel_clear_candidate_without_spurious_confirmation}});
}
