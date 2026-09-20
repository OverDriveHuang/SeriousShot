#include "application/input_mapper.hpp"

namespace hdrshot {
namespace {

[[nodiscard]] bool blocks_fixed_overlay_input(const FocusContext focus) {
  switch (focus) {
    case FocusContext::text_editor:
    case FocusContext::input_control:
    case FocusContext::file_dialog:
    case FocusContext::modal_control:
      return true;
    case FocusContext::overlay:
    case FocusContext::image_region:
      return false;
  }
  return true;
}

[[nodiscard]] bool no_modifiers(const KeyModifiers modifiers) {
  return !modifiers.primary && !modifiers.alternate && !modifiers.shift;
}

[[nodiscard]] UiCommand completion_command(const CompletionAction action) {
  switch (action) {
    case CompletionAction::copy_to_clipboard:
      return UiCommand::copy_and_close;
    case CompletionAction::save_default:
      return UiCommand::save_default;
    case CompletionAction::save_as:
      return UiCommand::save_as;
  }
  return UiCommand::copy_and_close;
}

}  // namespace

std::optional<UiCommand> InputMapper::map(
    const KeyEvent& event,
    const CompletionBindings bindings) {
  if (blocks_fixed_overlay_input(event.focus)) {
    return std::nullopt;
  }

  if (event.key == Key::escape && no_modifiers(event.modifiers)) {
    return UiCommand::cancel_capture;
  }
  if (event.key == Key::enter && no_modifiers(event.modifiers)) {
    return completion_command(bindings.enter);
  }

  if (event.key == Key::s && event.modifiers == KeyModifiers{true, false, false}) {
    return UiCommand::save_default;
  }
  if (event.key == Key::s && event.modifiers == KeyModifiers{true, true, false}) {
    return UiCommand::save_as;
  }
  if (event.key == Key::z && event.modifiers == KeyModifiers{true, false, false}) {
    return UiCommand::undo;
  }
  if (event.key == Key::y && event.modifiers == KeyModifiers{true, false, false}) {
    return UiCommand::redo;
  }
  return std::nullopt;
}

std::optional<UiCommand> InputMapper::map(
    const PointerGesture& gesture,
    const CompletionBindings bindings) {
  if (blocks_fixed_overlay_input(gesture.focus)) {
    return std::nullopt;
  }
  if (gesture.click_count == 2 && gesture.target == PointerTarget::image_region) {
    return completion_command(bindings.double_click);
  }
  return std::nullopt;
}

UiCommand InputMapper::map(const ButtonAction action) {
  switch (action) {
    case ButtonAction::copy:
      return UiCommand::copy_and_close;
    case ButtonAction::save_default:
      return UiCommand::save_default;
    case ButtonAction::save_as:
      return UiCommand::save_as;
    case ButtonAction::cancel:
      return UiCommand::cancel_capture;
    case ButtonAction::undo:
      return UiCommand::undo;
    case ButtonAction::redo:
      return UiCommand::redo;
  }
  return UiCommand::cancel_capture;
}

}  // namespace hdrshot
