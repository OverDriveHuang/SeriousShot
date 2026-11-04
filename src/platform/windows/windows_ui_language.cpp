#include "platform/ui_language.hpp"

#include <windows.h>

namespace hdrshot {

UiLanguage ui_language_from_windows_language_id(const std::uint16_t id) noexcept {
  return PRIMARYLANGID(id) == LANG_CHINESE ? UiLanguage::simplified_chinese : UiLanguage::english;
}

UiLanguage detect_system_ui_language() {
  return ui_language_from_windows_language_id(GetUserDefaultUILanguage());
}

}  // namespace hdrshot
