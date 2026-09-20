#pragma once

#include "domain/input/completion_action.hpp"

#include <cstdint>
#include <optional>

namespace hdrshot {

enum class FocusContext : std::uint8_t {
  overlay,
  image_region,
  text_editor,
  input_control,
  file_dialog,
  modal_control,
};

enum class Key : std::uint8_t { escape, enter, s, z, y, unknown };

struct KeyModifiers {
  bool primary{};
  bool alternate{};
  bool shift{};

  friend bool operator==(const KeyModifiers&, const KeyModifiers&) = default;
};

struct KeyEvent {
  FocusContext focus{FocusContext::overlay};
  Key key{Key::unknown};
  KeyModifiers modifiers{};
};

enum class PointerTarget : std::uint8_t { image_region, text_editor, button, menu, other };

struct PointerGesture {
  FocusContext focus{FocusContext::overlay};
  PointerTarget target{PointerTarget::other};
  std::uint8_t click_count{};
};

enum class ButtonAction : std::uint8_t { copy, save_default, save_as, cancel, undo, redo };

enum class UiCommand : std::uint8_t {
  cancel_capture,
  copy_and_close,
  save_default,
  save_as,
  undo,
  redo,
  analyze,
};

class InputMapper {
 public:
  [[nodiscard]] static std::optional<UiCommand> map(
      const KeyEvent& event,
      CompletionBindings bindings = {});
  [[nodiscard]] static std::optional<UiCommand> map(
      const PointerGesture& gesture,
      CompletionBindings bindings = {});
  [[nodiscard]] static UiCommand map(ButtonAction action);
};

}  // namespace hdrshot
