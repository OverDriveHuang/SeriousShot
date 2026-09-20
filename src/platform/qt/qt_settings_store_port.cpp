#include "platform/qt/qt_settings_store_port.hpp"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QString>

#include <map>
#include <utility>

namespace hdrshot {
namespace {

Error settings_error(
    const ErrorCode code,
    const Retryability retryability,
    std::map<std::string, std::string> context = {}) {
  return Error{code, "QtSettingsStorePort", retryability, std::move(context)};
}

Result<bool, Error> check_status(const QSettings& settings, const std::string& path) {
  if (settings.status() == QSettings::NoError) {
    return Result<bool, Error>::success(true);
  }
  const auto code = settings.status() == QSettings::AccessError
      ? ErrorCode::permission_denied
      : ErrorCode::settings_corrupt;
  return Result<bool, Error>::failure(settings_error(
      code,
      Retryability::after_user_action,
      {{"path", path}, {"qtStatus", std::to_string(static_cast<int>(settings.status()))}}));
}

QString save_format_key(const SaveFormat value) {
  switch (value) {
    case SaveFormat::png_display_p3_dual_range:
      return QStringLiteral("png_display_p3_dual_range");
    case SaveFormat::ultra_hdr_jpeg:
      return QStringLiteral("ultra_hdr_jpeg");
  }
  return {};
}

Result<SaveFormat, Error> parse_save_format(const QString& value) {
  if (value == QStringLiteral("png_display_p3_dual_range") ||
      value == QStringLiteral("png_hdr_sdr")) {
    return Result<SaveFormat, Error>::success(
        SaveFormat::png_display_p3_dual_range);
  }
  if (value == QStringLiteral("ultra_hdr_jpeg")) {
    return Result<SaveFormat, Error>::success(SaveFormat::ultra_hdr_jpeg);
  }
  return Result<SaveFormat, Error>::failure(settings_error(
      ErrorCode::settings_corrupt,
      Retryability::after_user_action,
      {{"field", "saveFormat"}}));
}

Result<PqDiffuseWhite, Error> parse_pq_diffuse_white(const unsigned int value) {
  if (value == 100U) {
    return Result<PqDiffuseWhite, Error>::success(PqDiffuseWhite::nits_100);
  }
  if (value == 203U) {
    return Result<PqDiffuseWhite, Error>::success(PqDiffuseWhite::nits_203);
  }
  return Result<PqDiffuseWhite, Error>::failure(settings_error(
      ErrorCode::settings_corrupt,
      Retryability::after_user_action,
      {{"field", "pqDiffuseWhiteNits"}}));
}

Result<HdrPqPrecision, Error> parse_hdr_pq_precision(const unsigned int value) {
  if (value == 10U) {
    return Result<HdrPqPrecision, Error>::success(HdrPqPrecision::bits_10);
  }
  if (value == 12U) {
    return Result<HdrPqPrecision, Error>::success(HdrPqPrecision::bits_12);
  }
  if (value == 16U) {
    return Result<HdrPqPrecision, Error>::success(HdrPqPrecision::bits_16);
  }
  return Result<HdrPqPrecision, Error>::failure(settings_error(
      ErrorCode::settings_corrupt,
      Retryability::after_user_action,
      {{"field", "hdrPqPrecisionBits"}}));
}

Result<UltraHdrJpegQuality, Error> parse_ultra_hdr_jpeg_quality(
    const unsigned int value) {
  switch (value) {
    case 85U:
      return Result<UltraHdrJpegQuality, Error>::success(
          UltraHdrJpegQuality::compact);
    case 95U:
      return Result<UltraHdrJpegQuality, Error>::success(
          UltraHdrJpegQuality::balanced);
    case 100U:
      return Result<UltraHdrJpegQuality, Error>::success(
          UltraHdrJpegQuality::maximum);
    default:
      return Result<UltraHdrJpegQuality, Error>::failure(settings_error(
          ErrorCode::settings_corrupt,
          Retryability::after_user_action,
          {{"field", "ultraHdrJpegQuality"}}));
  }
}

QString completion_action_key(const CompletionAction value) {
  switch (value) {
    case CompletionAction::copy_to_clipboard:
      return QStringLiteral("copy_to_clipboard");
    case CompletionAction::save_default:
      return QStringLiteral("save_default");
    case CompletionAction::save_as:
      return QStringLiteral("save_as");
  }
  return {};
}

Result<CompletionAction, Error> parse_completion_action(
    const QString& value,
    const std::string& field) {
  if (value == QStringLiteral("copy_to_clipboard")) {
    return Result<CompletionAction, Error>::success(
        CompletionAction::copy_to_clipboard);
  }
  if (value == QStringLiteral("save_default")) {
    return Result<CompletionAction, Error>::success(CompletionAction::save_default);
  }
  if (value == QStringLiteral("save_as")) {
    return Result<CompletionAction, Error>::success(CompletionAction::save_as);
  }
  return Result<CompletionAction, Error>::failure(settings_error(
      ErrorCode::settings_corrupt,
      Retryability::after_user_action,
      {{"field", field}}));
}

}  // namespace

QtSettingsStorePort::QtSettingsStorePort(
    std::string exact_path,
    std::string default_hotkey,
    std::string default_save_folder)
    : exact_path_(std::move(exact_path)),
      default_hotkey_(std::move(default_hotkey)),
      default_save_folder_(std::move(default_save_folder)) {}

Result<SettingsSnapshot, Error> QtSettingsStorePort::load() {
  if (exact_path_.empty() || default_hotkey_.empty() || default_save_folder_.empty()) {
    return Result<SettingsSnapshot, Error>::failure(settings_error(
        ErrorCode::invalid_input,
        Retryability::never,
        {{"reason", "missing_settings_contract"}}));
  }
  const bool existing_configuration =
      QFileInfo(QString::fromStdString(exact_path_)).exists();
  QSettings settings(QString::fromStdString(exact_path_), QSettings::IniFormat);
  const auto checked = check_status(settings, exact_path_);
  if (!checked) {
    return Result<SettingsSnapshot, Error>::failure(checked.error());
  }
  const auto schema = settings.value(QStringLiteral("schemaVersion"), 1).toUInt();
  if (schema != 1U && schema != 2U && schema != 3U && schema != 4U &&
      schema != 5U) {
    return Result<SettingsSnapshot, Error>::failure(settings_error(
        ErrorCode::settings_corrupt,
        Retryability::after_user_action,
        {{"path", exact_path_}, {"schemaVersion", std::to_string(schema)}}));
  }
  const auto save_format = parse_save_format(
      settings.value(QStringLiteral("saveFormat"), QStringLiteral("png_hdr_sdr")).toString());
  if (!save_format) {
    return Result<SettingsSnapshot, Error>::failure(save_format.error());
  }
  const auto pq_diffuse_white = parse_pq_diffuse_white(
      settings.value(QStringLiteral("pqDiffuseWhiteNits"), 203U).toUInt());
  if (!pq_diffuse_white) {
    return Result<SettingsSnapshot, Error>::failure(pq_diffuse_white.error());
  }
  const auto hdr_pq_precision = parse_hdr_pq_precision(
      settings.value(QStringLiteral("hdrPqPrecisionBits"), 10U).toUInt());
  if (!hdr_pq_precision) {
    return Result<SettingsSnapshot, Error>::failure(hdr_pq_precision.error());
  }
  const auto ultra_hdr_jpeg_quality = parse_ultra_hdr_jpeg_quality(
      settings.value(QStringLiteral("ultraHdrJpegQuality"), 95U).toUInt());
  if (!ultra_hdr_jpeg_quality) {
    return Result<SettingsSnapshot, Error>::failure(
        ultra_hdr_jpeg_quality.error());
  }
  const auto enter_completion_action = parse_completion_action(
      settings.value(
          QStringLiteral("enterCompletionAction"),
          QStringLiteral("copy_to_clipboard")).toString(),
      "enterCompletionAction");
  if (!enter_completion_action) {
    return Result<SettingsSnapshot, Error>::failure(
        enter_completion_action.error());
  }
  const auto double_click_completion_action = parse_completion_action(
      settings.value(
          QStringLiteral("doubleClickCompletionAction"),
          QStringLiteral("copy_to_clipboard")).toString(),
      "doubleClickCompletionAction");
  if (!double_click_completion_action) {
    return Result<SettingsSnapshot, Error>::failure(
        double_click_completion_action.error());
  }
  const auto analyzer_text = settings.value(QStringLiteral("analyzerPreferences"), QString()).toString();
  std::string analyzer_preferences;
  if (analyzer_text.size() <= 65536) {
    analyzer_preferences = analyzer_text.toStdString();
    if (analyzer_preferences.size() > 65536U) analyzer_preferences.clear();
  }
  return Result<SettingsSnapshot, Error>::success(SettingsSnapshot{
      5,
      settings.value(QStringLiteral("revision"), 0).toULongLong(),
      settings.value(
          QStringLiteral("globalCaptureHotkey"),
          QString::fromStdString(default_hotkey_)).toString().toStdString(),
      settings.value(
          QStringLiteral("defaultSaveFolder"),
          QString::fromStdString(default_save_folder_)).toString().toStdString(),
      save_format.value(),
      pq_diffuse_white.value(),
      hdr_pq_precision.value(),
      ultra_hdr_jpeg_quality.value(),
      enter_completion_action.value(),
      double_click_completion_action.value(),
      settings.value(
          QStringLiteral("initialSettingsPresented"),
          existing_configuration).toBool(),
      settings.value(QStringLiteral("detailedLogging"), false).toBool(),
      std::move(analyzer_preferences),
  });
}

Result<SettingsReceipt, Error> QtSettingsStorePort::save(const SettingsPatch& patch) {
  if (patch.analyzer_preferences_json && patch.analyzer_preferences_json->size() > 65536U)
    return Result<SettingsReceipt, Error>::failure(settings_error(
        ErrorCode::invalid_input, Retryability::never, {{"field", "analyzerPreferences"}}));
  const auto current = load();
  if (!current) {
    return Result<SettingsReceipt, Error>::failure(current.error());
  }
  const QFileInfo file_info(QString::fromStdString(exact_path_));
  if (!QDir().mkpath(file_info.absolutePath())) {
    return Result<SettingsReceipt, Error>::failure(settings_error(
        ErrorCode::permission_denied,
        Retryability::after_user_action,
        {{"path", exact_path_}, {"reason", "parent_directory_unavailable"}}));
  }
  QSettings settings(QString::fromStdString(exact_path_), QSettings::IniFormat);
  settings.setValue(QStringLiteral("schemaVersion"), 5);
  settings.setValue(
      QStringLiteral("revision"),
      static_cast<qulonglong>(current.value().revision + 1U));
  settings.setValue(
      QStringLiteral("globalCaptureHotkey"),
      QString::fromStdString(patch.global_capture_hotkey.value_or(
          current.value().global_capture_hotkey)));
  settings.setValue(
      QStringLiteral("defaultSaveFolder"),
      QString::fromStdString(patch.default_save_folder.value_or(
          current.value().default_save_folder)));
  settings.setValue(
      QStringLiteral("saveFormat"),
      save_format_key(patch.save_format.value_or(current.value().save_format)));
  settings.setValue(
      QStringLiteral("pqDiffuseWhiteNits"),
      static_cast<unsigned int>(patch.pq_diffuse_white.value_or(
          current.value().pq_diffuse_white)));
  settings.setValue(
      QStringLiteral("hdrPqPrecisionBits"),
      static_cast<unsigned int>(patch.hdr_pq_precision.value_or(
          current.value().hdr_pq_precision)));
  settings.setValue(
      QStringLiteral("ultraHdrJpegQuality"),
      static_cast<unsigned int>(patch.ultra_hdr_jpeg_quality.value_or(
          current.value().ultra_hdr_jpeg_quality)));
  settings.setValue(
      QStringLiteral("enterCompletionAction"),
      completion_action_key(patch.enter_completion_action.value_or(
          current.value().enter_completion_action)));
  settings.setValue(
      QStringLiteral("doubleClickCompletionAction"),
      completion_action_key(patch.double_click_completion_action.value_or(
          current.value().double_click_completion_action)));
  settings.setValue(
      QStringLiteral("initialSettingsPresented"),
      patch.initial_settings_presented.value_or(
          current.value().initial_settings_presented));
  settings.setValue(QStringLiteral("detailedLogging"),
      patch.detailed_logging.value_or(current.value().detailed_logging));
  settings.setValue(QStringLiteral("analyzerPreferences"), QString::fromStdString(
      patch.analyzer_preferences_json.value_or(current.value().analyzer_preferences_json)));
  settings.sync();
  const auto checked = check_status(settings, exact_path_);
  if (!checked) {
    return Result<SettingsReceipt, Error>::failure(checked.error());
  }
  return Result<SettingsReceipt, Error>::success(
      SettingsReceipt{current.value().revision + 1U});
}

}  // namespace hdrshot
