#include "application/settings_workflow.hpp"

#include <string>
#include <utility>

namespace hdrshot {
namespace {

Error invalid_field(std::string field) {
  return Error{
      ErrorCode::invalid_input,
      "SettingsWorkflow",
      Retryability::never,
      {{"field", std::move(field)}}};
}

}  // namespace

Result<SettingsUpdateReceipt, Error> SettingsWorkflow::change_detailed_logging(
    bool enabled, SettingsStorePort& settings_store) {
  SettingsPatch patch;
  patch.detailed_logging = enabled;
  const auto saved = settings_store.save(patch);
  if (!saved) return Result<SettingsUpdateReceipt, Error>::failure(saved.error());
  return Result<SettingsUpdateReceipt, Error>::success({saved.value().revision});
}

Result<SettingsUpdateReceipt, Error> SettingsWorkflow::change_default_folder(
    std::string folder,
    SettingsStorePort& settings_store) {
  if (folder.empty()) {
    return Result<SettingsUpdateReceipt, Error>::failure(invalid_field("default_save_folder"));
  }

  SettingsPatch patch;
  patch.default_save_folder = std::move(folder);
  const auto saved = settings_store.save(patch);
  if (!saved) {
    return Result<SettingsUpdateReceipt, Error>::failure(saved.error());
  }
  return Result<SettingsUpdateReceipt, Error>::success(
      SettingsUpdateReceipt{saved.value().revision});
}

Result<SettingsUpdateReceipt, Error> SettingsWorkflow::change_save_format(
    const SaveFormat save_format,
    SettingsStorePort& settings_store) {
  if (save_format != SaveFormat::png_display_p3_dual_range &&
      save_format != SaveFormat::ultra_hdr_jpeg) {
    return Result<SettingsUpdateReceipt, Error>::failure(invalid_field("save_format"));
  }
  SettingsPatch patch;
  patch.save_format = save_format;
  const auto saved = settings_store.save(patch);
  if (!saved) {
    return Result<SettingsUpdateReceipt, Error>::failure(saved.error());
  }
  return Result<SettingsUpdateReceipt, Error>::success(
      SettingsUpdateReceipt{saved.value().revision});
}

Result<SettingsUpdateReceipt, Error> SettingsWorkflow::change_ultra_hdr_jpeg_quality(
    const UltraHdrJpegQuality quality,
    SettingsStorePort& settings_store) {
  if (!valid_ultra_hdr_jpeg_quality(quality)) {
    return Result<SettingsUpdateReceipt, Error>::failure(
        invalid_field("ultra_hdr_jpeg_quality"));
  }
  SettingsPatch patch;
  patch.ultra_hdr_jpeg_quality = quality;
  const auto saved = settings_store.save(patch);
  if (!saved) {
    return Result<SettingsUpdateReceipt, Error>::failure(saved.error());
  }
  return Result<SettingsUpdateReceipt, Error>::success(
      SettingsUpdateReceipt{saved.value().revision});
}

Result<SettingsUpdateReceipt, Error> SettingsWorkflow::change_pq_diffuse_white(
    const PqDiffuseWhite pq_diffuse_white,
    SettingsStorePort& settings_store) {
  if (pq_diffuse_white != PqDiffuseWhite::nits_100 &&
      pq_diffuse_white != PqDiffuseWhite::nits_203) {
    return Result<SettingsUpdateReceipt, Error>::failure(
        invalid_field("pq_diffuse_white"));
  }
  SettingsPatch patch;
  patch.pq_diffuse_white = pq_diffuse_white;
  const auto saved = settings_store.save(patch);
  if (!saved) {
    return Result<SettingsUpdateReceipt, Error>::failure(saved.error());
  }
  return Result<SettingsUpdateReceipt, Error>::success(
      SettingsUpdateReceipt{saved.value().revision});
}

Result<SettingsUpdateReceipt, Error> SettingsWorkflow::change_hdr_pq_precision(
    const HdrPqPrecision hdr_pq_precision,
    SettingsStorePort& settings_store) {
  if (hdr_pq_precision != HdrPqPrecision::bits_10 &&
      hdr_pq_precision != HdrPqPrecision::bits_12 &&
      hdr_pq_precision != HdrPqPrecision::bits_16) {
    return Result<SettingsUpdateReceipt, Error>::failure(
        invalid_field("hdr_pq_precision"));
  }
  SettingsPatch patch;
  patch.hdr_pq_precision = hdr_pq_precision;
  const auto saved = settings_store.save(patch);
  if (!saved) {
    return Result<SettingsUpdateReceipt, Error>::failure(saved.error());
  }
  return Result<SettingsUpdateReceipt, Error>::success(
      SettingsUpdateReceipt{saved.value().revision});
}

Result<SettingsUpdateReceipt, Error> SettingsWorkflow::change_enter_completion_action(
    const CompletionAction action,
    SettingsStorePort& settings_store) {
  if (!valid_completion_action(action)) {
    return Result<SettingsUpdateReceipt, Error>::failure(
        invalid_field("enter_completion_action"));
  }
  SettingsPatch patch;
  patch.enter_completion_action = action;
  const auto saved = settings_store.save(patch);
  if (!saved) {
    return Result<SettingsUpdateReceipt, Error>::failure(saved.error());
  }
  return Result<SettingsUpdateReceipt, Error>::success(
      SettingsUpdateReceipt{saved.value().revision});
}

Result<SettingsUpdateReceipt, Error> SettingsWorkflow::change_double_click_completion_action(
    const CompletionAction action,
    SettingsStorePort& settings_store) {
  if (!valid_completion_action(action)) {
    return Result<SettingsUpdateReceipt, Error>::failure(
        invalid_field("double_click_completion_action"));
  }
  SettingsPatch patch;
  patch.double_click_completion_action = action;
  const auto saved = settings_store.save(patch);
  if (!saved) {
    return Result<SettingsUpdateReceipt, Error>::failure(saved.error());
  }
  return Result<SettingsUpdateReceipt, Error>::success(
      SettingsUpdateReceipt{saved.value().revision});
}

Result<SettingsUpdateReceipt, Error> SettingsWorkflow::mark_initial_settings_presented(
    SettingsStorePort& settings_store) {
  SettingsPatch patch;
  patch.initial_settings_presented = true;
  const auto saved = settings_store.save(patch);
  if (!saved) {
    return Result<SettingsUpdateReceipt, Error>::failure(saved.error());
  }
  return Result<SettingsUpdateReceipt, Error>::success(
      SettingsUpdateReceipt{saved.value().revision});
}

Result<SettingsUpdateReceipt, Error> SettingsWorkflow::replace_global_hotkey(
    std::string current_hotkey,
    std::string requested_hotkey,
    GlobalHotkeyPort& hotkey_port,
    SettingsStorePort& settings_store) {
  if (current_hotkey.empty()) {
    return Result<SettingsUpdateReceipt, Error>::failure(invalid_field("current_hotkey"));
  }
  if (requested_hotkey.empty()) {
    return Result<SettingsUpdateReceipt, Error>::failure(invalid_field("requested_hotkey"));
  }

  const auto replaced = hotkey_port.replace_hotkey(requested_hotkey);
  if (!replaced) {
    return Result<SettingsUpdateReceipt, Error>::failure(replaced.error());
  }

  SettingsPatch patch;
  patch.global_capture_hotkey = requested_hotkey;
  const auto saved = settings_store.save(patch);
  if (saved) {
    return Result<SettingsUpdateReceipt, Error>::success(
        SettingsUpdateReceipt{saved.value().revision});
  }

  const auto rollback = hotkey_port.replace_hotkey(current_hotkey);
  if (rollback) {
    auto error = saved.error();
    error.safe_context["hotkey_rollback"] = "succeeded";
    return Result<SettingsUpdateReceipt, Error>::failure(std::move(error));
  }

  return Result<SettingsUpdateReceipt, Error>::failure(Error{
      ErrorCode::state_inconsistent,
      "SettingsWorkflow",
      Retryability::after_recreate,
      {
          {"settings_error", to_string(saved.error().code)},
          {"rollback_error", to_string(rollback.error().code)},
      }});
}

}  // namespace hdrshot
