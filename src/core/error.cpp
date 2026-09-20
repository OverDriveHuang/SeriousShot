#include "core/error.hpp"

namespace hdrshot {

std::string to_string(const ErrorCode code) {
  switch (code) {
    case ErrorCode::invalid_input:
      return "invalid_input";
    case ErrorCode::object_not_found:
      return "object_not_found";
    case ErrorCode::precondition_failed:
      return "precondition_failed";
    case ErrorCode::invalid_color_contract:
      return "invalid_color_contract";
    case ErrorCode::metadata_conflict:
      return "metadata_conflict";
    case ErrorCode::png_encoding_failed:
      return "png_encoding_failed";
    case ErrorCode::ultra_hdr_encoding_failed:
      return "ultra_hdr_encoding_failed";
    case ErrorCode::unsupported_encoding:
      return "unsupported_encoding";
    case ErrorCode::clipboard_rejected:
      return "clipboard_rejected";
    case ErrorCode::path_already_exists:
      return "path_already_exists";
    case ErrorCode::path_not_writable:
      return "path_not_writable";
    case ErrorCode::storage_full:
      return "storage_full";
    case ErrorCode::dialog_failed:
      return "dialog_failed";
    case ErrorCode::settings_corrupt:
      return "settings_corrupt";
    case ErrorCode::hotkey_conflict:
      return "hotkey_conflict";
    case ErrorCode::permission_denied:
      return "permission_denied";
    case ErrorCode::state_inconsistent:
      return "state_inconsistent";
    case ErrorCode::capture_failed:
      return "capture_failed";
    case ErrorCode::presenter_failed:
      return "presenter_failed";
    case ErrorCode::operation_cancelled:
      return "operation_cancelled";
    case ErrorCode::display_configuration_changed:
      return "display_configuration_changed";
    case ErrorCode::unsupported_pixel_format:
      return "unsupported_pixel_format";
    case ErrorCode::font_unavailable:
      return "font_unavailable";
  }
  return "unknown";
}

}  // namespace hdrshot
