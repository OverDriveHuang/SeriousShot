#pragma once

#include <cstdint>

namespace hdrshot {

enum class CompletionAction : std::uint8_t {
  copy_to_clipboard,
  save_default,
  save_as,
};

struct CompletionBindings {
  CompletionAction enter{CompletionAction::copy_to_clipboard};
  CompletionAction double_click{CompletionAction::copy_to_clipboard};

  friend bool operator==(const CompletionBindings&, const CompletionBindings&) = default;
};

[[nodiscard]] constexpr bool valid_completion_action(
    const CompletionAction value) noexcept {
  return value == CompletionAction::copy_to_clipboard ||
      value == CompletionAction::save_default ||
      value == CompletionAction::save_as;
}

}  // namespace hdrshot
