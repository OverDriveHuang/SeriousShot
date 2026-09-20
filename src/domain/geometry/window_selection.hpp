#pragma once

#include "core/frame.hpp"
#include "domain/geometry/selection_model.hpp"

#include <memory>
#include <optional>

namespace hdrshot {

// Native executors describe geometry; they never decide selection gestures.
struct WindowRectF { double x{}, y{}, width{}, height{}; };
enum class WindowHitRole : std::uint8_t { selectable, blocker };
struct WindowCandidate {
  std::uint64_t token{};
  DisplayId display_id{};
  PixelRect bounds_px{};
  std::uint32_t front_to_back_order{};
  WindowHitRole role{WindowHitRole::selectable};
};
struct WindowSnapshot {
  SessionId session_id{};
  OperationId operation_id{};
  DisplayGeneration display_generation{};
  std::vector<WindowCandidate> candidates;
};
using WindowSnapshotRef = std::shared_ptr<const WindowSnapshot>;

// Both native rectangles MUST use the same origin and units. Output is a
// half-open, clipped ROI in this display's immutable capture pixels.
[[nodiscard]] std::optional<PixelRect> map_window_bounds(
    WindowRectF window, WindowRectF display, PixelSize capture_size);
[[nodiscard]] std::optional<PixelRect> window_at(
    const WindowSnapshot& snapshot, DisplayId display, PixelPoint point);

enum class PreviewHighlightKind : std::uint8_t { none, window_candidate, manual_drag };
struct PreviewHighlight {
  PreviewHighlightKind kind{PreviewHighlightKind::none};
  PixelRect rect{};
  std::uint64_t revision{};
  friend bool operator==(const PreviewHighlight&, const PreviewHighlight&) = default;
};
struct SelectionPointer {
  PixelPoint pixel{};
  double logical_x{}, logical_y{};
  bool inside{true};
};

// One initial selection gesture. No Qt, OS calls, color processing or exports.
class WindowSelectionGesture {
 public:
  static constexpr double drag_threshold_logical_px = 8.0;
  WindowSelectionGesture(WindowSnapshotRef windows, DisplayId display, PixelSize size);
  void move(SelectionPointer pointer);
  void press(SelectionPointer pointer);
  [[nodiscard]] std::optional<PixelRect> release(SelectionPointer pointer);
  void leave();
  void cancel();
  void reject_confirmation() { confirmed_ = false; cancel(); }
  [[nodiscard]] bool pressed() const { return pressed_; }
  [[nodiscard]] bool confirmed() const { return confirmed_; }
  [[nodiscard]] const PreviewHighlight& preview() const { return preview_; }

 private:
  void set_preview(PreviewHighlightKind kind, PixelRect rect);
  WindowSnapshotRef windows_;
  DisplayId display_{};
  PixelRect bounds_{};
  SelectionPointer anchor_{};
  std::optional<PixelRect> pressed_candidate_;
  bool pressed_{}, manual_{}, confirmed_{};
  PreviewHighlight preview_{};
};
} // namespace hdrshot
