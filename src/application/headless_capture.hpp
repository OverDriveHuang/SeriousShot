#pragma once

#include "application/export_workflow.hpp"
#include "ports/capture_preview_ports.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace hdrshot {

enum class CaptureCommandMode { gui, help, list_displays, capture };

struct CaptureCommand {
  CaptureCommandMode mode{CaptureCommandMode::gui};
  std::optional<DisplayId> display_id;
  std::optional<LogicalRect> desktop_rect_points;
};

// argv excludes the executable name. Parsing/geometry are shared by both OSes.
[[nodiscard]] Result<CaptureCommand, Error> parse_capture_command(
    std::span<const std::string_view> arguments);
[[nodiscard]] std::string_view capture_command_help();

struct HeadlessCaptureTarget {
  DisplaySnapshot display;
  PixelRect selection_px;
};

[[nodiscard]] Result<HeadlessCaptureTarget, Error> resolve_capture_target(
    const CaptureCommand& command, DisplayId primary_display,
    const DisplaySnapshotSet& displays);

// No presenter/window or platform-specific color math. Completion freezes the
// exact settings and pixels; the caller supplies existing export executors.
class HeadlessCapture {
 public:
  using Completion = std::function<void(Result<ExportSnapshot, Error>)>;
  HeadlessCapture(std::shared_ptr<DisplayCatalogPort> catalog,
                  std::shared_ptr<CapturePort> capture);
  ~HeadlessCapture();
  HeadlessCapture(const HeadlessCapture&) = delete;
  HeadlessCapture& operator=(const HeadlessCapture&) = delete;
  void begin(CaptureCommand command, DisplayId primary_display,
             SettingsSnapshot settings, Completion completion);
  void cancel();

 private:
  struct State;
  std::shared_ptr<State> state_;
};

}  // namespace hdrshot
