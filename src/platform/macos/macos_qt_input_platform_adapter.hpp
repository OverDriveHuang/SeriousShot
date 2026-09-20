#pragma once

#include "ui/qt/qt_input_platform_adapter.hpp"

namespace hdrshot {

class MacQtInputPlatformAdapter final : public QtInputPlatformAdapter {
 public:
  [[nodiscard]] Result<QKeySequence, Error> display_global_hotkey(
      const std::string& canonical_hotkey) const override;
  [[nodiscard]] Result<std::string, Error> canonical_global_hotkey(
      const QKeySequence& sequence) const override;
  [[nodiscard]] QString fixed_shortcut_label(UiCommand command) const override;
  [[nodiscard]] std::optional<UiCommand> fixed_overlay_command(
      const QKeyEvent& event,
      FocusContext focus,
      CompletionBindings bindings = {}) const override;
};

}  // namespace hdrshot
