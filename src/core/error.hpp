#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <source_location>

namespace hdrshot {

enum class ErrorCode : std::uint16_t {
  invalid_input,
  object_not_found,
  precondition_failed,
  invalid_color_contract,
  metadata_conflict,
  png_encoding_failed,
  ultra_hdr_encoding_failed,
  unsupported_encoding,
  clipboard_rejected,
  path_already_exists,
  path_not_writable,
  storage_full,
  dialog_failed,
  settings_corrupt,
  hotkey_conflict,
  permission_denied,
  state_inconsistent,
  capture_failed,
  presenter_failed,
  operation_cancelled,
  display_configuration_changed,
  unsupported_pixel_format,
  font_unavailable,
};

enum class Retryability : std::uint8_t {
  never,
  same_input,
  after_user_action,
  after_recreate,
};

struct Error {
  ErrorCode code{};
  std::string module;
  Retryability retryability{Retryability::never};
  std::map<std::string, std::string> safe_context;
  std::source_location origin = std::source_location::current();

  // Origin is diagnostic provenance, not part of the stable error identity.
  friend bool operator==(const Error& a, const Error& b) {
    return a.code == b.code && a.module == b.module &&
        a.retryability == b.retryability && a.safe_context == b.safe_context;
  }
};

[[nodiscard]] std::string to_string(ErrorCode code);

}  // namespace hdrshot
