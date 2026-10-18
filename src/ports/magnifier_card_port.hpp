#pragma once
#include "ports/capture_preview_ports.hpp"

namespace hdrshot {
// Complete immutable card contents. Desktop position belongs exclusively to
// the GUI window port and is deliberately absent from this request.
struct MagnifierCardFrame {
  SessionId session_id{};
  FrozenDesktopRef source;
  DisplayId display_id{};
  DisplayGeneration display_generation{};
  std::uint64_t generation{}, revision{};
  PixelSize size_px{};
  MagnifierPreview image;
  std::vector<std::uint8_t> ui_rgba; // straight-alpha, encoded sRGB RGBA8
};
class MagnifierCardPresenterPort {
 public:
  using Completion = std::function<void(Result<std::uint64_t, Error>)>;
  virtual ~MagnifierCardPresenterPort() = default;
  virtual void present_card(MagnifierCardFrame frame, Completion completion) = 0;
  // Invalidates queued content without touching window geometry or waiting.
  virtual void invalidate_card(std::uint64_t generation) = 0;
};
} // namespace hdrshot
