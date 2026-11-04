#include "application/settings_workflow.hpp"
#include "platform/qt/qt_settings_store_port.hpp"
#include "test_support.hpp"
#include "ui/qt/analysis_readings.hpp"
#include "ui/qt/analysis_defaults_json.hpp"
#include "ui/qt/ui_language_runtime.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <string>
#include <limits>
#include <vector>

namespace {

QByteArray ini_bytes(const QString& path) {
  QFile file(path);
  HDRSHOT_CHECK(file.open(QIODevice::ReadOnly));
  return file.readAll();
}

bool raw_language_matches(const QSettings& raw, const QString& expected) {
  return expected.isNull() ? !raw.contains("uiLanguage")
      : raw.contains("uiLanguage") && raw.value("uiLanguage").toString() == expected;
}

void ui_language_is_optional_and_only_changes_on_a_language_patch() {
  using namespace hdrshot;
  QTemporaryDir directory;
  HDRSHOT_CHECK(directory.isValid());
  for (unsigned int schema = 1; schema <= 5; ++schema) {
    const auto path = directory.filePath(QString::number(schema) + ".ini");
    {
      QSettings raw(path, QSettings::IniFormat);
      raw.setValue("schemaVersion", schema);
      raw.setValue("globalCaptureHotkey", "Ctrl+Alt+9");
      raw.sync();
      HDRSHOT_CHECK(raw.status() == QSettings::NoError);
    }
    QtSettingsStorePort store(path.toStdString(), "Ctrl+Shift+2", "/Pictures");
    HDRSHOT_CHECK(!store.load().value().ui_language);
    HDRSHOT_CHECK(SettingsWorkflow::change_detailed_logging(true, store).has_value());
    QSettings raw(path, QSettings::IniFormat);
    HDRSHOT_CHECK(!raw.contains("uiLanguage"));
    HDRSHOT_CHECK(raw.value("schemaVersion").toUInt() == 5);
    HDRSHOT_CHECK(raw.value("globalCaptureHotkey").toString() == "Ctrl+Alt+9");
    for (const auto& invalid : {QStringLiteral(""), QStringLiteral("zh-Hant"), QStringLiteral("EN")}) {
      raw.setValue("uiLanguage", invalid);
      raw.sync();
      HDRSHOT_CHECK(!store.load().value().ui_language);
      HDRSHOT_CHECK(SettingsWorkflow::mark_initial_settings_presented(store).has_value());
      HDRSHOT_CHECK(raw_language_matches(raw, invalid));
    }
    for (const auto language : {UiLanguage::simplified_chinese, UiLanguage::english}) {
      const auto before = store.load().value();
      const auto saved = SettingsWorkflow::change_ui_language(language, store);
      HDRSHOT_CHECK(saved && saved.value().settings_revision == before.revision + 1);
      auto expected = before;
      expected.ui_language = language;
      ++expected.revision;
      QtSettingsStorePort reopened(path.toStdString(), "ignored", "ignored");
      HDRSHOT_CHECK(reopened.load().value() == expected);
      HDRSHOT_CHECK(raw_language_matches(raw, language == UiLanguage::simplified_chinese
          ? QStringLiteral("zh-CN") : QStringLiteral("en")));
    }
  }
}

void ui_language_rejects_invalid_and_combined_patches_without_mutating() {
  using namespace hdrshot;
  QTemporaryDir directory;
  HDRSHOT_CHECK(directory.isValid());
  const auto path = directory.filePath("settings.ini");
  QtSettingsStorePort store(path.toStdString(), "Ctrl+Shift+2", "/Pictures");
  HDRSHOT_CHECK(SettingsWorkflow::change_ui_language(UiLanguage::simplified_chinese, store).has_value());
  const auto before = ini_bytes(path);
  const auto snapshot = store.load().value();
  const auto rejected = [&](const SettingsPatch& patch) {
    const auto result = store.save(patch);
    HDRSHOT_CHECK(!result && result.error().code == ErrorCode::invalid_input);
    HDRSHOT_CHECK(ini_bytes(path) == before);
    HDRSHOT_CHECK(store.load().value() == snapshot);
  };
  for (const auto assign : {
       +[](SettingsPatch& patch) { patch.detailed_logging = false; },
       +[](SettingsPatch& patch) { patch.analysis_defaults = AnalysisDefaults{}; },
       +[](SettingsPatch& patch) { patch.pinned_screenshot_defaults = PinnedScreenshotDefaults{}; },
       +[](SettingsPatch& patch) { patch.windows_capture_bypass_sdr_white_enabled = false; }}) {
    SettingsPatch patch;
    patch.ui_language = UiLanguage::english;
    assign(patch);
    rejected(patch);
  }
  SettingsPatch invalid;
  invalid.ui_language = static_cast<UiLanguage>(-1);
  rejected(invalid);
}

class ScopedIniWriteBlock {
 public:
  ScopedIniWriteBlock(const QString& file, const QString& directory) {
#ifdef Q_OS_WIN
    (void)directory;
    // QSettings can read, but its atomic rename cannot replace this open file.
    handle_ = CreateFileW(reinterpret_cast<LPCWSTR>(file.utf16()), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    HDRSHOT_CHECK(handle_ != INVALID_HANDLE_VALUE);
#else
    (void)file;
    directory_ = directory;
    HDRSHOT_CHECK(QFile::setPermissions(directory_, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
#endif
  }
  ~ScopedIniWriteBlock() {
#ifdef Q_OS_WIN
    CloseHandle(handle_);
#else
    QFile::setPermissions(directory_, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
  }
  ScopedIniWriteBlock(const ScopedIniWriteBlock&) = delete;
  ScopedIniWriteBlock& operator=(const ScopedIniWriteBlock&) = delete;

 private:
#ifdef Q_OS_WIN
  HANDLE handle_{INVALID_HANDLE_VALUE};
#else
  QString directory_;
#endif
};

void failed_ui_language_write_cannot_leak_into_the_next_save() {
  using namespace hdrshot;
  for (const auto& [prior, prior_revision] :
       {std::pair{QString{}, 41}, std::pair{QStringLiteral("invalid"), 41},
        std::pair{QStringLiteral("zh-CN"), 41}, std::pair{QString{}, 0}}) {
    QTemporaryDir directory;
    HDRSHOT_CHECK(directory.isValid());
    const auto path = directory.filePath("settings.ini");
    {
      QSettings raw(path, QSettings::IniFormat);
      raw.setValue("unrelatedKey", "original");
      if (prior_revision) raw.setValue("revision", prior_revision);
      if (!prior.isNull()) raw.setValue("uiLanguage", prior);
      raw.sync();
      HDRSHOT_CHECK(raw.status() == QSettings::NoError);
    }
    const auto bytes_before = ini_bytes(path);
    QtSettingsStorePort store(path.toStdString(), "Ctrl+Shift+2", "/Pictures");
    {
      const ScopedIniWriteBlock write_block(path, directory.path());
      const auto failed = SettingsWorkflow::change_ui_language(UiLanguage::english, store);
      HDRSHOT_CHECK(!failed && failed.error().code == ErrorCode::permission_denied);
      HDRSHOT_CHECK(failed.error().safe_context.at("qtStatus") ==
          std::to_string(QSettings::AccessError));
      HDRSHOT_CHECK(ini_bytes(path) == bytes_before);
      const QSettings second_instance(path, QSettings::IniFormat);
      HDRSHOT_CHECK(raw_language_matches(second_instance, prior));
      HDRSHOT_CHECK(second_instance.contains("revision") == (prior_revision != 0));
      HDRSHOT_CHECK(second_instance.value("revision").toInt() == prior_revision);
    }
    const auto unrelated = SettingsWorkflow::change_detailed_logging(true, store);
    HDRSHOT_CHECK(unrelated && unrelated.value().settings_revision ==
        static_cast<std::uint64_t>(prior_revision + 1));
    const auto independent_read = [&](const QString& expected_language, const int revision) {
      const QStringList args{"--verify-ini", path,
          expected_language.isNull() ? QStringLiteral("<absent>") : expected_language,
          QString::number(revision)};
      HDRSHOT_CHECK(QProcess::execute(QCoreApplication::applicationFilePath(), args) == 0);
    };
    independent_read(prior, prior_revision + 1);
    HDRSHOT_CHECK(SettingsWorkflow::change_ui_language(UiLanguage::english, store).has_value());
    independent_read(QStringLiteral("en"), prior_revision + 2);
  }
}

void pinned_defaults_roundtrip_and_invalid_patch_is_atomic() {
  QTemporaryDir directory;
  HDRSHOT_CHECK(directory.isValid());
  const auto path = directory.filePath("settings.ini").toStdString();
  hdrshot::QtSettingsStorePort store(path, "Command+Shift+2", "/Pictures");
  HDRSHOT_CHECK(store.load().value().pinned_screenshot_defaults == hdrshot::PinnedScreenshotDefaults{});
  HDRSHOT_CHECK(store.load().value().pinned_screenshot_defaults.short_edge == 500);
  for (int edge = 450; edge <= 800; edge += 50) {
    const hdrshot::PinnedScreenshotDefaults chosen{edge % 100 == 0, edge};
    hdrshot::SettingsPatch patch;
    patch.pinned_screenshot_defaults = chosen;
    HDRSHOT_CHECK(store.save(patch).has_value());
    hdrshot::QtSettingsStorePort reopened(path, "ignored", "ignored");
    HDRSHOT_CHECK(reopened.load().value().pinned_screenshot_defaults == chosen);
  }
  const auto before = store.load().value();
  for (const int bad : {-1, 399, 400, 425, 801}) {
    hdrshot::SettingsPatch patch;
    patch.pinned_screenshot_defaults = hdrshot::PinnedScreenshotDefaults{false, bad};
    patch.default_save_folder = "/Changed";
    HDRSHOT_CHECK(!store.save(patch));
    HDRSHOT_CHECK(store.load().value() == before);
  }
  hdrshot::SettingsPatch unrelated;
  unrelated.detailed_logging = true;
  unrelated.analysis_defaults = hdrshot::AnalysisDefaults{};
  HDRSHOT_CHECK(store.save(unrelated).has_value());
  HDRSHOT_CHECK(store.load().value().pinned_screenshot_defaults == before.pinned_screenshot_defaults);
}

void pinned_defaults_migrate_missing_and_recover_corrupt_fields_independently() {
  QTemporaryDir directory;
  HDRSHOT_CHECK(directory.isValid());
  const auto path = directory.filePath("settings.ini");
  QSettings raw(path, QSettings::IniFormat);
  raw.setValue("schemaVersion", 1);
  raw.setValue("globalCaptureHotkey", "Command+Option+9");
  raw.sync();
  hdrshot::QtSettingsStorePort store(path.toStdString(), "Command+Shift+2", "/Pictures");
  HDRSHOT_CHECK(store.load().value().pinned_screenshot_defaults == hdrshot::PinnedScreenshotDefaults{});
  HDRSHOT_CHECK(store.load().value().pinned_screenshot_defaults.short_edge == 500);
  raw.setValue("pinnedScreenshotAlwaysOnTop", false);
  raw.setValue("pinnedScreenshotShortEdge", 400);
  raw.sync();
  const auto legacy=store.load();
  HDRSHOT_CHECK(legacy.has_value());
  HDRSHOT_CHECK(!legacy.value().pinned_screenshot_defaults.always_on_top);
  HDRSHOT_CHECK(legacy.value().pinned_screenshot_defaults.short_edge == 450);
  HDRSHOT_CHECK(legacy.value().global_capture_hotkey == "Command+Option+9");
  hdrshot::SettingsPatch persist_legacy;
  persist_legacy.pinned_screenshot_defaults=legacy.value().pinned_screenshot_defaults;
  HDRSHOT_CHECK(store.save(persist_legacy).has_value());
  raw.sync();
  HDRSHOT_CHECK(raw.value("pinnedScreenshotShortEdge").toInt() == 450);
  for (const auto* malformed : {"", "true-ish", "0", "1", "2"}) {
    raw.setValue("pinnedScreenshotAlwaysOnTop", malformed);
    raw.setValue("pinnedScreenshotShortEdge", 650);
    raw.sync();
    const auto loaded = store.load();
    HDRSHOT_CHECK(loaded.has_value());
    HDRSHOT_CHECK(loaded.value().pinned_screenshot_defaults.always_on_top);
    HDRSHOT_CHECK(loaded.value().pinned_screenshot_defaults.short_edge == 650);
    HDRSHOT_CHECK(loaded.value().global_capture_hotkey == "Command+Option+9");
  }
  for (const auto* malformed : {"", "400.5", "NaN", "399", "425", "801", "9999999999999999999999"}) {
    raw.setValue("pinnedScreenshotAlwaysOnTop", false);
    raw.setValue("pinnedScreenshotShortEdge", malformed);
    raw.sync();
    const auto loaded = store.load();
    HDRSHOT_CHECK(loaded.has_value());
    HDRSHOT_CHECK(!loaded.value().pinned_screenshot_defaults.always_on_top);
    HDRSHOT_CHECK(loaded.value().pinned_screenshot_defaults.short_edge == 500);
  }
}

void defaults_and_patches_persist_across_instances() {
  QTemporaryDir directory;
  HDRSHOT_CHECK(directory.isValid());
  const auto path = directory.filePath(QStringLiteral("settings.ini")).toStdString();
  hdrshot::QtSettingsStorePort first(path, "Command+Shift+2", "/Pictures/SeriousShot");
  const auto defaults = first.load();
  HDRSHOT_CHECK(defaults.has_value());
  HDRSHOT_CHECK(defaults.value().revision == 0);
  HDRSHOT_CHECK(defaults.value().global_capture_hotkey == "Command+Shift+2");
  HDRSHOT_CHECK(defaults.value().default_save_folder == "/Pictures/SeriousShot");
  HDRSHOT_CHECK(
      defaults.value().save_format == hdrshot::SaveFormat::png_display_p3_dual_range);
  HDRSHOT_CHECK(
      defaults.value().pq_diffuse_white == hdrshot::PqDiffuseWhite::nits_203);
  HDRSHOT_CHECK(
      defaults.value().hdr_pq_precision == hdrshot::HdrPqPrecision::bits_10);
  HDRSHOT_CHECK(
      defaults.value().ultra_hdr_jpeg_quality ==
      hdrshot::UltraHdrJpegQuality::balanced);
  HDRSHOT_CHECK(
      defaults.value().enter_completion_action ==
      hdrshot::CompletionAction::copy_to_clipboard);
  HDRSHOT_CHECK(
      defaults.value().double_click_completion_action ==
      hdrshot::CompletionAction::copy_to_clipboard);
  HDRSHOT_CHECK(!defaults.value().initial_settings_presented);
  HDRSHOT_CHECK(!defaults.value().detailed_logging);
#ifdef Q_OS_WIN
  HDRSHOT_CHECK(defaults.value().windows_scrgb_gain == 0.5);
#else
  HDRSHOT_CHECK(defaults.value().windows_scrgb_gain == 1.0);
#endif
  HDRSHOT_CHECK(!defaults.value().windows_capture_gain_enabled);
  HDRSHOT_CHECK(!defaults.value().windows_capture_bypass_sdr_white_enabled);

  hdrshot::SettingsPatch hotkey;
  hotkey.global_capture_hotkey = "Command+Option+2";
  const auto first_saved = first.save(hotkey);
  HDRSHOT_CHECK(first_saved.has_value());
  HDRSHOT_CHECK(first_saved.value().revision == 1);

  hdrshot::SettingsPatch folder;
  folder.default_save_folder = "/Screenshots";
  const auto second_saved = first.save(folder);
  HDRSHOT_CHECK(second_saved.has_value());
  HDRSHOT_CHECK(second_saved.value().revision == 2);

  hdrshot::SettingsPatch format;
  format.save_format = hdrshot::SaveFormat::ultra_hdr_jpeg;
  format.pq_diffuse_white = hdrshot::PqDiffuseWhite::nits_100;
  format.hdr_pq_precision = hdrshot::HdrPqPrecision::bits_10;
  format.ultra_hdr_jpeg_quality = hdrshot::UltraHdrJpegQuality::maximum;
  format.enter_completion_action = hdrshot::CompletionAction::save_default;
  format.double_click_completion_action = hdrshot::CompletionAction::save_as;
  format.initial_settings_presented = true;
  format.detailed_logging = true;
#ifdef Q_OS_WIN
  format.windows_scrgb_gain = 0.5;
  format.windows_capture_gain_enabled = true;
  format.windows_capture_bypass_sdr_white_enabled = true;
#endif
  const auto third_saved = first.save(format);
  HDRSHOT_CHECK(third_saved.has_value());
  HDRSHOT_CHECK(third_saved.value().revision == 3);

  hdrshot::QtSettingsStorePort reopened(path, "ignored", "ignored");
  const auto loaded = reopened.load();
  HDRSHOT_CHECK(loaded.has_value());
  HDRSHOT_CHECK(loaded.value().revision == 3);
  HDRSHOT_CHECK(loaded.value().global_capture_hotkey == "Command+Option+2");
  HDRSHOT_CHECK(loaded.value().default_save_folder == "/Screenshots");
  HDRSHOT_CHECK(
      loaded.value().save_format == hdrshot::SaveFormat::ultra_hdr_jpeg);
  HDRSHOT_CHECK(loaded.value().pq_diffuse_white == hdrshot::PqDiffuseWhite::nits_100);
  HDRSHOT_CHECK(loaded.value().hdr_pq_precision == hdrshot::HdrPqPrecision::bits_10);
  HDRSHOT_CHECK(
      loaded.value().ultra_hdr_jpeg_quality ==
      hdrshot::UltraHdrJpegQuality::maximum);
  HDRSHOT_CHECK(
      loaded.value().enter_completion_action == hdrshot::CompletionAction::save_default);
  HDRSHOT_CHECK(
      loaded.value().double_click_completion_action == hdrshot::CompletionAction::save_as);
  HDRSHOT_CHECK(loaded.value().initial_settings_presented);
  HDRSHOT_CHECK(loaded.value().detailed_logging);
#ifdef Q_OS_WIN
  HDRSHOT_CHECK(loaded.value().windows_scrgb_gain == 0.5);
  HDRSHOT_CHECK(loaded.value().windows_capture_gain_enabled);
  HDRSHOT_CHECK(loaded.value().windows_capture_bypass_sdr_white_enabled);
  QSettings raw(QString::fromStdString(path), QSettings::IniFormat);
  HDRSHOT_CHECK(raw.value("windowsCaptureCompatibilityVersion").toInt() == 1);
  HDRSHOT_CHECK(raw.value("windowsCaptureGainEnabled").toBool());
  HDRSHOT_CHECK(raw.value("windowsCaptureBypassSdrWhiteEnabled").toBool());
  HDRSHOT_CHECK(raw.value("windowsScRgbGain").toDouble() == 0.5);
#endif
  hdrshot::SettingsPatch quiet;
  quiet.detailed_logging = false;
  HDRSHOT_CHECK(reopened.save(quiet).has_value());
  HDRSHOT_CHECK(!first.load().value().detailed_logging);
}

void invalid_windows_gain_never_commits() {
#ifdef Q_OS_WIN
  QTemporaryDir directory;
  HDRSHOT_CHECK(directory.isValid());
  const auto path = directory.filePath("settings.ini").toStdString();
  hdrshot::QtSettingsStorePort store(path, "Control+Shift+2", "/Pictures");
  auto before = store.load().value();
  for (const double invalid : {-0.01, 3.01, std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN()}) {
    hdrshot::SettingsPatch patch;
    patch.windows_scrgb_gain = invalid;
    HDRSHOT_CHECK(!store.save(patch));
    HDRSHOT_CHECK(store.load().value() == before);
  }
  QSettings damaged(QString::fromStdString(path), QSettings::IniFormat);
  damaged.setValue("windowsScRgbGain", "nan");
  damaged.sync();
  HDRSHOT_CHECK(!store.load());
#endif
}

void legacy_windows_gain_migrates_without_enabling_bypass() {
#ifdef Q_OS_WIN
  for (const double legacy_gain : {0.0, 0.5, 1.0, 1.274008, 3.0}) {
    QTemporaryDir directory;
    HDRSHOT_CHECK(directory.isValid());
    const auto path = directory.filePath("settings.ini");
    {
      QSettings legacy(path, QSettings::IniFormat);
      legacy.setValue("schemaVersion", 5);
      legacy.setValue("windowsScRgbGain", legacy_gain);
      legacy.sync();
    }
    hdrshot::QtSettingsStorePort store(path.toStdString(), "Control+Shift+2", "/Pictures");
    const auto loaded = store.load();
    HDRSHOT_CHECK(loaded.has_value());
    HDRSHOT_CHECK(loaded.value().windows_capture_gain_enabled == (legacy_gain != 1.0));
    HDRSHOT_CHECK(!loaded.value().windows_capture_bypass_sdr_white_enabled);
    HDRSHOT_CHECK(loaded.value().windows_scrgb_gain ==
        (legacy_gain == 1.0 ? 0.5 : legacy_gain));
    hdrshot::SettingsPatch other;
    other.detailed_logging = true;
    HDRSHOT_CHECK(store.save(other).has_value());
    const auto reopened = store.load();
    HDRSHOT_CHECK(reopened.has_value());
    HDRSHOT_CHECK(reopened.value().windows_capture_gain_enabled ==
        loaded.value().windows_capture_gain_enabled);
    HDRSHOT_CHECK(reopened.value().windows_scrgb_gain == loaded.value().windows_scrgb_gain);
    QSettings raw(path, QSettings::IniFormat);
    HDRSHOT_CHECK(raw.value("windowsCaptureCompatibilityVersion").toInt() == 1);
    HDRSHOT_CHECK(raw.value("windowsCaptureBypassSdrWhiteEnabled").toString() == "false");
  }
#endif
}

void windows_compatibility_preserves_independent_options() {
#ifdef Q_OS_WIN
  QTemporaryDir directory;
  HDRSHOT_CHECK(directory.isValid());
  const auto path = directory.filePath("settings.ini").toStdString();
  hdrshot::QtSettingsStorePort store(path, "Control+Shift+2", "/Pictures");
  hdrshot::SettingsPatch patch;
  patch.windows_capture_gain_enabled = false;
  patch.windows_capture_bypass_sdr_white_enabled = true;
  patch.windows_scrgb_gain = 1.274008;
  const auto first = store.save(patch);
  HDRSHOT_CHECK(first.has_value());
  HDRSHOT_CHECK(first.value().revision == 1);
  hdrshot::QtSettingsStorePort reopened(path, "Control+Shift+2", "/Pictures");
  const auto loaded = reopened.load();
  HDRSHOT_CHECK(loaded.has_value());
  HDRSHOT_CHECK(!loaded.value().windows_capture_gain_enabled);
  HDRSHOT_CHECK(loaded.value().windows_capture_bypass_sdr_white_enabled);
  HDRSHOT_CHECK(loaded.value().windows_scrgb_gain == 1.274008);
  patch.windows_capture_gain_enabled = true;
  patch.windows_capture_bypass_sdr_white_enabled = false;
  patch.windows_scrgb_gain = 0.0;
  const auto second = reopened.save(patch);
  HDRSHOT_CHECK(second.has_value());
  HDRSHOT_CHECK(second.value().revision == 2);
  const auto reloaded = store.load();
  HDRSHOT_CHECK(reloaded.has_value());
  HDRSHOT_CHECK(reloaded.value().windows_capture_gain_enabled);
  HDRSHOT_CHECK(!reloaded.value().windows_capture_bypass_sdr_white_enabled);
  HDRSHOT_CHECK(reloaded.value().windows_scrgb_gain == 0.0);
#endif
}

void malformed_windows_compatibility_fails_without_writing() {
#ifdef Q_OS_WIN
  struct BadSetting { const char* key; const char* value; const char* field; };
  for (const auto bad : {
      BadSetting{"windowsCaptureCompatibilityVersion", "2", "windowsCaptureCompatibilityVersion"},
      BadSetting{"windowsCaptureGainEnabled", "yes", "windowsCaptureGainEnabled"},
      BadSetting{"windowsCaptureBypassSdrWhiteEnabled", "2", "windowsCaptureBypassSdrWhiteEnabled"},
      BadSetting{"windowsScRgbGain", "NaN", "windowsScRgbGain"},
      BadSetting{"windowsScRgbGain", "3.1", "windowsScRgbGain"}}) {
    QTemporaryDir directory;
    HDRSHOT_CHECK(directory.isValid());
    const auto path = directory.filePath("settings.ini");
    {
      QSettings raw(path, QSettings::IniFormat);
      raw.setValue("windowsCaptureCompatibilityVersion", 1);
      raw.setValue("windowsCaptureGainEnabled", true);
      raw.setValue("windowsCaptureBypassSdrWhiteEnabled", false);
      raw.setValue("windowsScRgbGain", 0.5);
      raw.setValue(bad.key, bad.value);
      raw.sync();
    }
    hdrshot::QtSettingsStorePort store(path.toStdString(), "Control+Shift+2", "/Pictures");
    const auto loaded = store.load();
    HDRSHOT_CHECK(!loaded);
    HDRSHOT_CHECK(loaded.error().code == hdrshot::ErrorCode::settings_corrupt);
    HDRSHOT_CHECK(loaded.error().safe_context.at("field") == bad.field);
    hdrshot::SettingsPatch patch;
    patch.detailed_logging = true;
    HDRSHOT_CHECK(!store.save(patch));
    QSettings raw(path, QSettings::IniFormat);
    HDRSHOT_CHECK(!raw.contains("detailedLogging"));
  }
  for (const auto missing_key : {"windowsCaptureGainEnabled",
      "windowsCaptureBypassSdrWhiteEnabled", "windowsScRgbGain"}) {
    QTemporaryDir directory;
    HDRSHOT_CHECK(directory.isValid());
    const auto path = directory.filePath("settings.ini");
    {
      QSettings raw(path, QSettings::IniFormat);
      raw.setValue("windowsCaptureCompatibilityVersion", 1);
      raw.setValue("windowsCaptureGainEnabled", true);
      raw.setValue("windowsCaptureBypassSdrWhiteEnabled", false);
      raw.setValue("windowsScRgbGain", 0.5);
      raw.remove(missing_key);
      raw.sync();
    }
    hdrshot::QtSettingsStorePort store(path.toStdString(), "Control+Shift+2", "/Pictures");
    const auto loaded = store.load();
    HDRSHOT_CHECK(!loaded);
    HDRSHOT_CHECK(loaded.error().code == hdrshot::ErrorCode::settings_corrupt);
    HDRSHOT_CHECK(loaded.error().safe_context.at("field") == missing_key);
  }
  QTemporaryDir directory;
  HDRSHOT_CHECK(directory.isValid());
  const auto path = directory.filePath("settings.ini");
  {
    QSettings raw(path, QSettings::IniFormat);
    raw.setValue("windowsCaptureGainEnabled", true);
    raw.sync();
  }
  hdrshot::QtSettingsStorePort store(path.toStdString(), "Control+Shift+2", "/Pictures");
  const auto partial = store.load();
  HDRSHOT_CHECK(!partial);
  HDRSHOT_CHECK(partial.error().safe_context.at("field") ==
      "windowsCaptureCompatibilityVersion");
#endif
}

void schema_one_migrates_old_format_and_defaults_diffuse_white() {
  QTemporaryDir directory;
  HDRSHOT_CHECK(directory.isValid());
  const auto qpath = directory.filePath(QStringLiteral("settings.ini"));
  QSettings settings(qpath, QSettings::IniFormat);
  settings.setValue(QStringLiteral("schemaVersion"), 1);
  settings.setValue(QStringLiteral("saveFormat"), QStringLiteral("png_hdr_sdr"));
  settings.sync();

  hdrshot::QtSettingsStorePort store(
      qpath.toStdString(), "Command+Shift+2", "/Pictures/SeriousShot");
  const auto loaded = store.load();
  HDRSHOT_CHECK(loaded.has_value());
  HDRSHOT_CHECK(loaded.value().schema_version == 5U);
  HDRSHOT_CHECK(
      loaded.value().save_format == hdrshot::SaveFormat::png_display_p3_dual_range);
  HDRSHOT_CHECK(loaded.value().pq_diffuse_white == hdrshot::PqDiffuseWhite::nits_203);
  HDRSHOT_CHECK(loaded.value().hdr_pq_precision == hdrshot::HdrPqPrecision::bits_10);
  HDRSHOT_CHECK(
      loaded.value().ultra_hdr_jpeg_quality ==
      hdrshot::UltraHdrJpegQuality::balanced);
  HDRSHOT_CHECK(loaded.value().initial_settings_presented);
}

void schema_two_defaults_hdr_pq_precision() {
  QTemporaryDir directory;
  HDRSHOT_CHECK(directory.isValid());
  const auto qpath = directory.filePath(QStringLiteral("settings.ini"));
  QSettings settings(qpath, QSettings::IniFormat);
  settings.setValue(QStringLiteral("schemaVersion"), 2);
  settings.setValue(QStringLiteral("saveFormat"),
                    QStringLiteral("png_display_p3_dual_range"));
  settings.setValue(QStringLiteral("pqDiffuseWhiteNits"), 100U);
  settings.sync();

  hdrshot::QtSettingsStorePort store(
      qpath.toStdString(), "Command+Shift+2", "/Pictures/SeriousShot");
  const auto loaded = store.load();
  HDRSHOT_CHECK(loaded.has_value());
  HDRSHOT_CHECK(loaded.value().schema_version == 5U);
  HDRSHOT_CHECK(loaded.value().pq_diffuse_white == hdrshot::PqDiffuseWhite::nits_100);
  HDRSHOT_CHECK(loaded.value().hdr_pq_precision == hdrshot::HdrPqPrecision::bits_10);
  HDRSHOT_CHECK(
      loaded.value().ultra_hdr_jpeg_quality ==
      hdrshot::UltraHdrJpegQuality::balanced);
  HDRSHOT_CHECK(loaded.value().initial_settings_presented);
}

void unsupported_schema_fails_explicitly() {
  QTemporaryDir directory;
  HDRSHOT_CHECK(directory.isValid());
  const auto qpath = directory.filePath(QStringLiteral("settings.ini"));
  QSettings settings(qpath, QSettings::IniFormat);
  settings.setValue(QStringLiteral("schemaVersion"), 99);
  settings.sync();

  hdrshot::QtSettingsStorePort store(
      qpath.toStdString(), "Command+Shift+2", "/Pictures/SeriousShot");
  const auto loaded = store.load();
  HDRSHOT_CHECK(!loaded.has_value());
  HDRSHOT_CHECK(loaded.error().code == hdrshot::ErrorCode::settings_corrupt);
}

void analyzer_preferences_are_independent_bounded_and_preserved() {
  QTemporaryDir directory;
  HDRSHOT_CHECK(directory.isValid());
  const auto path = directory.filePath("settings.ini").toStdString();
  hdrshot::QtSettingsStorePort store(path,"Command+Shift+2","/Pictures");
  HDRSHOT_CHECK(store.load().value().analyzer_preferences_json.empty());
  hdrshot::SettingsPatch analysis;
  analysis.analyzer_preferences_json = R"({"version":1,"white":100,"ratios":[0.7,0.4,0.6,0.5]})";
  HDRSHOT_CHECK(store.save(analysis).has_value());
  const auto first = store.load().value();
  HDRSHOT_CHECK(first.pq_diffuse_white == hdrshot::PqDiffuseWhite::nits_203);
  hdrshot::SettingsPatch other;
  other.save_format = hdrshot::SaveFormat::ultra_hdr_jpeg;
  HDRSHOT_CHECK(store.save(other).has_value());
  hdrshot::QtSettingsStorePort reopened(path,"Command+Shift+2","/Pictures");
  HDRSHOT_CHECK(reopened.load().value().analyzer_preferences_json == *analysis.analyzer_preferences_json);
  const auto before = reopened.load().value();
  analysis.analyzer_preferences_json = std::string(65537,'x');
  analysis.pq_diffuse_white = hdrshot::PqDiffuseWhite::nits_100;
  HDRSHOT_CHECK(!reopened.save(analysis));
  HDRSHOT_CHECK(reopened.load().value() == before);
  QSettings damaged(QString::fromStdString(path), QSettings::IniFormat);
  damaged.setValue("analyzerPreferences", QString(65537, QChar('x')));
  damaged.sync();
  HDRSHOT_CHECK(reopened.load().has_value());
  HDRSHOT_CHECK(reopened.load().value().analyzer_preferences_json.empty());
  HDRSHOT_CHECK(reopened.load().value().save_format == before.save_format);
}

void scalar_readings_are_separate_precise_and_deduplicated() {
  hdrshot::MagnifierReadings all;all.selected.fill(true);
  auto r=hdrshot::magnifier_readout({2,.25,.125},203,false);
  auto rows=hdrshot::magnifier_scalar_rows(r,all);
  HDRSHOT_CHECK(std::count_if(rows.begin(),rows.end(),[](const auto& x){return x.value=="--";})==13);
  hdrshot::AnalyzerReadings fields;fields.selected.fill(true);
  auto readout=hdrshot::analysis::make_readout({2,2,2},{1,1,1},1,0,{hdrshot::analysis::WorkingSpace::display_p3_sdr,203,0});
  const auto sdr=hdrshot::analyzer_reading_rows(readout,fields,hdrshot::analysis::WorkingSpace::display_p3_sdr);
  HDRSHOT_CHECK(sdr.filter("L*a*b*").size()==1 && sdr.filter("ITP").empty() && !sdr.join("\n").contains("--"));
  HDRSHOT_CHECK(std::isfinite(readout.encoded_luma));HDRSHOT_CHECK_NEAR(readout.encoded_luma,1,1e-12);
}

void magnifier_storage_ids_and_menu_order_are_compatible() {
  using namespace hdrshot;
  HDRSHOT_CHECK(magnifier_reading_names().size() == 13);
  for (int old_id = 0; old_id < 9; ++old_id) {
    QJsonArray flags;
    for (int i = 0; i < 9; ++i) flags.append(i == old_id);
    const auto json = QString::fromUtf8(QJsonDocument(QJsonObject{
        {"version", 2}, {"white", 100}, {"magnifier", flags}}).toJson());
    const auto old = analysis_defaults_from_json(json);
    HDRSHOT_CHECK(old.reference_white_nits == 100);
    for (int i = 0; i < 13; ++i)
      HDRSHOT_CHECK(old.magnifier.selected[i] == (i == old_id));
    const auto migrated = analysis_defaults_from_json(analysis_defaults_json(old));
    HDRSHOT_CHECK(migrated == old);
  }
  for (std::size_t id = 0; id < MagnifierReadings::count; ++id) {
    AnalysisDefaults chosen;
    chosen.magnifier.selected.fill(false);
    chosen.magnifier.selected[id] = true;
    HDRSHOT_CHECK(analysis_defaults_from_json(analysis_defaults_json(chosen)) == chosen);
    const auto menu = magnifier_menu_selection(chosen.magnifier);
    HDRSHOT_CHECK(std::count(menu.begin(), menu.end(), true) == 1);
    HDRSHOT_CHECK(magnifier_readings_from_menu(menu) == chosen.magnifier);
    for (std::size_t i = 0; i < menu.size(); ++i)
      HDRSHOT_CHECK(menu[i] == (magnifier_reading_order[i] == id));
  }
  const auto defaults = magnifier_menu_selection(MagnifierReadings{});
  HDRSHOT_CHECK(defaults[0] && defaults[5]);
  HDRSHOT_CHECK(std::count(defaults.begin(), defaults.end(), true) == 2);
  for (std::size_t menu : {2u, 4u, 6u, 7u}) {
    MagnifierReadings only;
    only.selected.fill(false);
    only.selected[magnifier_reading_order[menu]] = true;
    const auto rows = magnifier_scalar_rows(magnifier_readout({.3, .4, .2}, 203, false), only);
    HDRSHOT_CHECK(rows.size() == 5 && rows[0].heading);
    HDRSHOT_CHECK(rows[0].name == magnifier_reading_names()[int(menu)]);
    HDRSHOT_CHECK(rows[1].name == (menu < 5 ? "Y′" : "Y"));
    HDRSHOT_CHECK(rows[2].name == (menu < 5 ? "R′" : "R"));
    for (std::size_t i = 1; i < rows.size(); ++i)
      HDRSHOT_CHECK(!rows[i].heading && rows[i].value != "--");
  }
}

void analysis_defaults_roundtrip_migration_and_local_isolation() {
  QTemporaryDir directory;
  const auto path=directory.filePath("settings.ini");
  {
    QSettings old(path,QSettings::IniFormat);
    old.setValue("schemaVersion",5);
    old.setValue("globalCaptureHotkey","Command+Option+9");
    old.setValue("detailedLogging",true);
    old.setValue("analyzerPreferences",R"({"version":1,"white":100,"blur":4,"sampleSize":31,"readings":[true,false,false,true,true],"vectorGain":2.5})");
    old.sync();
  }
  hdrshot::QtSettingsStorePort store(path.toStdString(),"Command+Shift+2","/Pictures");
  const auto first=store.load(); HDRSHOT_CHECK(first.has_value());
  const auto migrated=first.value().analysis_defaults;
  HDRSHOT_CHECK(migrated==hdrshot::AnalysisDefaults{});
  auto chosen=migrated;
  chosen.magnifier_enabled=false; chosen.magnifier_zoom=16;
  chosen.hdr_space=hdrshot::analysis::WorkingSpace::bt2020_pq;
  chosen.sdr_space=hdrshot::analysis::WorkingSpace::srgb_sdr;
  chosen.analyzer.selected={true,true,false,true,true,true,true};
  chosen.magnifier.selected.fill(true);
  hdrshot::SettingsPatch patch;patch.analysis_defaults=chosen;
  HDRSHOT_CHECK(store.save(patch).has_value());
  hdrshot::QtSettingsStorePort reopened(path.toStdString(),"ignored","ignored");
  HDRSHOT_CHECK(reopened.load().value().analysis_defaults==chosen);
  hdrshot::SettingsPatch local;
  local.analyzer_preferences_json=R"({"version":1,"white":203,"blur":0,"sampleSize":1,"readings":[false,false,true,false,false],"vectorGain":1.5})";
  HDRSHOT_CHECK(reopened.save(local).has_value());
  const auto after=reopened.load().value();
  HDRSHOT_CHECK(after.analysis_defaults==chosen);
  HDRSHOT_CHECK(after.global_capture_hotkey=="Command+Option+9" && after.detailed_logging);
  HDRSHOT_CHECK(after.analyzer_preferences_json==*local.analyzer_preferences_json);
  const auto before=after;
  chosen.sample_side=2; patch.analysis_defaults=chosen;
  patch.detailed_logging=false;
  HDRSHOT_CHECK(!reopened.save(patch));
  HDRSHOT_CHECK(reopened.load().value()==before); // all fields roll back
}

void analysis_corrupt_values_recover_without_legacy_reentry() {
  QTemporaryDir directory;
  const auto path=directory.filePath("settings.ini");
  QSettings raw(path,QSettings::IniFormat);
  raw.setValue("analyzerPreferences",R"({"version":1,"white":100,"blur":4,"sampleSize":31})");
  raw.setValue("analysisDefaults",R"({"version":1,"white":80,"blur":3,"sampleSize":2,"zoom":0,"hdrSpace":0,"sdrSpace":3})");
  raw.sync();
  hdrshot::QtSettingsStorePort store(path.toStdString(),"Command+Shift+2","/Pictures");
  HDRSHOT_CHECK(store.load().has_value());
  HDRSHOT_CHECK(store.load().value().analysis_defaults==hdrshot::AnalysisDefaults{});
  raw.setValue("analysisDefaults","{broken"); raw.sync();
  HDRSHOT_CHECK(store.load().value().analysis_defaults==hdrshot::AnalysisDefaults{});
}

void invalid_completion_action_fails_explicitly() {
  QTemporaryDir directory;
  HDRSHOT_CHECK(directory.isValid());
  const auto qpath = directory.filePath(QStringLiteral("settings.ini"));
  QSettings settings(qpath, QSettings::IniFormat);
  settings.setValue(QStringLiteral("schemaVersion"), 4);
  settings.setValue(
      QStringLiteral("enterCompletionAction"), QStringLiteral("delete_everything"));
  settings.sync();

  hdrshot::QtSettingsStorePort store(
      qpath.toStdString(), "Command+Shift+2", "/Pictures/SeriousShot");
  const auto loaded = store.load();
  HDRSHOT_CHECK(!loaded.has_value());
  HDRSHOT_CHECK(loaded.error().code == hdrshot::ErrorCode::settings_corrupt);
  HDRSHOT_CHECK(loaded.error().safe_context.at("field") == "enterCompletionAction");
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  const auto args = app.arguments();
  if (args.size() == 5 && args[1] == "--verify-ini") {
    const QSettings raw(args[2], QSettings::IniFormat);
    const auto expected = args[3] == "<absent>" ? QString{} : args[3];
    return raw_language_matches(raw, expected) && raw.value("revision").toInt() == args[4].toInt() &&
        raw.value("detailedLogging").toBool() && raw.value("unrelatedKey") == "original" ? 0 : 1;
  }
  hdrshot::QtUiLanguageRuntime language;
  const auto localized = language.initialize(app, hdrshot::UiLanguage::simplified_chinese,
      [] { return hdrshot::UiLanguage::english; });
  HDRSHOT_CHECK(localized && !localized.value().catalog_error);
  using hdrshot::test::TestCase;
  return hdrshot::test::run(std::vector<TestCase>{
      {"language preference is optional and changed only by a language patch", ui_language_is_optional_and_only_changes_on_a_language_patch},
      {"invalid and mixed language patches leave every key intact", ui_language_rejects_invalid_and_combined_patches_without_mutating},
      {"failed language and revision do not leak into a later save", failed_ui_language_write_cannot_leak_into_the_next_save},
      {"pinned defaults roundtrip and invalid patches are atomic", pinned_defaults_roundtrip_and_invalid_patch_is_atomic},
      {"pinned defaults migrate old config and recover corrupt fields independently", pinned_defaults_migrate_missing_and_recover_corrupt_fields_independently},
      {"scalar readings preserve precision, separate components, deduplicate I and reject nonfinite", scalar_readings_are_separate_precise_and_deduplicated},
      {"magnifier keeps all old storage IDs and maps 13 menu groups independently", magnifier_storage_ids_and_menu_order_are_compatible},
      {"defaults migrate once, persist independently and reject invalid patch", analysis_defaults_roundtrip_migration_and_local_isolation},
      {"corrupt defaults recover without reimporting local preferences", analysis_corrupt_values_recover_without_legacy_reentry},
      {"Analyzer preferences independent and bounded", analyzer_preferences_are_independent_bounded_and_preserved},
      {"Qt settings defaults and persistence", defaults_and_patches_persist_across_instances},
      {"Qt settings rejects invalid Windows gain", invalid_windows_gain_never_commits},
      {"Qt settings migrates legacy Windows gain", legacy_windows_gain_migrates_without_enabling_bypass},
      {"Qt settings keeps Windows compatibility options independent", windows_compatibility_preserves_independent_options},
      {"Qt settings rejects malformed Windows compatibility", malformed_windows_compatibility_fails_without_writing},
      {"Qt settings migrates schema one", schema_one_migrates_old_format_and_defaults_diffuse_white},
      {"Qt settings migrates schema two", schema_two_defaults_hdr_pq_precision},
      {"Qt settings rejects unsupported schema", unsupported_schema_fails_explicitly},
      {"Qt settings rejects invalid completion action", invalid_completion_action_fails_explicitly},
  });
}
