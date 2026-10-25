#pragma once

namespace hdrshot {

inline constexpr int minimum_pinned_screenshot_short_edge = 450;

struct PinnedScreenshotDefaults {
  bool always_on_top{true};
  int short_edge{500}; // Logical pixels.

  friend bool operator==(const PinnedScreenshotDefaults&, const PinnedScreenshotDefaults&) = default;
};

[[nodiscard]] constexpr bool valid_pinned_screenshot_defaults(
    const PinnedScreenshotDefaults& defaults) noexcept {
  return defaults.short_edge >= minimum_pinned_screenshot_short_edge && defaults.short_edge <= 800 &&
      defaults.short_edge % 50 == 0;
}

}  // namespace hdrshot
