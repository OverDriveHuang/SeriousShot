#include <QGroupBox>
#include "ui/qt/settings_window.hpp"
#include "core/build_metadata.hpp"
#include "ui/qt/qt_window_activation_port.hpp"
#include "qt_test_input_platform_adapter.hpp"
#include "test_support.hpp"

#include <QApplication>
#include <QDateTime>
#include <QUrl>
#include <QEvent>
#include <QFont>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QPalette>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyleHints>
#include <QWindow>
#include <QTabWidget>

#include <cstdio>
#include <string>
#include <utility>

namespace {

using namespace hdrshot;

class MemorySettingsStore final : public SettingsStorePort {
 public:
  SettingsSnapshot snapshot{5, 0, "Command+Option+8", "/Pictures/SeriousShot"};
  bool fail_next_save{};
  int save_attempts{};

  Result<SettingsSnapshot, Error> load() override {
    return Result<SettingsSnapshot, Error>::success(snapshot);
  }

  Result<SettingsReceipt, Error> save(const SettingsPatch& patch) override {
    ++save_attempts;
    if (fail_next_save) {
      fail_next_save = false;
      return Result<SettingsReceipt, Error>::failure(Error{
          ErrorCode::permission_denied,
          "MemorySettingsStore",
          Retryability::after_user_action,
          {},
      });
    }
    if (patch.detailed_logging) snapshot.detailed_logging = *patch.detailed_logging;
    if (patch.analysis_defaults) snapshot.analysis_defaults = *patch.analysis_defaults;
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
    if (patch.windows_scrgb_gain) snapshot.windows_scrgb_gain = *patch.windows_scrgb_gain;
    if (patch.windows_capture_gain_enabled)
      snapshot.windows_capture_gain_enabled = *patch.windows_capture_gain_enabled;
    if (patch.windows_capture_bypass_sdr_white_enabled)
      snapshot.windows_capture_bypass_sdr_white_enabled =
          *patch.windows_capture_bypass_sdr_white_enabled;
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

class MemoryGlobalHotkey final : public GlobalHotkeyPort {
 public:
  void set_trigger_handler(HotkeyTriggerHandler handler) override {
    handler_ = std::move(handler);
  }
  Result<HotkeyReceipt, Error> register_hotkey(std::string hotkey) override {
    active_ = std::move(hotkey);
    return Result<HotkeyReceipt, Error>::success(HotkeyReceipt{active_});
  }
  Result<HotkeyReceipt, Error> replace_hotkey(std::string hotkey) override {
    active_ = std::move(hotkey);
    return Result<HotkeyReceipt, Error>::success(HotkeyReceipt{active_});
  }
  Result<HotkeyReceipt, Error> unregister_hotkey() override {
    active_.clear();
    return Result<HotkeyReceipt, Error>::success(HotkeyReceipt{});
  }

  [[nodiscard]] const std::string& active() const noexcept { return active_; }

 private:
  HotkeyTriggerHandler handler_;
  std::string active_;
};

class MemoryFolderOpener final : public FolderOpenerPort {
 public:
  Result<OpenFolderReceipt, Error> open_folder(
      const OpenFolderRequest& request) override {
    requests.push_back(request);
    return Result<OpenFolderReceipt, Error>::success(OpenFolderReceipt{request.exact_path});
  }

  std::vector<OpenFolderRequest> requests;
};

struct Fixture {
  MemorySettingsStore store;
  MemoryGlobalHotkey hotkey;
  MemoryFolderOpener folder_opener;
  hdrshot::test::TestInputPlatformAdapter input_platform;
  SettingsWindow window{
      store,
      hotkey,
      folder_opener,
      input_platform,
      "Command+Shift+2",
      "/Application Support/SeriousShot/logs"};
};

void reset_button_restores_default_and_applies_immediately() {
  Fixture fixture;
  fixture.window.reload_and_show();
  QApplication::processEvents();
  auto* edit = fixture.window.findChild<QKeySequenceEdit*>(QStringLiteral("globalHotkeyEdit"));
  auto* reset = fixture.window.findChild<QPushButton*>(
      QStringLiteral("resetGlobalHotkeyButton"));
  HDRSHOT_CHECK(edit != nullptr);
  HDRSHOT_CHECK(reset != nullptr);
  const auto expected = fixture.input_platform.display_global_hotkey("Command+Shift+2");
  HDRSHOT_CHECK(expected.has_value());
  HDRSHOT_CHECK(edit->keySequence() != expected.value());

  reset->click();

  HDRSHOT_CHECK(edit->keySequence() == expected.value());
  HDRSHOT_CHECK(fixture.store.snapshot.global_capture_hotkey == "Command+Shift+2");
  HDRSHOT_CHECK(fixture.hotkey.active() == "Command+Shift+2");
}

void editor_records_only_after_explicit_click_focus_and_drops_focus_on_deactivate() {
  Fixture fixture;
  fixture.window.reload_and_show();
  QApplication::processEvents();
  auto* edit = fixture.window.findChild<QKeySequenceEdit*>(QStringLiteral("globalHotkeyEdit"));
  HDRSHOT_CHECK(edit != nullptr);
  HDRSHOT_CHECK(edit->focusPolicy() == Qt::NoFocus);
  HDRSHOT_CHECK(edit->testAttribute(Qt::WA_TransparentForMouseEvents));
  HDRSHOT_CHECK(QApplication::focusWidget() != edit);

  auto* customize = fixture.window.findChild<QPushButton*>(
      QStringLiteral("customizeGlobalHotkeyButton"));
  HDRSHOT_CHECK(customize != nullptr);
  customize->click();
  QApplication::processEvents();
  HDRSHOT_CHECK(QApplication::focusWidget() == edit);
  HDRSHOT_CHECK(edit->focusPolicy() == Qt::StrongFocus);
  QEvent deactivate(QEvent::WindowDeactivate);
  QApplication::sendEvent(&fixture.window, &deactivate);
  HDRSHOT_CHECK(QApplication::focusWidget() != edit);

  const auto unchanged = edit->keySequence();
  QKeyEvent unrelated_key(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
  QApplication::sendEvent(&fixture.window, &unrelated_key);
  HDRSHOT_CHECK(edit->keySequence() == unchanged);
}

void custom_button_is_the_only_entry_to_recording_and_command_round_trips() {
  Fixture fixture;
  fixture.window.reload_and_show();
  QApplication::processEvents();
  auto* edit = fixture.window.findChild<QKeySequenceEdit*>(QStringLiteral("globalHotkeyEdit"));
  auto* customize = fixture.window.findChild<QPushButton*>(
      QStringLiteral("customizeGlobalHotkeyButton"));
  auto* folder = fixture.window.findChild<QLineEdit*>(
      QStringLiteral("defaultSaveFolderEdit"));
  HDRSHOT_CHECK(edit != nullptr);
  HDRSHOT_CHECK(customize != nullptr);
  HDRSHOT_CHECK(folder != nullptr);
  HDRSHOT_CHECK(fixture.window.findChild<QPushButton*>(
                    QStringLiteral("applySettingsButton")) == nullptr);

  const auto original = edit->keySequence();
  QKeyEvent accidental(QEvent::KeyPress, Qt::Key_2,
      Qt::ControlModifier | Qt::ShiftModifier);
  QApplication::sendEvent(&fixture.window, &accidental);
  HDRSHOT_CHECK(edit->keySequence() == original);

  customize->click();
  QApplication::processEvents();
  QKeyEvent command_shift_2(QEvent::KeyPress, Qt::Key_2,
      Qt::ControlModifier | Qt::ShiftModifier);
  QApplication::sendEvent(edit, &command_shift_2);
  QKeyEvent command_shift_2_release(QEvent::KeyRelease, Qt::Key_2,
      Qt::ControlModifier | Qt::ShiftModifier);
  QApplication::sendEvent(edit, &command_shift_2_release);
  QApplication::processEvents();
  const auto canonical = fixture.input_platform.canonical_global_hotkey(
      edit->keySequence());
  HDRSHOT_CHECK(canonical.has_value());
  HDRSHOT_CHECK(canonical.value() == "Command+Shift+2");
  HDRSHOT_CHECK(edit->focusPolicy() == Qt::NoFocus);
  HDRSHOT_CHECK(fixture.store.snapshot.global_capture_hotkey == "Command+Shift+2");
  HDRSHOT_CHECK(fixture.hotkey.active() == "Command+Shift+2");

  customize->click();
  QApplication::processEvents();
  HDRSHOT_CHECK(edit->keySequence().isEmpty());
  QMouseEvent outside(
      QEvent::MouseButtonPress, QPointF(2, 2), QPointF(2, 2), QPointF(2, 2),
      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(folder, &outside);
  HDRSHOT_CHECK(edit->keySequence() != QKeySequence{});
  HDRSHOT_CHECK(edit->focusPolicy() == Qt::NoFocus);
}

void general_rows_are_wide_aligned_and_folder_actions_are_explicit() {
  Fixture fixture;
  fixture.window.reload_and_show();
  QApplication::processEvents();
  auto* hotkey = fixture.window.findChild<QKeySequenceEdit*>(QStringLiteral("globalHotkeyEdit"));
  auto* folder = fixture.window.findChild<QLineEdit*>(QStringLiteral("defaultSaveFolderEdit"));
  auto* customize = fixture.window.findChild<QPushButton*>(
      QStringLiteral("customizeGlobalHotkeyButton"));
  auto* reset = fixture.window.findChild<QPushButton*>(
      QStringLiteral("resetGlobalHotkeyButton"));
  auto* choose = fixture.window.findChild<QPushButton*>(
      QStringLiteral("chooseDefaultSaveFolderButton"));
  auto* open = fixture.window.findChild<QPushButton*>(
      QStringLiteral("openDefaultSaveFolderButton"));
  auto* open_logs = fixture.window.findChild<QPushButton*>(
      QStringLiteral("openLogsFolderButton"));
  auto* build_timestamp = fixture.window.findChild<QLabel*>(
      QStringLiteral("buildIdentityLabel"));
  auto* format = fixture.window.findChild<QComboBox*>(QStringLiteral("saveFormatCombo"));
  auto* diffuse_white = fixture.window.findChild<QComboBox*>(
      QStringLiteral("pqDiffuseWhiteCombo"));
  auto* precision = fixture.window.findChild<QComboBox*>(
      QStringLiteral("hdrPqPrecisionCombo"));
  auto* ultra_hdr_quality = fixture.window.findChild<QComboBox*>(
      QStringLiteral("ultraHdrJpegQualityCombo"));
  auto* enter_action = fixture.window.findChild<QComboBox*>(
      QStringLiteral("enterCompletionActionCombo"));
  auto* double_click_action = fixture.window.findChild<QComboBox*>(
      QStringLiteral("doubleClickCompletionActionCombo"));
  auto* author = fixture.window.findChild<QLabel*>(QStringLiteral("authorLabel"));
  auto* description = fixture.window.findChild<QLabel*>(
      QStringLiteral("settingsDescriptionLabel"));
  auto* general_label = fixture.window.findChild<QLabel*>(
      QStringLiteral("generalSectionLabel"));
  auto* fixed_label = fixture.window.findChild<QLabel*>(
      QStringLiteral("fixedOperationsSectionLabel"));
  auto* general_container = fixture.window.findChild<QGroupBox*>(
      QStringLiteral("generalSettingsContainer"));
  auto* fixed_container = fixture.window.findChild<QGroupBox*>(
      QStringLiteral("fixedOperationsContainer"));
  HDRSHOT_CHECK(hotkey != nullptr && folder != nullptr && customize != nullptr);
  HDRSHOT_CHECK(reset != nullptr && choose != nullptr && open != nullptr);
  HDRSHOT_CHECK(open_logs != nullptr && format != nullptr && diffuse_white != nullptr &&
                precision != nullptr && ultra_hdr_quality != nullptr &&
                enter_action != nullptr &&
                double_click_action != nullptr);
  HDRSHOT_CHECK(build_timestamp != nullptr);
  HDRSHOT_CHECK(author != nullptr && author->text() == QStringLiteral("by Overdrive"));
  HDRSHOT_CHECK(description != nullptr && general_label != nullptr && fixed_label != nullptr);
  HDRSHOT_CHECK(general_container != nullptr && fixed_container != nullptr);
  HDRSHOT_CHECK(fixture.window.findChild<QWidget*>(
                    QStringLiteral("completionActionSeparator")) == nullptr);
  HDRSHOT_CHECK(fixture.window.windowTitle() == QStringLiteral("SeriousShot 设置"));
  const auto description_x = description->mapTo(&fixture.window, QPoint{}).x();
  HDRSHOT_CHECK(general_label->mapTo(&fixture.window, QPoint{}).x() >= description_x);
  HDRSHOT_CHECK(fixed_label->mapTo(&fixture.window, QPoint{}).x() ==
      general_label->mapTo(&fixture.window, QPoint{}).x());
  HDRSHOT_CHECK(general_label->font().pointSizeF() == description->font().pointSizeF());
  HDRSHOT_CHECK(fixed_label->font().pointSizeF() == description->font().pointSizeF());
  HDRSHOT_CHECK(general_label->geometry().bottom() < general_container->geometry().top());
  HDRSHOT_CHECK(fixed_label->geometry().bottom() < fixed_container->geometry().top());
  HDRSHOT_CHECK(hotkey->width() >= 160);
  HDRSHOT_CHECK(folder->width() >= 160);
  HDRSHOT_CHECK(
      hotkey->mapTo(&fixture.window, QPoint{hotkey->width(), 0}).x() <=
      customize->mapTo(&fixture.window, QPoint{}).x());
  HDRSHOT_CHECK(
      folder->mapTo(&fixture.window, QPoint{folder->width(), 0}).x() <=
      choose->mapTo(&fixture.window, QPoint{}).x());
  HDRSHOT_CHECK(customize->width() == choose->width());
  HDRSHOT_CHECK(reset->width() == open->width());
  HDRSHOT_CHECK(customize->mapTo(&fixture.window, QPoint{}).x() ==
                choose->mapTo(&fixture.window, QPoint{}).x());
  HDRSHOT_CHECK(reset->mapTo(&fixture.window, QPoint{}).x() ==
                open->mapTo(&fixture.window, QPoint{}).x());
  HDRSHOT_CHECK(format->currentText() ==
                QStringLiteral("PNG（SDR：Display P3 ICC；HDR：Display P3 PQ）"));
  HDRSHOT_CHECK(diffuse_white->currentText() == QStringLiteral("203 nit"));
  HDRSHOT_CHECK(diffuse_white->toolTip().contains(QStringLiteral("只影响 HDR")));
  HDRSHOT_CHECK(precision->currentText() == QStringLiteral("小体积（10-bit PQ，默认）"));
  HDRSHOT_CHECK(precision->toolTip().contains(QStringLiteral("16-bit RGB")));
  HDRSHOT_CHECK(!ultra_hdr_quality->isVisible());
  HDRSHOT_CHECK(
      ultra_hdr_quality->currentText() == QStringLiteral("均衡（95，默认）"));
  HDRSHOT_CHECK(enter_action->currentText() == QStringLiteral("复制到剪贴板"));
  HDRSHOT_CHECK(double_click_action->currentText() == QStringLiteral("复制到剪贴板"));
  HDRSHOT_CHECK(folder->toolTip() == folder->text());
  HDRSHOT_CHECK(build_timestamp->text().startsWith(QStringLiteral("版本 ") +
      QString::fromLatin1(hdrshot::product_version().data(), static_cast<qsizetype>(hdrshot::product_version().size()))));
  HDRSHOT_CHECK(!build_timestamp->text().contains(QStringLiteral("构建时间")));
  const auto timestamp = QDateTime::fromString(QString::fromLatin1(
      hdrshot::source_commit_timestamp().data(), static_cast<qsizetype>(hdrshot::source_commit_timestamp().size())), Qt::ISODate);
  HDRSHOT_CHECK(build_timestamp->text().contains(timestamp.isValid()
      ? timestamp.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'")) : QStringLiteral("未知")));
  HDRSHOT_CHECK(build_timestamp->text().contains(QStringLiteral("含未提交修改")) == hdrshot::source_modified());
  HDRSHOT_CHECK(
      build_timestamp->palette().color(QPalette::WindowText) ==
      fixture.window.palette().color(QPalette::WindowText));
  HDRSHOT_CHECK(
      build_timestamp->mapTo(&fixture.window, QPoint{}).x() <
      open_logs->mapTo(&fixture.window, QPoint{}).x());

  open->click();
  open_logs->click();
  HDRSHOT_CHECK(fixture.folder_opener.requests.size() == 2);
  HDRSHOT_CHECK(fixture.folder_opener.requests[0].exact_path == "/Pictures/SeriousShot");
  HDRSHOT_CHECK(fixture.folder_opener.requests[1].exact_path ==
                "/Application Support/SeriousShot/logs");
}

void completion_actions_and_precision_apply_immediately() {
  Fixture fixture;
  fixture.window.reload_and_show();
  QApplication::processEvents();
  auto* enter_action = fixture.window.findChild<QComboBox*>(
      QStringLiteral("enterCompletionActionCombo"));
  auto* double_click_action = fixture.window.findChild<QComboBox*>(
      QStringLiteral("doubleClickCompletionActionCombo"));
  auto* precision = fixture.window.findChild<QComboBox*>(
      QStringLiteral("hdrPqPrecisionCombo"));
  HDRSHOT_CHECK(enter_action != nullptr && double_click_action != nullptr &&
                precision != nullptr);

  enter_action->setCurrentIndex(enter_action->findData(
      static_cast<int>(CompletionAction::save_default)));
  HDRSHOT_CHECK(
      fixture.store.snapshot.enter_completion_action == CompletionAction::save_default);
  HDRSHOT_CHECK(
      fixture.store.snapshot.double_click_completion_action ==
      CompletionAction::copy_to_clipboard);

  double_click_action->setCurrentIndex(double_click_action->findData(
      static_cast<int>(CompletionAction::save_as)));
  HDRSHOT_CHECK(
      fixture.store.snapshot.double_click_completion_action == CompletionAction::save_as);

  precision->setCurrentIndex(precision->findData(
      static_cast<int>(HdrPqPrecision::bits_16)));
  HDRSHOT_CHECK(fixture.store.snapshot.hdr_pq_precision == HdrPqPrecision::bits_16);
}

void windows_capture_compatibility_controls_are_transactional() {
#ifdef Q_OS_WIN
  Fixture fixture;
  fixture.store.snapshot.windows_capture_gain_enabled = false;
  fixture.store.snapshot.windows_capture_bypass_sdr_white_enabled = false;
  fixture.store.snapshot.windows_scrgb_gain = 0.5;
  int notifications = 0;
  fixture.window.set_applied([&](const SettingsSnapshot&) { ++notifications; });
  fixture.window.reload_and_show();
  QApplication::processEvents();
  auto* gain = fixture.window.findChild<QLineEdit*>("windowsScRgbGainEdit");
  auto* parent = fixture.window.findChild<QCheckBox*>(
      "windowsCaptureCompatibilityCheckBox");
  auto* gain_check = fixture.window.findChild<QCheckBox*>(
      "windowsCaptureGainCheckBox");
  auto* bypass = fixture.window.findChild<QCheckBox*>(
      "windowsCaptureBypassSdrWhiteCheckBox");
  auto* help = fixture.window.findChild<QLabel*>(
      "windowsCaptureCompatibilityHelp");
  HDRSHOT_CHECK(gain && parent && gain_check && bypass && help);
  HDRSHOT_CHECK(gain_check->text() == QStringLiteral("scRGB gain ="));
  HDRSHOT_CHECK(help->text() == QStringLiteral(
      "部分 Windows 10 版本截图亮度异常时可尝试开启，默认关闭。"));
  HDRSHOT_CHECK(parent->checkState() == Qt::Unchecked);
  HDRSHOT_CHECK(!gain_check->isEnabled() && !bypass->isEnabled() && !gain->isEnabled());
  HDRSHOT_CHECK(gain->text() == "0.5");

  const auto click_once = [&](QCheckBox* control) {
    const auto saves = fixture.store.save_attempts;
    const auto notices = notifications;
    control->click();
    HDRSHOT_CHECK(fixture.store.save_attempts == saves + 1);
    HDRSHOT_CHECK(notifications == notices + 1);
  };
  click_once(parent);
  HDRSHOT_CHECK(parent->checkState() == Qt::Checked);
  HDRSHOT_CHECK(gain_check->isChecked() && bypass->isChecked());
  HDRSHOT_CHECK(gain_check->isEnabled() && bypass->isEnabled() && gain->isEnabled());
  click_once(gain_check);
  HDRSHOT_CHECK(parent->checkState() == Qt::PartiallyChecked);
  HDRSHOT_CHECK(!gain_check->isChecked() && bypass->isChecked() && !gain->isEnabled());
  click_once(gain_check);
  HDRSHOT_CHECK(parent->checkState() == Qt::Checked);
  click_once(bypass);
  HDRSHOT_CHECK(parent->checkState() == Qt::PartiallyChecked);
  click_once(gain_check);
  HDRSHOT_CHECK(parent->checkState() == Qt::Unchecked);
  HDRSHOT_CHECK(!gain_check->isEnabled() && !bypass->isEnabled());
  click_once(parent);
  HDRSHOT_CHECK(parent->checkState() == Qt::Checked);
  click_once(parent);
  HDRSHOT_CHECK(parent->checkState() == Qt::Unchecked);
  HDRSHOT_CHECK(fixture.store.snapshot.windows_scrgb_gain == 0.5);

  fixture.store.snapshot.windows_capture_gain_enabled = true;
  fixture.store.snapshot.windows_capture_bypass_sdr_white_enabled = false;
  fixture.window.reload_and_show();
  HDRSHOT_CHECK(parent->checkState() == Qt::PartiallyChecked);
  click_once(parent);
  HDRSHOT_CHECK(parent->checkState() == Qt::Checked);
  HDRSHOT_CHECK(gain_check->isChecked() && bypass->isChecked());

  gain->setFocus();
  gain->setText("1.0");
  gain->clearFocus();
  QApplication::processEvents();
  HDRSHOT_CHECK(fixture.store.snapshot.windows_scrgb_gain == 1.0);
  HDRSHOT_CHECK(gain_check->isChecked() && parent->checkState() == Qt::Checked);
  click_once(parent);
  click_once(parent);
  HDRSHOT_CHECK(gain->text() == "1.0");
  HDRSHOT_CHECK(fixture.store.snapshot.windows_scrgb_gain == 1.0);
  gain->setFocus();
  gain->setText("2.5");
  QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(gain, &escape);
  HDRSHOT_CHECK(gain->text() == "1.0");
  HDRSHOT_CHECK(fixture.store.snapshot.windows_scrgb_gain == 1.0);

  fixture.store.fail_next_save = true;
  const auto before_failure = notifications;
  bypass->click();
  HDRSHOT_CHECK(parent->checkState() == Qt::Checked && bypass->isChecked());
  HDRSHOT_CHECK(notifications == before_failure);
  gain->setFocus();
  gain->setText("nan");
  bypass->click();
  HDRSHOT_CHECK(gain->text() == "1.0" && bypass->isChecked());
  HDRSHOT_CHECK(notifications == before_failure);
  fixture.store.snapshot.windows_capture_gain_enabled = false;
  fixture.store.snapshot.windows_capture_bypass_sdr_white_enabled = true;
  fixture.window.reload_and_show();
  HDRSHOT_CHECK(parent->checkState() == Qt::PartiallyChecked);
  HDRSHOT_CHECK(!gain_check->isChecked() && bypass->isChecked());
  HDRSHOT_CHECK(!gain->isEnabled());
  fixture.store.snapshot.windows_scrgb_gain = 1.1234567890123;
  fixture.window.reload_and_show();
  click_once(parent);
  HDRSHOT_CHECK(fixture.store.snapshot.windows_scrgb_gain == 1.1234567890123);
#else
  Fixture fixture;
  HDRSHOT_CHECK(fixture.window.findChild<QLineEdit*>("windowsScRgbGainEdit") == nullptr);
  HDRSHOT_CHECK(fixture.window.findChild<QCheckBox*>("windowsCaptureCompatibilityCheckBox") == nullptr);
#endif
}

void windows_capture_click_commits_pending_gain_once() {
#ifdef Q_OS_WIN
  Fixture fixture;
  fixture.store.snapshot.windows_capture_gain_enabled = true;
  fixture.store.snapshot.windows_capture_bypass_sdr_white_enabled = true;
  fixture.store.snapshot.windows_scrgb_gain = 0.5;
  int notifications = 0;
  fixture.window.set_applied([&](const SettingsSnapshot&) { ++notifications; });
  fixture.window.reload_and_show();
  QApplication::processEvents();
  auto* gain = fixture.window.findChild<QLineEdit*>("windowsScRgbGainEdit");
  auto* bypass = fixture.window.findChild<QCheckBox*>(
      "windowsCaptureBypassSdrWhiteCheckBox");
  auto* scroll = fixture.window.findChild<QScrollArea*>(
      "generalSettingsScrollArea");
  HDRSHOT_CHECK(gain && bypass && scroll);
  scroll->ensureWidgetVisible(bypass);
  QApplication::processEvents();
  gain->setFocus();
  HDRSHOT_CHECK(gain->hasFocus());
  gain->setText("1.274008");
  const auto saves = fixture.store.save_attempts;
  const QPoint local{8, bypass->height() / 2};
  const QPoint window_local = bypass->mapTo(&fixture.window, local);
  const QPoint global = bypass->mapToGlobal(local);
  QMouseEvent press(QEvent::MouseButtonPress, QPointF(window_local), QPointF(global),
      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(fixture.window.windowHandle(), &press);
  HDRSHOT_CHECK(!gain->hasFocus());
  HDRSHOT_CHECK(fixture.store.save_attempts == saves);
  QMouseEvent release(QEvent::MouseButtonRelease, QPointF(window_local), QPointF(global),
      Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(fixture.window.windowHandle(), &release);
  QApplication::processEvents();
  HDRSHOT_CHECK(fixture.store.save_attempts == saves + 1);
  HDRSHOT_CHECK(notifications == 1);
  HDRSHOT_CHECK(fixture.store.snapshot.windows_scrgb_gain == 1.274008);
  HDRSHOT_CHECK(fixture.store.snapshot.windows_capture_gain_enabled);
  HDRSHOT_CHECK(!fixture.store.snapshot.windows_capture_bypass_sdr_white_enabled);
  HDRSHOT_CHECK(gain->text() == "1.274008");
#endif
}

void failed_auto_apply_restores_the_last_valid_snapshot() {
  Fixture fixture;
  fixture.window.reload_and_show();
  QApplication::processEvents();
  auto* enter_action = fixture.window.findChild<QComboBox*>(
      QStringLiteral("enterCompletionActionCombo"));
  HDRSHOT_CHECK(enter_action != nullptr);
  HDRSHOT_CHECK(
      fixture.store.snapshot.enter_completion_action ==
      CompletionAction::copy_to_clipboard);

  fixture.store.fail_next_save = true;
  enter_action->setCurrentIndex(enter_action->findData(
      static_cast<int>(CompletionAction::save_default)));
  HDRSHOT_CHECK(
      fixture.store.snapshot.enter_completion_action ==
      CompletionAction::copy_to_clipboard);
  HDRSHOT_CHECK(
      enter_action->currentData().toInt() ==
      static_cast<int>(CompletionAction::copy_to_clipboard));
}

void save_format_switches_only_the_relevant_quality_rows() {
  Fixture fixture;
  SettingsSnapshot applied = fixture.store.snapshot;
  fixture.window.set_applied([&](const SettingsSnapshot& snapshot) { applied = snapshot; });
  fixture.window.reload_and_show();
  QApplication::processEvents();
  QApplication::processEvents();
  auto* format = fixture.window.findChild<QComboBox*>(QStringLiteral("saveFormatCombo"));
  auto* diffuse_white = fixture.window.findChild<QComboBox*>(
      QStringLiteral("pqDiffuseWhiteCombo"));
  auto* precision = fixture.window.findChild<QComboBox*>(
      QStringLiteral("hdrPqPrecisionCombo"));
  auto* quality = fixture.window.findChild<QComboBox*>(
      QStringLiteral("ultraHdrJpegQualityCombo"));
  HDRSHOT_CHECK(
      format != nullptr && diffuse_white != nullptr && precision != nullptr &&
      quality != nullptr);
  const auto png_position = format->mapTo(&fixture.window, QPoint{});
  const auto png_width = format->width();
  const auto png_viewport_width = fixture.window.findChild<QScrollArea*>("generalSettingsScrollArea")->viewport()->width();
  auto* format_scroll = fixture.window.findChild<QScrollArea*>("generalSettingsScrollArea");

  format->setCurrentIndex(format->findData(
      static_cast<int>(SaveFormat::ultra_hdr_jpeg)));
  // The next capture reads this callback snapshot immediately, without another event turn.
  HDRSHOT_CHECK(applied.save_format == SaveFormat::ultra_hdr_jpeg);
  QApplication::processEvents();
  HDRSHOT_CHECK(fixture.store.snapshot.save_format == SaveFormat::ultra_hdr_jpeg);
  HDRSHOT_CHECK(format->currentText() == QStringLiteral("JPEG（ISO 21496-1 + XMP）"));
  HDRSHOT_CHECK(!diffuse_white->isVisible());
  HDRSHOT_CHECK(!precision->isVisible());
  HDRSHOT_CHECK(quality->isVisible());
  HDRSHOT_CHECK(format->mapTo(&fixture.window, QPoint{}) == png_position);
  HDRSHOT_CHECK(format->width() == png_width);
  HDRSHOT_CHECK(quality->mapTo(&fixture.window, QPoint{}).x() == png_position.x());

  quality->setCurrentIndex(quality->findData(
      static_cast<int>(UltraHdrJpegQuality::compact)));
  HDRSHOT_CHECK(
      fixture.store.snapshot.ultra_hdr_jpeg_quality ==
      UltraHdrJpegQuality::compact);
  format->setCurrentIndex(format->findData(
      static_cast<int>(SaveFormat::png_display_p3_dual_range)));
  HDRSHOT_CHECK(applied.save_format == SaveFormat::png_display_p3_dual_range);
  QApplication::processEvents();
  HDRSHOT_CHECK(diffuse_white->isVisible());
  HDRSHOT_CHECK(precision->isVisible());
  HDRSHOT_CHECK(!quality->isVisible());
  HDRSHOT_CHECK(format->mapTo(&fixture.window, QPoint{}) == png_position);
  HDRSHOT_CHECK(format_scroll);
  HDRSHOT_CHECK(format->width() - png_width ==
      format_scroll->viewport()->width() - png_viewport_width);
  HDRSHOT_CHECK(
      fixture.store.snapshot.ultra_hdr_jpeg_quality ==
      UltraHdrJpegQuality::compact);
}

void general_sections_scroll_together_without_overlap() {
  Fixture fixture;
  fixture.window.setMinimumHeight(480);
  fixture.window.reload_and_show();
  for (int height : {770, 730, 480}) {
    fixture.window.resize(860, height);
    HDRSHOT_CHECK(fixture.window.height()==height);
    int previous_range=-1;
    for (int format_value : {1, 0, 1, 0}) {
      auto* format = fixture.window.findChild<QComboBox*>("saveFormatCombo");
      format->setCurrentIndex(format_value);
      auto* status = fixture.window.findChild<QLabel*>("settingsStatusLabel");
      for (bool show_status : {true, false}) {
        status->setVisible(show_status);
        for (int i=0; i<6; ++i) QApplication::processEvents();
        auto* scroll=fixture.window.findChild<QScrollArea*>("generalSettingsScrollArea");
        HDRSHOT_CHECK(scroll);
        auto* general=fixture.window.findChild<QGroupBox*>("generalSettingsContainer");
        auto* fixed=fixture.window.findChild<QGroupBox*>("fixedOperationsContainer");
        auto* label=fixture.window.findChild<QLabel*>("fixedOperationsSectionLabel");
        auto* content=scroll->widget();
        HDRSHOT_CHECK(content->isAncestorOf(general) && content->isAncestorOf(fixed));
        HDRSHOT_CHECK(general->mapTo(content,QPoint(0,general->height())).y()+12 <= label->mapTo(content,QPoint{}).y());
        HDRSHOT_CHECK(label->mapTo(content,QPoint(0,label->height())).y() <= fixed->mapTo(content,QPoint{}).y());
        HDRSHOT_CHECK(general->height() >= general->minimumSizeHint().height());
        HDRSHOT_CHECK(fixed->height() >= fixed->minimumSizeHint().height());
        if (height==480) HDRSHOT_CHECK(scroll->verticalScrollBar()->maximum()>0);
        if (show_status && height==480 && previous_range>=0) {
          const auto save_format=static_cast<SaveFormat>(format->currentData().toInt());
          if (save_format==SaveFormat::png_display_p3_dual_range)
            HDRSHOT_CHECK(scroll->verticalScrollBar()->maximum()>previous_range);
          else HDRSHOT_CHECK(scroll->verticalScrollBar()->maximum()<previous_range);
        }
        if (show_status) previous_range=scroll->verticalScrollBar()->maximum();
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        for (int i=0; i<3; ++i) QApplication::processEvents();
        const int bottom=fixed->mapTo(scroll->viewport(),QPoint(0,fixed->height())).y();
        HDRSHOT_CHECK(bottom<=scroll->viewport()->height());
        auto* page=scroll->parentWidget();
        HDRSHOT_CHECK(page->height()-fixed->mapTo(page,QPoint(0,fixed->height())).y() >= page->layout()->contentsMargins().bottom());
        scroll->verticalScrollBar()->setValue(0);
      }
    }
  }
}

void iso_jpeg_label_preserves_existing_format_and_quality() {
  Fixture fixture;
  // No persisted enum/schema migration for a display-name change.
  fixture.store.snapshot.save_format = SaveFormat::ultra_hdr_jpeg;
  fixture.store.snapshot.ultra_hdr_jpeg_quality = UltraHdrJpegQuality::maximum;
  const auto revision = fixture.store.snapshot.revision;
  fixture.window.reload_and_show();
  QApplication::processEvents();
  auto* format = fixture.window.findChild<QComboBox*>(QStringLiteral("saveFormatCombo"));
  auto* quality = fixture.window.findChild<QComboBox*>(QStringLiteral("ultraHdrJpegQualityCombo"));
  HDRSHOT_CHECK(format && quality);
  HDRSHOT_CHECK(format->currentText() == QStringLiteral("JPEG（ISO 21496-1 + XMP）"));
  HDRSHOT_CHECK(format->currentData().toInt() == static_cast<int>(SaveFormat::ultra_hdr_jpeg));
  HDRSHOT_CHECK(quality->currentData().toInt() == static_cast<int>(UltraHdrJpegQuality::maximum));
  HDRSHOT_CHECK(fixture.store.snapshot.revision == revision);
}

void fresh_settings_window_ignores_legacy_presentation_flag_and_close_stays_closed() {
  for (const bool previously_presented : {false, true}) {
    for (int launch = 0; launch < 2; ++launch) {
      Fixture fixture;
      fixture.store.snapshot.initial_settings_presented = previously_presented;
      fixture.store.snapshot.revision = 42;
      fixture.store.snapshot.save_format = SaveFormat::ultra_hdr_jpeg;
      fixture.window.reload_and_show();
      QApplication::processEvents();
      HDRSHOT_CHECK(fixture.window.isVisible());
      HDRSHOT_CHECK(fixture.store.snapshot.revision == 42U);
      HDRSHOT_CHECK(fixture.store.snapshot.initial_settings_presented == previously_presented);
      HDRSHOT_CHECK(fixture.store.snapshot.save_format == SaveFormat::ultra_hdr_jpeg);
      fixture.window.close();
      QApplication::processEvents();
      HDRSHOT_CHECK(!fixture.window.isVisible());
    }
  }
}

void every_settings_open_uses_injected_window_activation() {
  class Activation final : public hdrshot::QtWindowActivationPort {
   public:
    int calls{};
    void activate(QWidget& window) override { ++calls; window.showNormal(); }
  } activation;
  Fixture fixture;
  fixture.window.set_window_activation_port(&activation);
  fixture.window.reload_and_show();
  HDRSHOT_CHECK(activation.calls == 1);
  HDRSHOT_CHECK(fixture.window.isVisible());
  fixture.window.hide();
  fixture.window.reload_and_show();
  HDRSHOT_CHECK(activation.calls == 2);
  HDRSHOT_CHECK(fixture.window.isVisible());
  // A resident reuses this window after close, capture hides and minimization.
  // None of those states should require a capture to make settings reopen.
  for(int cycle=0;cycle<3;++cycle) {
    fixture.window.close();
    fixture.window.reload_and_show();
    HDRSHOT_CHECK(fixture.window.isVisible());
    fixture.window.showMinimized();
    fixture.window.hide();
    fixture.window.reload_and_show();
    HDRSHOT_CHECK(fixture.window.isVisible());
    HDRSHOT_CHECK(!fixture.window.isMinimized());
  }
  HDRSHOT_CHECK(activation.calls == 8);
}

void detailed_logging_applies_and_rolls_back_on_failure() {
  Fixture fixture;
  bool active = false; int changes=0;
  fixture.window.set_applied([&](const SettingsSnapshot& settings) {
    active=settings.detailed_logging; ++changes;
  });
  fixture.window.reload_and_show(); QApplication::processEvents();
  auto* check=fixture.window.findChild<QCheckBox*>("detailedLoggingCheckBox");
  auto* logs=fixture.window.findChild<QPushButton*>("openLogsFolderButton");
  auto* build=fixture.window.findChild<QLabel*>("buildIdentityLabel");
  HDRSHOT_CHECK(check && logs && build);
  HDRSHOT_CHECK(!check->isChecked());
  HDRSHOT_CHECK(check->geometry().right() < logs->geometry().left());
  HDRSHOT_CHECK(build->geometry().bottom() < check->geometry().top());
  check->click();
  HDRSHOT_CHECK(active && fixture.store.snapshot.detailed_logging && changes==1);
  fixture.window.reload_and_show();
  HDRSHOT_CHECK(check->isChecked() && changes==1);
  fixture.store.fail_next_save=true;
  check->click();
  HDRSHOT_CHECK(check->isChecked() && active && changes==1);
  check->click();
  HDRSHOT_CHECK(!active && !fixture.store.snapshot.detailed_logging && changes==2);
}
void analysis_tab_defaults_are_shared_independent_and_rollback_immediately() {
  Fixture f;
  f.window.reload_and_show(); QApplication::processEvents();
  auto* tabs=f.window.findChild<QTabWidget*>("settingsTabs");
  HDRSHOT_CHECK(tabs && tabs->count()==2 && tabs->tabText(0)=="通用" && tabs->tabText(1)=="分析");
  tabs->setCurrentIndex(1); QApplication::processEvents();
  for(const auto* name : {"analysisHdrSpaceCombo","analysisSdrSpaceCombo","analysisWhiteCombo","analysisBlurCombo","analysisSampleCombo","magnifierZoomCombo"})
    HDRSHOT_CHECK(f.window.findChildren<QComboBox*>(name).size()==1);
  auto* enabled=f.window.findChild<QCheckBox*>("magnifierEnabledCheck");
  auto* zoom=f.window.findChild<QComboBox*>("magnifierZoomCombo");
  auto* blur=f.window.findChild<QComboBox*>("analysisBlurCombo");
  auto* sample=f.window.findChild<QComboBox*>("analysisSampleCombo");
  HDRSHOT_CHECK(enabled->isChecked() && zoom->currentData().toInt()==8);
  HDRSHOT_CHECK(blur->currentData().toDouble()==0 && sample->currentData().toInt()==1);
  auto* magnifier=f.window.findChild<QCheckBox*>("magnifierReadingsOption0");
  auto* analyzer=f.window.findChild<QCheckBox*>("analyzerReadingsOption0");
  HDRSHOT_CHECK(magnifier->isChecked() && !analyzer->isChecked());
  magnifier->click(); HDRSHOT_CHECK(!f.store.snapshot.analysis_defaults.magnifier.selected[0]);
  HDRSHOT_CHECK(!analyzer->isChecked() && !f.store.snapshot.analysis_defaults.analyzer.selected[0]);
  f.store.fail_next_save=true;
  enabled->click();
  HDRSHOT_CHECK(enabled->isChecked() && f.store.snapshot.analysis_defaults.magnifier_enabled);
  f.store.fail_next_save=true;
  zoom->setCurrentIndex(3);
  HDRSHOT_CHECK(zoom->currentData().toInt()==8 && f.store.snapshot.analysis_defaults.magnifier_zoom==8);
  blur->setCurrentIndex(5); sample->setCurrentIndex(5);
  HDRSHOT_CHECK(f.store.snapshot.analysis_defaults.blur_sigma_px==8 && f.store.snapshot.analysis_defaults.sample_side==101);
  HDRSHOT_CHECK(f.window.findChild<QCheckBox*>("magnifierReadingsOption4")->text()=="L*a*b* · D65（SDR）");
  for(int i=0;i<9;++i)f.window.findChild<QCheckBox*>("magnifierReadingsOption"+QString::number(i))->setChecked(true);
  HDRSHOT_CHECK(f.window.findChildren<QWidget*>("magnifierReadingsChip8").size()==1);
  auto* scroll=f.window.findChild<QScrollArea*>("analysisSettingsScrollArea");
  HDRSHOT_CHECK(scroll && scroll->widgetResizable());
  // Explicitly lower the production minimum for this short-window stress
  // case; resize(480) alone is clamped to the platform's normal minimum.
  f.window.setMinimumHeight(480);
  f.window.resize(f.window.minimumWidth(),480); QApplication::processEvents();
  HDRSHOT_CHECK(f.window.height()==480);
  HDRSHOT_CHECK(scroll->verticalScrollBar()->maximum()>0);
  auto* last=f.window.findChild<QWidget*>("magnifierReadingsChip8");
  scroll->ensureWidgetVisible(last); QApplication::processEvents();
  HDRSHOT_CHECK(last->mapTo(scroll->viewport(),QPoint{}).y()>=0);
  HDRSHOT_CHECK(last->mapTo(scroll->viewport(),QPoint(0,last->height())).y()<=scroll->viewport()->height());
}

void reading_selector_blank_area_opens_menu_and_chips_stay_independent() {
  Fixture fixture;
  fixture.window.reload_and_show();
  fixture.window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(1);
  QApplication::processEvents();
  const auto press = [](QWidget* widget, QPoint point) {
    const auto global = widget->mapToGlobal(point);
    QMouseEvent event(QEvent::MouseButtonPress, QPointF(point), QPointF(global),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
    QApplication::processEvents();
  };
  for (const auto* name : {"magnifierReadings", "analyzerReadings"}) {
    auto* selector = fixture.window.findChild<QWidget*>(name);
    auto* menu = selector->findChild<QMenu*>();
    HDRSHOT_CHECK(menu);
    press(selector, QPoint(2, 2));
    HDRSHOT_CHECK(menu->isVisible());
    menu->hide();
    auto* chips = selector->findChild<QWidget*>(QString(name) + "Chips");
    const QPoint blank(chips->width() - 1, chips->height() - 1);
    HDRSHOT_CHECK(chips->childAt(blank) == nullptr);
    press(chips, blank);
    HDRSHOT_CHECK(menu->isVisible());
    menu->hide();
    auto* chip = chips->findChildren<QWidget*>(
        QRegularExpression(QString(name) + "Chip[0-9]+"), Qt::FindDirectChildrenOnly).first();
    press(chip->findChild<QLabel*>(), QPoint(2, 2));
    HDRSHOT_CHECK(!menu->isVisible());
    const auto before = fixture.store.save_attempts;
    chip->findChild<QToolButton*>()->click();
    QApplication::processEvents();
    HDRSHOT_CHECK(fixture.store.save_attempts == before + 1 && !menu->isVisible());
  }
}

void releases_open_only_on_click() {
  Fixture fixture;
  int calls = 0;
  bool succeeds = true;
  fixture.window.set_release_page_opener([&](const QUrl& url) {
    ++calls;
    HDRSHOT_CHECK(url == QUrl("https://github.com/OverDriveHuang/SeriousShot/releases"));
    return succeeds;
  });
  fixture.window.reload_and_show();
  QApplication::processEvents();
  HDRSHOT_CHECK(calls == 0);
  auto* releases = fixture.window.findChild<QPushButton*>("openReleasesButton");
  auto* identity = fixture.window.findChild<QLabel*>("buildIdentityLabel");
  HDRSHOT_CHECK(releases && identity);
  HDRSHOT_CHECK(identity->geometry().right() < releases->geometry().left());
  fixture.window.resize(fixture.window.minimumSize());
  QApplication::processEvents();
  HDRSHOT_CHECK(identity->geometry().right() < releases->geometry().left());
  auto* close = fixture.window.findChild<QPushButton*>("closeSettingsButton");
  HDRSHOT_CHECK(close && identity->geometry().bottom() < close->geometry().top());
  releases->click();
  HDRSHOT_CHECK(calls == 1);
  succeeds = false;
  releases->click();
  HDRSHOT_CHECK(calls == 2);
  auto* status = fixture.window.findChild<QLabel*>("settingsStatusLabel");
  HDRSHOT_CHECK(status && status->isVisible());
  HDRSHOT_CHECK(status->text().contains(QStringLiteral("无法打开浏览器")));
}
}  // namespace

int main(int argc, char** argv) {
#ifdef Q_OS_WIN
  constexpr auto native_platform="windows";
#else
  constexpr auto native_platform="cocoa";
#endif
  qputenv("QT_QPA_PLATFORM",qEnvironmentVariableIsSet("HDRSHOT_SETTINGS_NATIVE")?native_platform:"offscreen");
  QApplication application(argc, argv);
  if(qEnvironmentVariableIsSet("HDRSHOT_SETTINGS_LIGHT")) {
    application.styleHints()->setColorScheme(Qt::ColorScheme::Light);
    QApplication::processEvents();
  }
  if (qEnvironmentVariableIsSet("HDRSHOT_SETTINGS_DARK")) {
    auto palette = application.palette();
    palette.setColor(QPalette::Window, QColor(30, 30, 30));
    palette.setColor(QPalette::WindowText, QColor(240, 240, 240));
    palette.setColor(QPalette::Base, QColor(42, 42, 42));
    palette.setColor(QPalette::AlternateBase, QColor(50, 50, 50));
    palette.setColor(QPalette::Text, QColor(240, 240, 240));
    palette.setColor(QPalette::Button, QColor(48, 48, 48));
    palette.setColor(QPalette::ButtonText, QColor(240, 240, 240));
    application.setPalette(palette);
  }
  const auto screenshot_path = qgetenv("HDRSHOT_SETTINGS_SCREENSHOT");
  if (!screenshot_path.isEmpty()) {
    const int font_id = QFontDatabase::addApplicationFont(
        QCoreApplication::applicationDirPath() +
        QStringLiteral("/assets/fonts/NotoSansSC-VF.ttf"));
    if (font_id >= 0) {
      const auto families = QFontDatabase::applicationFontFamilies(font_id);
      if (!families.isEmpty()) application.setFont(QFont(families.front(), 10));
    }
    Fixture fixture;
#ifdef Q_OS_WIN
    fixture.store.snapshot.windows_scrgb_gain = 0.5;
#endif
    if (qEnvironmentVariableIsSet("HDRSHOT_SETTINGS_ULTRAHDR")) {
      fixture.store.snapshot.save_format = SaveFormat::ultra_hdr_jpeg;
    }
    fixture.window.reload_and_show();
    QApplication::processEvents();
    if (qEnvironmentVariableIsSet("HDRSHOT_SETTINGS_ANALYSIS")) {
      fixture.window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(1);
      QApplication::processEvents();
    }
    if(qEnvironmentVariableIsSet("HDRSHOT_SETTINGS_ALL_READINGS")){
      for(int i=0;i<9;++i)fixture.window.findChild<QCheckBox*>("magnifierReadingsOption"+QString::number(i))->setChecked(true);
      for(int i=0;i<7;++i)fixture.window.findChild<QCheckBox*>("analyzerReadingsOption"+QString::number(i))->setChecked(true);
      QApplication::processEvents();
    }
    if (qEnvironmentVariableIsSet("HDRSHOT_SETTINGS_MINIMUM")) {
      fixture.window.resize(fixture.window.minimumSize());
      QApplication::processEvents();
    }
    if (qEnvironmentVariableIsSet("HDRSHOT_SETTINGS_SHORT")) {
      fixture.window.setMinimumHeight(480);
      fixture.window.resize(fixture.window.minimumWidth(),480);
      QApplication::processEvents();
      HDRSHOT_CHECK(fixture.window.height()==480);
    }
    if (qEnvironmentVariableIsSet("HDRSHOT_SETTINGS_SHOW_COMPATIBILITY")) {
      auto* scroll = fixture.window.findChild<QScrollArea*>("generalSettingsScrollArea");
      auto* option = fixture.window.findChild<QCheckBox*>(
          "windowsCaptureBypassSdrWhiteCheckBox");
      if (scroll != nullptr && option != nullptr) {
        scroll->ensureWidgetVisible(option, 0, 8);
        QApplication::processEvents();
      }
    }
    QApplication::processEvents();
    const auto scroll_to_bottom=[&] {
      if(!qEnvironmentVariableIsSet("HDRSHOT_SETTINGS_BOTTOM"))return;
      QApplication::processEvents();
      auto* scroll=fixture.window.findChild<QScrollArea*>(
          qEnvironmentVariableIsSet("HDRSHOT_SETTINGS_ANALYSIS")?"analysisSettingsScrollArea":"generalSettingsScrollArea");
      HDRSHOT_CHECK(scroll);
      scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
      QApplication::processEvents();
      HDRSHOT_CHECK(scroll->verticalScrollBar()->value()==scroll->verticalScrollBar()->maximum());
    };
    scroll_to_bottom();
    if (qEnvironmentVariableIsSet("HDRSHOT_SETTINGS_REPORT_SCROLL")) {
      auto* scroll = fixture.window.findChild<QScrollArea*>("generalSettingsScrollArea");
      if (scroll != nullptr) {
        std::printf("settings_size=%dx%d general_scroll_max=%d general_scroll_visible=%d\n",
            fixture.window.width(), fixture.window.height(),
            scroll->verticalScrollBar()->maximum(),
            scroll->verticalScrollBar()->isVisible() ? 1 : 0);
      }
      auto* analysis_scroll=fixture.window.findChild<QScrollArea*>("analysisSettingsScrollArea");
      if(analysis_scroll)
        std::printf("analysis_scroll_max=%d analysis_scroll_visible=%d platform=%s dpr=%.2f\n",
            analysis_scroll->verticalScrollBar()->maximum(),
            analysis_scroll->verticalScrollBar()->isVisible()?1:0,
            QGuiApplication::platformName().toUtf8().constData(),fixture.window.devicePixelRatioF());
    }
    if (!fixture.window.grab().save(QString::fromUtf8(screenshot_path))) {
      return 1;
    }
    if(qEnvironmentVariableIsSet("HDRSHOT_SETTINGS_ANALYSIS")){
      auto* scroll=fixture.window.findChild<QScrollArea*>("analysisSettingsScrollArea");
      if(!scroll->widget()->grab().save(QString::fromUtf8(screenshot_path)+".full.png"))return 1;
    }
    // Also inspect the only locally colored text in Settings in both palettes.
    auto* logging = fixture.window.findChild<QCheckBox*>("detailedLoggingCheckBox");
    logging->click();
    QApplication::processEvents();
    scroll_to_bottom();
    if (!fixture.window.grab().save(QString::fromUtf8(screenshot_path) + ".success.png"))
      return 1;
    fixture.store.fail_next_save = true;
    logging->click();
    QApplication::processEvents();
    scroll_to_bottom();
    if (!fixture.window.grab().save(QString::fromUtf8(screenshot_path) + ".error.png"))
      return 1;
    if(qEnvironmentVariableIsSet("HDRSHOT_SETTINGS_NATIVE"))return 0;
  }
  return hdrshot::test::run({
      {"general sections share dynamic scroll without overlap", general_sections_scroll_together_without_overlap},
      {"reading selector blank hit area and independent chips", reading_selector_blank_area_opens_menu_and_chips_stay_independent},
      {"two-tab defaults, separate readings, bounded scroll and error rollback", analysis_tab_defaults_are_shared_independent_and_rollback_immediately},
      {"releases opens fixed URL only on explicit click", releases_open_only_on_click},
      {"detailed log checkbox autosaves and restores on error", detailed_logging_applies_and_rolls_back_on_failure},
      {"fresh settings opens with either legacy flag and close stays closed", fresh_settings_window_ignores_legacy_presentation_flag_and_close_stays_closed},
      {"settings opens through injected platform activation", every_settings_open_uses_injected_window_activation},
      {"settings reset button applies default", reset_button_restores_default_and_applies_immediately},
      {"hotkey editor requires explicit active focus", editor_records_only_after_explicit_click_focus_and_drops_focus_on_deactivate},
      {"custom button gates hotkey recording", custom_button_is_the_only_entry_to_recording_and_command_round_trips},
      {"settings general rows are wide and aligned", general_rows_are_wide_aligned_and_folder_actions_are_explicit},
      {"completion actions and precision auto apply", completion_actions_and_precision_apply_immediately},
      {"Windows capture compatibility UI persists each state change once", windows_capture_compatibility_controls_are_transactional},
      {"Windows checkbox click commits pending gain once", windows_capture_click_commits_pending_gain_once},
      {"save format switches relevant rows", save_format_switches_only_the_relevant_quality_rows},
      {"ISO JPEG label preserves stored format and quality", iso_jpeg_label_preserves_existing_format_and_quality},
      {"failed auto apply restores snapshot", failed_auto_apply_restores_the_last_valid_snapshot},
  });
}
