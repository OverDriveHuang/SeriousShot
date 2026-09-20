#pragma once

#include "core/error.hpp"
#include "core/result.hpp"
#include "ports/export_ports.hpp"

#include <string>

namespace hdrshot {

struct SettingsUpdateReceipt {
  std::uint64_t settings_revision{};

  friend bool operator==(const SettingsUpdateReceipt&, const SettingsUpdateReceipt&) = default;
};

class SettingsWorkflow {
 public:
  [[nodiscard]] static Result<SettingsUpdateReceipt, Error> change_detailed_logging(
      bool enabled, SettingsStorePort& settings_store);
  [[nodiscard]] static Result<SettingsUpdateReceipt, Error> change_default_folder(
      std::string folder,
      SettingsStorePort& settings_store);

  [[nodiscard]] static Result<SettingsUpdateReceipt, Error> change_save_format(
      SaveFormat save_format,
      SettingsStorePort& settings_store);

  [[nodiscard]] static Result<SettingsUpdateReceipt, Error> change_pq_diffuse_white(
      PqDiffuseWhite pq_diffuse_white,
      SettingsStorePort& settings_store);

  [[nodiscard]] static Result<SettingsUpdateReceipt, Error> change_hdr_pq_precision(
      HdrPqPrecision hdr_pq_precision,
      SettingsStorePort& settings_store);

  [[nodiscard]] static Result<SettingsUpdateReceipt, Error> change_ultra_hdr_jpeg_quality(
      UltraHdrJpegQuality quality,
      SettingsStorePort& settings_store);

  [[nodiscard]] static Result<SettingsUpdateReceipt, Error> change_enter_completion_action(
      CompletionAction action,
      SettingsStorePort& settings_store);

  [[nodiscard]] static Result<SettingsUpdateReceipt, Error> change_double_click_completion_action(
      CompletionAction action,
      SettingsStorePort& settings_store);

  [[nodiscard]] static Result<SettingsUpdateReceipt, Error> mark_initial_settings_presented(
      SettingsStorePort& settings_store);

  [[nodiscard]] static Result<SettingsUpdateReceipt, Error> replace_global_hotkey(
      std::string current_hotkey,
      std::string requested_hotkey,
      GlobalHotkeyPort& hotkey_port,
      SettingsStorePort& settings_store);
};

}  // namespace hdrshot
