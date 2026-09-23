#include "ui/qt/settings_window.hpp"
#include "core/build_metadata.hpp"
#include "ui/qt/qt_window_activation_port.hpp"
#include "qt_test_input_platform_adapter.hpp"
#include "test_support.hpp"

#include <QApplication>
#include <QDateTime>
#include <QUrl>
#include <QEvent>
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

#include <string>
#include <utility>

namespace {

using namespace hdrshot;

class MemorySettingsStore final : public SettingsStorePort {
 public:
  SettingsSnapshot snapshot{5, 0, "Command+Option+8", "/Pictures/SeriousShot"};
  bool fail_next_save{};

  Result<SettingsSnapshot, Error> load() override {
    return Result<SettingsSnapshot, Error>::success(snapshot);
  }

  Result<SettingsReceipt, Error> save(const SettingsPatch& patch) override {
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
  HDRSHOT_CHECK(general_label->mapTo(&fixture.window, QPoint{}).x() == description_x);
  HDRSHOT_CHECK(fixed_label->mapTo(&fixture.window, QPoint{}).x() == description_x);
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
  HDRSHOT_CHECK(format->width() == png_width);
  HDRSHOT_CHECK(
      fixture.store.snapshot.ultra_hdr_jpeg_quality ==
      UltraHdrJpegQuality::compact);
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
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);
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
    Fixture fixture;
    if (qEnvironmentVariableIsSet("HDRSHOT_SETTINGS_ULTRAHDR")) {
      fixture.store.snapshot.save_format = SaveFormat::ultra_hdr_jpeg;
    }
    fixture.window.reload_and_show();
    QApplication::processEvents();
    if (!fixture.window.grab().save(QString::fromUtf8(screenshot_path))) {
      return 1;
    }
    // Also inspect the only locally colored text in Settings in both palettes.
    auto* logging = fixture.window.findChild<QCheckBox*>("detailedLoggingCheckBox");
    logging->click();
    QApplication::processEvents();
    if (!fixture.window.grab().save(QString::fromUtf8(screenshot_path) + ".success.png"))
      return 1;
    fixture.store.fail_next_save = true;
    logging->click();
    QApplication::processEvents();
    if (!fixture.window.grab().save(QString::fromUtf8(screenshot_path) + ".error.png"))
      return 1;
  }
  return hdrshot::test::run({
      {"releases opens fixed URL only on explicit click", releases_open_only_on_click},
      {"detailed log checkbox autosaves and restores on error", detailed_logging_applies_and_rolls_back_on_failure},
      {"fresh settings opens with either legacy flag and close stays closed", fresh_settings_window_ignores_legacy_presentation_flag_and_close_stays_closed},
      {"settings opens through injected platform activation", every_settings_open_uses_injected_window_activation},
      {"settings reset button applies default", reset_button_restores_default_and_applies_immediately},
      {"hotkey editor requires explicit active focus", editor_records_only_after_explicit_click_focus_and_drops_focus_on_deactivate},
      {"custom button gates hotkey recording", custom_button_is_the_only_entry_to_recording_and_command_round_trips},
      {"settings general rows are wide and aligned", general_rows_are_wide_aligned_and_folder_actions_are_explicit},
      {"completion actions and precision auto apply", completion_actions_and_precision_apply_immediately},
      {"save format switches relevant rows", save_format_switches_only_the_relevant_quality_rows},
      {"ISO JPEG label preserves stored format and quality", iso_jpeg_label_preserves_existing_format_and_quality},
      {"failed auto apply restores snapshot", failed_auto_apply_restores_the_last_valid_snapshot},
  });
}
