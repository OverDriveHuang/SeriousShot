#pragma once

#include "ports/export_ports.hpp"

#include <cstdint>
#include <string_view>

namespace hdrshot {

[[nodiscard]] UiLanguage detect_system_ui_language();

[[nodiscard]] constexpr UiLanguage ui_language_from_apple_language_tag(
    const std::string_view tag) noexcept {
  const auto primary = tag.substr(0, tag.find_first_of("-_"));
  if (primary.size() == 2 && (primary[0] == 'z' || primary[0] == 'Z') &&
      (primary[1] == 'h' || primary[1] == 'H')) {
    return UiLanguage::simplified_chinese;
  }
  return UiLanguage::english;
}

#ifdef _WIN32
[[nodiscard]] UiLanguage ui_language_from_windows_language_id(std::uint16_t id) noexcept;
#endif

}  // namespace hdrshot
