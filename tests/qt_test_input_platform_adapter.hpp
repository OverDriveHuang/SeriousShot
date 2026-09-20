#pragma once
#include "ui/qt/qt_input_platform_adapter.hpp"
#include <QKeyEvent>
namespace hdrshot::test {
// Deliberately fixed test mapping, not a production Windows/Mac executor.
class TestInputPlatformAdapter final : public QtInputPlatformAdapter {
 public:
  Result<QKeySequence, Error> display_global_hotkey(const std::string& text) const override {
    auto keys = QString::fromStdString(text);
    keys.replace("Command", "Ctrl").replace("Option", "Alt");
    return Result<QKeySequence, Error>::success(QKeySequence::fromString(keys, QKeySequence::PortableText));
  }
  Result<std::string, Error> canonical_global_hotkey(const QKeySequence& value) const override {
    auto text = value.toString(QKeySequence::PortableText);
    text.replace("Ctrl", "Command").replace("Alt", "Option");
    return Result<std::string, Error>::success(text.toStdString());
  }
  QString fixed_shortcut_label(UiCommand) const override { return QStringLiteral("Test shortcut"); }
  std::optional<UiCommand> fixed_overlay_command(
      const QKeyEvent& event, FocusContext focus, CompletionBindings bindings = {}) const override {
    Key key{Key::unknown};
    if (event.key() == Qt::Key_Escape) key = Key::escape;
    if (event.key() == Qt::Key_Return || event.key() == Qt::Key_Enter) key = Key::enter;
    if (event.key() == Qt::Key_S) key = Key::s;
    if (event.key() == Qt::Key_Y) key = Key::y;
    if (event.key() == Qt::Key_Z) key = Key::z;
    const auto modifiers = event.modifiers();
    return InputMapper::map(KeyEvent{focus, key, KeyModifiers{
        (modifiers & Qt::ControlModifier) != 0, (modifiers & Qt::AltModifier) != 0,
        (modifiers & Qt::ShiftModifier) != 0}}, bindings);
  }
};
} // namespace hdrshot::test
