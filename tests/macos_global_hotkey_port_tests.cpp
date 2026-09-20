#include "platform/macos/macos_global_hotkey_port.hpp"
#include "test_support.hpp"

#include <Carbon/Carbon.h>

#include <vector>

namespace {

void parses_default_mac_hotkey() {
  const auto parsed = hdrshot::MacGlobalHotkeyPort::parse_hotkey("Command+Shift+2");
  HDRSHOT_CHECK(parsed.has_value());
  HDRSHOT_CHECK((parsed.value().carbon_modifiers & cmdKey) != 0);
  HDRSHOT_CHECK((parsed.value().carbon_modifiers & controlKey) == 0);
}

void command_and_control_are_not_interchanged_at_native_registration_boundary() {
  const auto command = hdrshot::MacGlobalHotkeyPort::parse_hotkey("Command+Shift+2");
  const auto control = hdrshot::MacGlobalHotkeyPort::parse_hotkey("Control+Shift+2");
  HDRSHOT_CHECK(command.has_value());
  HDRSHOT_CHECK(control.has_value());
  HDRSHOT_CHECK((command.value().carbon_modifiers & cmdKey) != 0);
  HDRSHOT_CHECK((command.value().carbon_modifiers & controlKey) == 0);
  HDRSHOT_CHECK((control.value().carbon_modifiers & controlKey) != 0);
  HDRSHOT_CHECK((control.value().carbon_modifiers & cmdKey) == 0);
}

void parses_option_and_letter() {
  const auto parsed = hdrshot::MacGlobalHotkeyPort::parse_hotkey("Command+Option+S");
  HDRSHOT_CHECK(parsed.has_value());
}

void rejects_missing_modifier_and_unsupported_key() {
  const auto missing = hdrshot::MacGlobalHotkeyPort::parse_hotkey("2");
  HDRSHOT_CHECK(!missing.has_value());
  HDRSHOT_CHECK(missing.error().code == hdrshot::ErrorCode::invalid_input);

  const auto unsupported = hdrshot::MacGlobalHotkeyPort::parse_hotkey("Command+Shift+F12");
  HDRSHOT_CHECK(!unsupported.has_value());
  HDRSHOT_CHECK(unsupported.error().code == hdrshot::ErrorCode::invalid_input);
}

void rejects_duplicate_modifier_or_key() {
  HDRSHOT_CHECK(!hdrshot::MacGlobalHotkeyPort::parse_hotkey("Command+Command+2").has_value());
  HDRSHOT_CHECK(!hdrshot::MacGlobalHotkeyPort::parse_hotkey("Command+2+3").has_value());
}

}  // namespace

int main() {
  using hdrshot::test::TestCase;
  return hdrshot::test::run(std::vector<TestCase>{
      {"parse default Mac hotkey", parses_default_mac_hotkey},
      {"native Command and Control are distinct", command_and_control_are_not_interchanged_at_native_registration_boundary},
      {"parse option and letter", parses_option_and_letter},
      {"reject missing modifier and unsupported key", rejects_missing_modifier_and_unsupported_key},
      {"reject duplicate modifier or key", rejects_duplicate_modifier_or_key},
  });
}
