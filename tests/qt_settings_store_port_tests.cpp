#include "platform/qt/qt_settings_store_port.hpp"
#include "test_support.hpp"

#include <QSettings>
#include <QTemporaryDir>

#include <string>
#include <vector>

namespace {

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
  hdrshot::SettingsPatch quiet;
  quiet.detailed_logging = false;
  HDRSHOT_CHECK(reopened.save(quiet).has_value());
  HDRSHOT_CHECK(!first.load().value().detailed_logging);
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

int main() {
  using hdrshot::test::TestCase;
  return hdrshot::test::run(std::vector<TestCase>{
      {"Analyzer preferences independent and bounded", analyzer_preferences_are_independent_bounded_and_preserved},
      {"Qt settings defaults and persistence", defaults_and_patches_persist_across_instances},
      {"Qt settings migrates schema one", schema_one_migrates_old_format_and_defaults_diffuse_white},
      {"Qt settings migrates schema two", schema_two_defaults_hdr_pq_precision},
      {"Qt settings rejects unsupported schema", unsupported_schema_fails_explicitly},
      {"Qt settings rejects invalid completion action", invalid_completion_action_fails_explicitly},
  });
}
