#include "platform/macos/macos_qt_input_platform_adapter.hpp"

#include <QKeyCombination>
#include <QKeyEvent>

#include <cctype>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace hdrshot {
namespace {

Error codec_error(std::string reason) {
  return Error{
      ErrorCode::invalid_input,
      "MacQtInputPlatformAdapter",
      Retryability::after_user_action,
      {{"reason", std::move(reason)}},
  };
}

std::vector<std::string> tokens(const std::string& value) {
  std::vector<std::string> result;
  std::stringstream stream(value);
  std::string token;
  while (std::getline(stream, token, '+')) {
    if (!token.empty()) {
      result.push_back(std::move(token));
    }
  }
  return result;
}

}  // namespace

Result<QKeySequence, Error> MacQtInputPlatformAdapter::display_global_hotkey(
    const std::string& canonical_hotkey) const {
  Qt::KeyboardModifiers modifiers{};
  Qt::Key key{Qt::Key_unknown};
  for (const auto& token : tokens(canonical_hotkey)) {
    // Qt deliberately maps the physical macOS Command key to ControlModifier
    // and the physical Control key to MetaModifier.
    if (token == "Command") {
      modifiers |= Qt::ControlModifier;
    } else if (token == "Control") {
      modifiers |= Qt::MetaModifier;
    } else if (token == "Option") {
      modifiers |= Qt::AltModifier;
    } else if (token == "Shift") {
      modifiers |= Qt::ShiftModifier;
    } else if (token.size() == 1 && key == Qt::Key_unknown) {
      const auto character = static_cast<char>(
          std::toupper(static_cast<unsigned char>(token.front())));
      if (character >= 'A' && character <= 'Z') {
        key = static_cast<Qt::Key>(Qt::Key_A + (character - 'A'));
      } else if (character >= '0' && character <= '9') {
        key = static_cast<Qt::Key>(Qt::Key_0 + (character - '0'));
      } else {
        return Result<QKeySequence, Error>::failure(codec_error("unsupported_key"));
      }
    } else {
      return Result<QKeySequence, Error>::failure(codec_error("unsupported_token"));
    }
  }
  if (modifiers == Qt::NoModifier || key == Qt::Key_unknown) {
    return Result<QKeySequence, Error>::failure(codec_error("modifier_and_key_required"));
  }
  return Result<QKeySequence, Error>::success(
      QKeySequence(QKeyCombination(modifiers, key)));
}

Result<std::string, Error> MacQtInputPlatformAdapter::canonical_global_hotkey(
    const QKeySequence& sequence) const {
  if (sequence.isEmpty()) {
    return Result<std::string, Error>::failure(codec_error("empty"));
  }
  const auto combination = sequence[0];
  const auto modifiers = combination.keyboardModifiers();
  std::string value;
  const auto append = [&value](const char* token) {
    if (!value.empty()) {
      value += '+';
    }
    value += token;
  };
  if ((modifiers & Qt::ControlModifier) != 0) append("Command");
  if ((modifiers & Qt::MetaModifier) != 0) append("Control");
  if ((modifiers & Qt::AltModifier) != 0) append("Option");
  if ((modifiers & Qt::ShiftModifier) != 0) append("Shift");
  if (value.empty()) {
    return Result<std::string, Error>::failure(codec_error("modifier_required"));
  }
  const auto key = combination.key();
  char character{};
  if (key >= Qt::Key_A && key <= Qt::Key_Z) {
    character = static_cast<char>('A' + (key - Qt::Key_A));
  } else if (key >= Qt::Key_0 && key <= Qt::Key_9) {
    character = static_cast<char>('0' + (key - Qt::Key_0));
  } else {
    return Result<std::string, Error>::failure(codec_error("unsupported_key"));
  }
  value += '+';
  value += character;
  return Result<std::string, Error>::success(std::move(value));
}

QString MacQtInputPlatformAdapter::fixed_shortcut_label(const UiCommand command) const {
  switch (command) {
    case UiCommand::cancel_capture: return QStringLiteral("Esc");
    case UiCommand::copy_and_close: return QStringLiteral("Return / 双击选区");
    case UiCommand::save_default: return QStringLiteral("⌘ S");
    case UiCommand::save_as: return QStringLiteral("⌥ ⌘ S");
    case UiCommand::undo: return QStringLiteral("⌘ Z");
    case UiCommand::redo: return QStringLiteral("⌘ Y");
    default: return {};
  }
}

std::optional<UiCommand> MacQtInputPlatformAdapter::fixed_overlay_command(
    const QKeyEvent& event,
    const FocusContext focus,
    const CompletionBindings bindings) const {
  Key key{Key::unknown};
  switch (event.key()) {
    case Qt::Key_Escape: key = Key::escape; break;
    case Qt::Key_Return:
    case Qt::Key_Enter: key = Key::enter; break;
    case Qt::Key_S: key = Key::s; break;
    case Qt::Key_Y: key = Key::y; break;
    case Qt::Key_Z: key = Key::z; break;
    default: break;
  }
  const auto modifiers = event.modifiers();
  return InputMapper::map(KeyEvent{
      focus,
      key,
      KeyModifiers{
          (modifiers & Qt::ControlModifier) != 0,
          (modifiers & Qt::AltModifier) != 0,
          (modifiers & Qt::ShiftModifier) != 0,
      },
  }, bindings);
}

}  // namespace hdrshot
