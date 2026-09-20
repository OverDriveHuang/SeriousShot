#include "platform/macos/macos_qt_input_platform_adapter.hpp"
#include "test_support.hpp"

#include <QCoreApplication>
#include <QKeyCombination>
#include <QKeyEvent>

namespace {

void command_and_control_follow_qt_macos_physical_modifier_contract() {
  hdrshot::MacQtInputPlatformAdapter adapter;
  const auto command = adapter.display_global_hotkey("Command+Shift+2");
  HDRSHOT_CHECK(command.has_value());
  HDRSHOT_CHECK(
      (command.value()[0].keyboardModifiers() & Qt::ControlModifier) != 0);
  HDRSHOT_CHECK(
      (command.value()[0].keyboardModifiers() & Qt::MetaModifier) == 0);
  HDRSHOT_CHECK(adapter.canonical_global_hotkey(command.value()).value() ==
      "Command+Shift+2");

  const auto control = adapter.display_global_hotkey("Control+Shift+2");
  HDRSHOT_CHECK(control.has_value());
  HDRSHOT_CHECK(
      (control.value()[0].keyboardModifiers() & Qt::MetaModifier) != 0);
  HDRSHOT_CHECK(
      (control.value()[0].keyboardModifiers() & Qt::ControlModifier) == 0);
  HDRSHOT_CHECK(adapter.canonical_global_hotkey(control.value()).value() ==
      "Control+Shift+2");
}

void physical_command_recording_round_trips_to_native_command_token() {
  hdrshot::MacQtInputPlatformAdapter adapter;
  const QKeySequence physical_command(QKeyCombination(
      Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_2));
  const auto canonical = adapter.canonical_global_hotkey(physical_command);
  HDRSHOT_CHECK(canonical.has_value());
  HDRSHOT_CHECK(canonical.value() == "Command+Shift+2");
}

void physical_macos_keys_normalize_before_domain_mapping() {
  hdrshot::MacQtInputPlatformAdapter adapter;
  HDRSHOT_CHECK(adapter.fixed_shortcut_label(hdrshot::UiCommand::save_as) ==
      QStringLiteral("⌥ ⌘ S"));
  HDRSHOT_CHECK(adapter.fixed_shortcut_label(hdrshot::UiCommand::redo) ==
      QStringLiteral("⌘ Y"));
  QKeyEvent save_as(
      QEvent::KeyPress,
      Qt::Key_S,
      Qt::ControlModifier | Qt::AltModifier);
  HDRSHOT_CHECK(adapter.fixed_overlay_command(
      save_as, hdrshot::FocusContext::overlay) == hdrshot::UiCommand::save_as);

  QKeyEvent redo(QEvent::KeyPress, Qt::Key_Y, Qt::ControlModifier);
  HDRSHOT_CHECK(adapter.fixed_overlay_command(
      redo, hdrshot::FocusContext::overlay) == hdrshot::UiCommand::redo);

  QKeyEvent blocked_enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
  HDRSHOT_CHECK(!adapter.fixed_overlay_command(
      blocked_enter, hdrshot::FocusContext::text_editor).has_value());
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  return hdrshot::test::run({
      {"macOS Command and Control mapping", command_and_control_follow_qt_macos_physical_modifier_contract},
      {"physical Command recording round trip", physical_command_recording_round_trips_to_native_command_token},
      {"macOS physical overlay shortcuts", physical_macos_keys_normalize_before_domain_mapping},
  });
}
