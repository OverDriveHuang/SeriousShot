#include "application/settings_workflow.hpp"
#include "test_support.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using hdrshot::Error;
using hdrshot::ErrorCode;
using hdrshot::CompletionAction;
using hdrshot::GlobalHotkeyPort;
using hdrshot::HdrPqPrecision;
using hdrshot::HotkeyReceipt;
using hdrshot::Result;
using hdrshot::Retryability;
using hdrshot::PqDiffuseWhite;
using hdrshot::SaveFormat;
using hdrshot::SettingsPatch;
using hdrshot::SettingsReceipt;
using hdrshot::SettingsSnapshot;
using hdrshot::SettingsStorePort;
using hdrshot::SettingsWorkflow;
using hdrshot::UltraHdrJpegQuality;

Error test_error(const ErrorCode code, std::string module, const Retryability retryability) {
  return Error{code, std::move(module), retryability, {}};
}

class RecordingSettingsStore final : public SettingsStorePort {
 public:
  SettingsSnapshot snapshot{1, 4, "Ctrl+Shift+S", "/Pictures"};
  std::optional<ErrorCode> save_error;
  std::vector<SettingsPatch> patches;

  Result<SettingsSnapshot, Error> load() override {
    return Result<SettingsSnapshot, Error>::success(snapshot);
  }

  Result<SettingsReceipt, Error> save(const SettingsPatch& patch) override {
    patches.push_back(patch);
    if (save_error.has_value()) {
      return Result<SettingsReceipt, Error>::failure(test_error(
          *save_error, "RecordingSettingsStore", Retryability::after_user_action));
    }
    if (patch.global_capture_hotkey.has_value()) {
      snapshot.global_capture_hotkey = *patch.global_capture_hotkey;
    }
    if (patch.default_save_folder.has_value()) {
      snapshot.default_save_folder = *patch.default_save_folder;
    }
    if (patch.save_format.has_value()) {
      snapshot.save_format = *patch.save_format;
    }
    if (patch.pq_diffuse_white.has_value()) {
      snapshot.pq_diffuse_white = *patch.pq_diffuse_white;
    }
    if (patch.hdr_pq_precision.has_value()) {
      snapshot.hdr_pq_precision = *patch.hdr_pq_precision;
    }
    if (patch.ultra_hdr_jpeg_quality.has_value()) {
      snapshot.ultra_hdr_jpeg_quality = *patch.ultra_hdr_jpeg_quality;
    }
    if (patch.enter_completion_action.has_value()) {
      snapshot.enter_completion_action = *patch.enter_completion_action;
    }
    if (patch.double_click_completion_action.has_value()) {
      snapshot.double_click_completion_action = *patch.double_click_completion_action;
    }
    if (patch.initial_settings_presented.has_value()) {
      snapshot.initial_settings_presented = *patch.initial_settings_presented;
    }
    ++snapshot.revision;
    return Result<SettingsReceipt, Error>::success(SettingsReceipt{snapshot.revision});
  }
};

class RecordingGlobalHotkey final : public GlobalHotkeyPort {
 public:
  std::string active{"Ctrl+Shift+S"};
  std::string conflict;
  bool fail_rollback{};
  std::vector<std::string> replacements;

  void set_trigger_handler(hdrshot::HotkeyTriggerHandler handler) override {
    trigger_handler = std::move(handler);
  }

  Result<HotkeyReceipt, Error> register_hotkey(std::string hotkey) override {
    active = std::move(hotkey);
    return Result<HotkeyReceipt, Error>::success(HotkeyReceipt{active});
  }

  Result<HotkeyReceipt, Error> replace_hotkey(std::string hotkey) override {
    replacements.push_back(hotkey);
    if (hotkey == conflict || (fail_rollback && replacements.size() > 1)) {
      return Result<HotkeyReceipt, Error>::failure(test_error(
          ErrorCode::hotkey_conflict, "RecordingGlobalHotkey", Retryability::after_user_action));
    }
    active = std::move(hotkey);
    return Result<HotkeyReceipt, Error>::success(HotkeyReceipt{active});
  }

  Result<HotkeyReceipt, Error> unregister_hotkey() override {
    active.clear();
    return Result<HotkeyReceipt, Error>::success(HotkeyReceipt{});
  }

  hdrshot::HotkeyTriggerHandler trigger_handler;
};

void default_folder_persists_only_folder() {
  RecordingSettingsStore settings;
  const auto result = SettingsWorkflow::change_default_folder("/Screenshots", settings);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value().settings_revision == 5);
  HDRSHOT_CHECK(settings.snapshot.default_save_folder == "/Screenshots");
  HDRSHOT_CHECK(settings.snapshot.global_capture_hotkey == "Ctrl+Shift+S");
  HDRSHOT_CHECK(settings.patches.size() == 1);
  HDRSHOT_CHECK(!settings.patches.front().global_capture_hotkey.has_value());
}

void hotkey_success_updates_system_then_settings() {
  RecordingSettingsStore settings;
  RecordingGlobalHotkey hotkey;
  const auto result = SettingsWorkflow::replace_global_hotkey(
      "Ctrl+Shift+S", "Ctrl+Alt+A", hotkey, settings);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(hotkey.active == "Ctrl+Alt+A");
  HDRSHOT_CHECK(settings.snapshot.global_capture_hotkey == "Ctrl+Alt+A");
  HDRSHOT_CHECK(hotkey.replacements == std::vector<std::string>{"Ctrl+Alt+A"});
  HDRSHOT_CHECK(settings.patches.size() == 1);
}

void hotkey_conflict_does_not_write_settings() {
  RecordingSettingsStore settings;
  RecordingGlobalHotkey hotkey;
  hotkey.conflict = "Ctrl+X";
  const auto result = SettingsWorkflow::replace_global_hotkey(
      "Ctrl+Shift+S", "Ctrl+X", hotkey, settings);
  HDRSHOT_CHECK(!result.has_value());
  HDRSHOT_CHECK(result.error().code == ErrorCode::hotkey_conflict);
  HDRSHOT_CHECK(hotkey.active == "Ctrl+Shift+S");
  HDRSHOT_CHECK(settings.patches.empty());
}

void settings_failure_rolls_hotkey_back() {
  RecordingSettingsStore settings;
  settings.save_error = ErrorCode::permission_denied;
  RecordingGlobalHotkey hotkey;
  const auto result = SettingsWorkflow::replace_global_hotkey(
      "Ctrl+Shift+S", "Ctrl+Alt+A", hotkey, settings);
  HDRSHOT_CHECK(!result.has_value());
  HDRSHOT_CHECK(result.error().code == ErrorCode::permission_denied);
  HDRSHOT_CHECK(result.error().safe_context.at("hotkey_rollback") == "succeeded");
  HDRSHOT_CHECK(hotkey.active == "Ctrl+Shift+S");
  HDRSHOT_CHECK(hotkey.replacements ==
                std::vector<std::string>({"Ctrl+Alt+A", "Ctrl+Shift+S"}));
}

void failed_rollback_is_explicit_inconsistent_state() {
  RecordingSettingsStore settings;
  settings.save_error = ErrorCode::permission_denied;
  RecordingGlobalHotkey hotkey;
  hotkey.fail_rollback = true;
  const auto result = SettingsWorkflow::replace_global_hotkey(
      "Ctrl+Shift+S", "Ctrl+Alt+A", hotkey, settings);
  HDRSHOT_CHECK(!result.has_value());
  HDRSHOT_CHECK(result.error().code == ErrorCode::state_inconsistent);
  HDRSHOT_CHECK(result.error().retryability == Retryability::after_recreate);
  HDRSHOT_CHECK(hotkey.active == "Ctrl+Alt+A");
}

void invalid_values_do_not_touch_ports() {
  RecordingSettingsStore settings;
  RecordingGlobalHotkey hotkey;
  const auto folder = SettingsWorkflow::change_default_folder("", settings);
  HDRSHOT_CHECK(!folder.has_value());
  HDRSHOT_CHECK(settings.patches.empty());

  const auto key = SettingsWorkflow::replace_global_hotkey(
      "Ctrl+Shift+S", "", hotkey, settings);
  HDRSHOT_CHECK(!key.has_value());
  HDRSHOT_CHECK(hotkey.replacements.empty());
}

void diffuse_white_persists_independently() {
  RecordingSettingsStore settings;
  const auto result = SettingsWorkflow::change_pq_diffuse_white(
      PqDiffuseWhite::nits_100, settings);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(settings.snapshot.pq_diffuse_white == PqDiffuseWhite::nits_100);
  HDRSHOT_CHECK(settings.snapshot.save_format == SaveFormat::png_display_p3_dual_range);
  HDRSHOT_CHECK(settings.patches.size() == 1U);
  HDRSHOT_CHECK(!settings.patches.front().save_format.has_value());
}

void hdr_pq_precision_persists_independently() {
  RecordingSettingsStore settings;
  const auto result = SettingsWorkflow::change_hdr_pq_precision(
      HdrPqPrecision::bits_10, settings);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(settings.snapshot.hdr_pq_precision == HdrPqPrecision::bits_10);
  HDRSHOT_CHECK(settings.snapshot.pq_diffuse_white == PqDiffuseWhite::nits_203);
  HDRSHOT_CHECK(settings.patches.size() == 1U);
  HDRSHOT_CHECK(!settings.patches.front().pq_diffuse_white.has_value());
}

void invalid_hdr_pq_precision_is_rejected() {
  RecordingSettingsStore settings;
  const auto result = SettingsWorkflow::change_hdr_pq_precision(
      static_cast<HdrPqPrecision>(11), settings);
  HDRSHOT_CHECK(!result.has_value());
  HDRSHOT_CHECK(settings.patches.empty());
}

void ultra_hdr_format_and_quality_persist_independently() {
  RecordingSettingsStore settings;
  const auto format = SettingsWorkflow::change_save_format(
      SaveFormat::ultra_hdr_jpeg, settings);
  HDRSHOT_CHECK(format.has_value());
  HDRSHOT_CHECK(settings.snapshot.save_format == SaveFormat::ultra_hdr_jpeg);
  const auto quality = SettingsWorkflow::change_ultra_hdr_jpeg_quality(
      UltraHdrJpegQuality::maximum, settings);
  HDRSHOT_CHECK(quality.has_value());
  HDRSHOT_CHECK(
      settings.snapshot.ultra_hdr_jpeg_quality == UltraHdrJpegQuality::maximum);
  HDRSHOT_CHECK(settings.snapshot.pq_diffuse_white == PqDiffuseWhite::nits_203);
  HDRSHOT_CHECK(settings.snapshot.hdr_pq_precision == HdrPqPrecision::bits_10);
}

void invalid_ultra_hdr_quality_is_rejected() {
  RecordingSettingsStore settings;
  const auto result = SettingsWorkflow::change_ultra_hdr_jpeg_quality(
      static_cast<UltraHdrJpegQuality>(91), settings);
  HDRSHOT_CHECK(!result.has_value());
  HDRSHOT_CHECK(result.error().code == ErrorCode::invalid_input);
  HDRSHOT_CHECK(settings.patches.empty());
}

void completion_actions_persist_independently() {
  RecordingSettingsStore settings;
  const auto enter = SettingsWorkflow::change_enter_completion_action(
      CompletionAction::save_default, settings);
  const auto double_click = SettingsWorkflow::change_double_click_completion_action(
      CompletionAction::save_as, settings);
  HDRSHOT_CHECK(enter.has_value());
  HDRSHOT_CHECK(double_click.has_value());
  HDRSHOT_CHECK(
      settings.snapshot.enter_completion_action == CompletionAction::save_default);
  HDRSHOT_CHECK(
      settings.snapshot.double_click_completion_action == CompletionAction::save_as);
  HDRSHOT_CHECK(settings.patches.size() == 2U);
  HDRSHOT_CHECK(settings.patches[0].enter_completion_action.has_value());
  HDRSHOT_CHECK(!settings.patches[0].double_click_completion_action.has_value());
  HDRSHOT_CHECK(!settings.patches[1].enter_completion_action.has_value());
  HDRSHOT_CHECK(settings.patches[1].double_click_completion_action.has_value());
}

void invalid_completion_action_is_rejected() {
  RecordingSettingsStore settings;
  const auto result = SettingsWorkflow::change_enter_completion_action(
      static_cast<CompletionAction>(99), settings);
  HDRSHOT_CHECK(!result.has_value());
  HDRSHOT_CHECK(settings.patches.empty());
}

void initial_settings_presentation_is_persisted() {
  RecordingSettingsStore settings;
  HDRSHOT_CHECK(!settings.snapshot.initial_settings_presented);
  const auto result = SettingsWorkflow::mark_initial_settings_presented(settings);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(settings.snapshot.initial_settings_presented);
  HDRSHOT_CHECK(settings.patches.size() == 1U);
  HDRSHOT_CHECK(settings.patches.front().initial_settings_presented == true);
}

}  // namespace

int main() {
  using hdrshot::test::TestCase;
  return hdrshot::test::run(std::vector<TestCase>{
      {"default folder persists only folder", default_folder_persists_only_folder},
      {"hotkey success updates both", hotkey_success_updates_system_then_settings},
      {"hotkey conflict does not persist", hotkey_conflict_does_not_write_settings},
      {"settings failure rolls back hotkey", settings_failure_rolls_hotkey_back},
      {"rollback failure is explicit", failed_rollback_is_explicit_inconsistent_state},
      {"invalid values do not touch ports", invalid_values_do_not_touch_ports},
      {"diffuse white persists independently", diffuse_white_persists_independently},
      {"HDR PQ precision persists independently", hdr_pq_precision_persists_independently},
      {"invalid HDR PQ precision is rejected", invalid_hdr_pq_precision_is_rejected},
      {"Ultra HDR format and quality persist", ultra_hdr_format_and_quality_persist_independently},
      {"invalid Ultra HDR quality is rejected", invalid_ultra_hdr_quality_is_rejected},
      {"completion actions persist independently", completion_actions_persist_independently},
      {"invalid completion action is rejected", invalid_completion_action_is_rejected},
      {"initial settings presentation persists", initial_settings_presentation_is_persisted},
  });
}
