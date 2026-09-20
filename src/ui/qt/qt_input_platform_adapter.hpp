#pragma once

#include "application/input_mapper.hpp"
#include "core/error.hpp"
#include "core/result.hpp"

#include <QKeySequence>
#include <QString>

#include <optional>
#include <string>

class QKeyEvent;

namespace hdrshot {

// Qt reports modifier keys using platform-specific semantics (on macOS the
// physical Command key is Qt::ControlModifier). Shared Qt views must not know
// those rules, so each platform composition root injects one adapter. The
// adapter normalizes physical keys; InputMapper contains no OS branch.
class QtInputPlatformAdapter {
 public:
  virtual ~QtInputPlatformAdapter() = default;

  [[nodiscard]] virtual Result<QKeySequence, Error> display_global_hotkey(
      const std::string& canonical_hotkey) const = 0;
  [[nodiscard]] virtual Result<std::string, Error> canonical_global_hotkey(
      const QKeySequence& sequence) const = 0;
  [[nodiscard]] virtual QString fixed_shortcut_label(UiCommand command) const = 0;
  [[nodiscard]] virtual std::optional<UiCommand> fixed_overlay_command(
      const QKeyEvent& event,
      FocusContext focus,
      CompletionBindings bindings = {}) const = 0;
};

}  // namespace hdrshot
