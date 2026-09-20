#include "application/input_mapper.hpp"
#include "test_support.hpp"

#include <optional>
#include <vector>

namespace {

using hdrshot::ButtonAction;
using hdrshot::CompletionAction;
using hdrshot::CompletionBindings;
using hdrshot::FocusContext;
using hdrshot::InputMapper;
using hdrshot::Key;
using hdrshot::KeyEvent;
using hdrshot::KeyModifiers;
using hdrshot::PointerGesture;
using hdrshot::PointerTarget;
using hdrshot::UiCommand;

void check_key(
    const Key key,
    const KeyModifiers modifiers,
    const UiCommand expected) {
  const auto actual = InputMapper::map(KeyEvent{FocusContext::overlay, key, modifiers});
  HDRSHOT_CHECK(actual.has_value());
  HDRSHOT_CHECK(actual.value() == expected);
}

void normalized_fixed_keys() {
  check_key(Key::escape, {}, UiCommand::cancel_capture);
  check_key(Key::enter, {}, UiCommand::copy_and_close);
  check_key(Key::s, {true, false, false}, UiCommand::save_default);
  check_key(Key::s, {true, true, false}, UiCommand::save_as);
  check_key(Key::z, {true, false, false}, UiCommand::undo);
  check_key(Key::y, {true, false, false}, UiCommand::redo);
}

void focus_blocks_fixed_input() {
  for (const auto focus : {
           FocusContext::text_editor,
           FocusContext::input_control,
           FocusContext::file_dialog,
           FocusContext::modal_control,
       }) {
    const auto key = InputMapper::map(
        KeyEvent{focus, Key::enter, KeyModifiers{}});
    const auto pointer = InputMapper::map(
        PointerGesture{focus, PointerTarget::image_region, 2});
    HDRSHOT_CHECK(!key.has_value());
    HDRSHOT_CHECK(!pointer.has_value());
  }
}

void double_click_and_button_share_copy_command() {
  const auto pointer = InputMapper::map(
      PointerGesture{FocusContext::image_region, PointerTarget::image_region, 2});
  HDRSHOT_CHECK(pointer.has_value());
  HDRSHOT_CHECK(pointer.value() == UiCommand::copy_and_close);
  HDRSHOT_CHECK(InputMapper::map(ButtonAction::copy) == UiCommand::copy_and_close);
}

void configurable_completion_actions_are_independent() {
  const CompletionBindings bindings{
      CompletionAction::save_default,
      CompletionAction::save_as,
  };
  const auto enter = InputMapper::map(
      KeyEvent{FocusContext::overlay, Key::enter, {}}, bindings);
  const auto double_click = InputMapper::map(
      PointerGesture{FocusContext::image_region, PointerTarget::image_region, 2},
      bindings);
  HDRSHOT_CHECK(enter == UiCommand::save_default);
  HDRSHOT_CHECK(double_click == UiCommand::save_as);
  HDRSHOT_CHECK(InputMapper::map(ButtonAction::copy) == UiCommand::copy_and_close);
}

void unrelated_input_is_ignored() {
  HDRSHOT_CHECK(!InputMapper::map(
                     KeyEvent{FocusContext::overlay, Key::unknown, {}})
                     .has_value());
  HDRSHOT_CHECK(!InputMapper::map(
                     PointerGesture{FocusContext::image_region, PointerTarget::image_region, 1})
                     .has_value());
  HDRSHOT_CHECK(!InputMapper::map(
                     PointerGesture{FocusContext::image_region, PointerTarget::button, 2})
                     .has_value());
}

}  // namespace

int main() {
  return hdrshot::test::run({
      {"U1-KEY-001 normalized fixed keys", normalized_fixed_keys},
      {"U1-FOCUS-001 focus blocks fixed input", focus_blocks_fixed_input},
      {"U1-POINTER-001 double click and button share copy", double_click_and_button_share_copy_command},
      {"U1-COMPLETE-001 completion bindings are independent", configurable_completion_actions_are_independent},
      {"U1-NEG-001 unrelated input ignored", unrelated_input_is_ignored},
  });
}
