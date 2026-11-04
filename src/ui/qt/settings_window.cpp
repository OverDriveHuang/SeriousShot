#include <QTimer>
#include "ui/qt/analysis_readings.hpp"
#include "ui/qt/settings_window.hpp"
#include "ui/qt/ui_language_runtime.hpp"
#include <QCheckBox>
#include <QTabWidget>
#include <QGridLayout>
#include "core/build_metadata.hpp"

#include <QFileDialog>
#include <QDateTime>
#include <QDoubleValidator>
#include <QDesktopServices>
#include <QUrl>
#include <QEvent>
#include <QApplication>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyCombination>
#include <QKeySequence>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <string>
#include <utility>

namespace hdrshot {
namespace {

QLabel* shortcut_value(const QString& value, QWidget* parent) {
  auto* label = new QLabel(value, parent);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

QString login_startup_text(const LoginStartupText& text, const std::string& fallback) {
  if (!text.id) return QString::fromStdString(fallback);
  auto translated = qtTrId(text.id);
  if (text.argument) translated = translated.arg(QString::fromStdString(*text.argument));
  return translated;
}

QString login_startup_error_field(const Error& error, const std::string& field) {
  const auto original = error.safe_context.find(field);
  if (original == error.safe_context.end()) return {};
  //% "The system did not allow this change. Check system settings, then check the box again."
  [[maybe_unused]] constexpr auto disallowed_id = QT_TRID_NOOP("settings.login.change_disallowed");
  const auto id = error.safe_context.find(field + "_id");
  if (id == error.safe_context.end()) return QString::fromStdString(original->second);
  auto translated = qtTrId(id->second.c_str());
  const auto argument = error.safe_context.find(field + "_argument");
  if (argument != error.safe_context.end())
    translated = translated.arg(QString::fromStdString(argument->second));
  const auto suffix = error.safe_context.find(field + "_suffix");
  if (suffix != error.safe_context.end()) translated += QString::fromStdString(suffix->second);
  return translated;
}

QString login_startup_diagnostic(const LoginStartupSnapshot& snapshot) {
  if (!snapshot.diagnostic_text.id) return QString::fromStdString(snapshot.diagnostic);
  auto message = login_startup_text(snapshot.diagnostic_text, snapshot.diagnostic);
  if (snapshot.diagnostic_error) {
    const auto& error = *snapshot.diagnostic_error;
    for (const auto& field : error.safe_context) {
      const auto& key = field.first;
      if (key.ends_with("_id") || key.ends_with("_argument") || key.ends_with("_suffix")) continue;
      message += " " + QString::fromStdString(key) + "=" + login_startup_error_field(error, key);
    }
  }
  message += QString::fromStdString(snapshot.diagnostic_native_suffix);
  return message;
}

#ifdef Q_OS_WIN
QString windows_gain_text(double gain) {
  auto text = QString::number(gain, 'f', 12);
  while (text.endsWith(QLatin1Char('0'))) text.chop(1);
  if (text.endsWith(QLatin1Char('.'))) text += QLatin1Char('0');
  return text;
}
#endif

}  // namespace

SettingsWindow::SettingsWindow(
    SettingsStorePort& settings_store,
    GlobalHotkeyPort& hotkey_port,
    FolderOpenerPort& folder_opener,
    QtInputPlatformAdapter& input_platform,
    std::string default_global_hotkey,
    std::string diagnostics_folder,
    QWidget* parent)
    : QWidget(parent),
      settings_store_(settings_store),
      hotkey_port_(hotkey_port),
      folder_opener_(folder_opener),
      input_platform_(input_platform),
      default_global_hotkey_(std::move(default_global_hotkey)),
      diagnostics_folder_(std::move(diagnostics_folder)) {
  qApp->installEventFilter(this);
  //% "SeriousShot Settings"
  setWindowTitle(qtTrId("settings.title"));
  setWindowFlag(Qt::WindowStaysOnTopHint, false);
#ifdef Q_OS_WIN
  setMinimumSize(800, 780);
  resize(860, 850);
#else
  setMinimumSize(800, 730);
  resize(860, 770);
#endif

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(28, 24, 28, 24);
  root->setSpacing(18);

  auto* heading = new QHBoxLayout();
  auto* title = new QLabel(QStringLiteral("SeriousShot"), this);
  auto title_font = title->font();
  title_font.setPointSize(22);
  title_font.setBold(true);
  title->setFont(title_font);
  title->setObjectName(QStringLiteral("settingsTitleLabel"));
  auto* author = new QLabel(QStringLiteral("by OverDrive"), this);
  author->setObjectName(QStringLiteral("authorLabel"));
  heading->addWidget(title);
  heading->addWidget(author, 0, Qt::AlignBaseline);
  heading->addStretch(1);
  language_restart_hint_ = new QPushButton(this);
  language_restart_hint_->setObjectName(QStringLiteral("settingsLanguageRestartHint"));
  language_restart_hint_->setFlat(true);
  language_restart_hint_->setCursor(Qt::PointingHandCursor);
  language_restart_hint_->setFocusPolicy(Qt::StrongFocus);
  language_restart_hint_->setStyleSheet(QStringLiteral(
      "QPushButton { border: 0; padding: 2px; color: palette(link); }"
      "QPushButton:hover { text-decoration: underline; }"
      "QPushButton:focus { border: 1px solid palette(highlight); }"
      "QPushButton:disabled { color: palette(text); }"));
  connect(language_restart_hint_, &QPushButton::clicked, this,
          [this] { confirm_language_restart(); });
  language_restart_hint_->hide();
  heading->addWidget(language_restart_hint_);
  auto* language_buttons = new QHBoxLayout();
  language_buttons->setSpacing(4);
  heading->addLayout(language_buttons);
  const auto add_language_button = [this, language_buttons](
      const QString& text, const QString& name, const char* object_name, UiLanguage language) {
    auto* button = new QPushButton(text, this);
    button->setObjectName(QString::fromLatin1(object_name));
    button->setAccessibleName(name);
    button->setCheckable(true);
    button->setEnabled(false);
    button->setFocusPolicy(Qt::StrongFocus);
    button->setFixedWidth(36);
    button->setMinimumHeight(28);
    button->setStyleSheet(QStringLiteral(
        // Keep every state on the same compact box model. Cocoa's native
        // unchecked button can reserve more label inset than a 36px button has.
        "QPushButton { background-color: palette(button); color: palette(button-text); "
        "border: 1px solid palette(mid); border-radius: 3px; padding: 0px 4px; } "
        "QPushButton:checked { background-color: palette(highlight); "
        "color: palette(highlighted-text); border-color: palette(highlight); } "
        "QPushButton:focus { border-color: palette(highlight); } "
        "QPushButton:checked:focus { border-color: palette(highlighted-text); }"));
    language_buttons->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, language] {
      persist_ui_language(language);
    });
    return button;
  };
  //% "Simplified Chinese"
  chinese_language_button_ = add_language_button(
      QStringLiteral("中"), qtTrId("language.simplified_chinese.accessible"),
      "settingsLanguageChineseButton", UiLanguage::simplified_chinese);
  //% "English"
  english_language_button_ = add_language_button(
      QStringLiteral("EN"), qtTrId("language.english.accessible"),
      "settingsLanguageEnglishButton", UiLanguage::english);
  root->addLayout(heading);

  auto* tabs = new QTabWidget(this);
  tabs->setObjectName("settingsTabs");
  root->addWidget(tabs,1);
  auto* general_page=new QWidget(tabs);
  auto* general_root=new QVBoxLayout(general_page);
  general_root->setContentsMargins(0,0,0,0);
  //% "General"
  tabs->addTab(general_page,qtTrId("settings.tab.general"));
  auto* analysis_page=new QWidget(tabs);
  //% "Analysis"
  tabs->addTab(analysis_page,qtTrId("settings.tab.analysis"));
  build_analysis_tab(analysis_page);
  auto* pinned_page = new QWidget(tabs);
  //% "Pinned"
  tabs->addTab(pinned_page, qtTrId("settings.tab.pinned"));
  build_pinned_screenshot_tab(pinned_page);
  const auto analysis_margins = analysis_page->layout()->contentsMargins();
  general_root->setContentsMargins(analysis_margins);
  auto* general_section = new QVBoxLayout();
  general_section->setContentsMargins(0, 0, 0, 0);
  general_section->setSpacing(6);
  //% "General"
  auto* general_label = new QLabel(qtTrId("settings.section.general"), this);
  general_label->setObjectName(QStringLiteral("generalSectionLabel"));
  general_label->setFont(font());
  general_section->addWidget(general_label);
  auto* general = new QGroupBox(this);
  general->setObjectName(QStringLiteral("generalSettingsContainer"));
  general_form_ = new QFormLayout(general);
  general_form_->setSizeConstraint(QLayout::SetMinimumSize);
  general_form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  general_form_->setRowWrapPolicy(QFormLayout::DontWrapRows);
  general_form_->setHorizontalSpacing(18);
  general_form_->setVerticalSpacing(14);
  login_startup_check_ = new QCheckBox(general);
  login_startup_check_->setObjectName(QStringLiteral("loginStartupCheckBox"));
  //% "Launch at login"
  login_startup_check_->setAccessibleName(qtTrId("settings.login.label"));
  //% "Launch at login"
  general_form_->addRow(qtTrId("settings.login.label"), login_startup_check_);
  general_form_->setRowVisible(login_startup_check_, false);
  connect(login_startup_check_, &QCheckBox::clicked, this, [this](bool enabled) {
    persist_login_startup(enabled);
  });
  hotkey_edit_ = new QKeySequenceEdit(general);
  hotkey_edit_->setObjectName(QStringLiteral("globalHotkeyEdit"));
  hotkey_edit_->setMaximumSequenceLength(1);
  hotkey_edit_->setMinimumWidth(160);
  hotkey_edit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  hotkey_edit_->setFocusPolicy(Qt::NoFocus);
  hotkey_edit_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
  auto* hotkey_row = new QWidget(general);
  auto* hotkey_layout = new QHBoxLayout(hotkey_row);
  hotkey_layout->setContentsMargins(0, 0, 0, 0);
  //% "Customize"
  customize_hotkey_button_ = new QPushButton(qtTrId("settings.shortcut.customize"), hotkey_row);
  customize_hotkey_button_->setObjectName(QStringLiteral("customizeGlobalHotkeyButton"));
  connect(customize_hotkey_button_, &QPushButton::clicked, this, [this] {
    if (recording_hotkey_) {
      finish_hotkey_recording(false);
    } else {
      start_hotkey_recording();
    }
  });
  //% "Reset"
  auto* reset_hotkey = new QPushButton(qtTrId("settings.shortcut.reset"), hotkey_row);
  reset_hotkey->setObjectName(QStringLiteral("resetGlobalHotkeyButton"));
  connect(reset_hotkey, &QPushButton::clicked, this, [this] {
    finish_hotkey_recording(false);
    persist_hotkey(default_global_hotkey_);
  });
  customize_hotkey_button_->setFixedWidth(96);
  reset_hotkey->setFixedWidth(96);
  connect(hotkey_edit_, &QKeySequenceEdit::keySequenceChanged, this,
      [this](const QKeySequence& sequence) {
    if (recording_hotkey_ && !sequence.isEmpty()) {
      QTimer::singleShot(0, this, [this] { finish_hotkey_recording(true); });
    }
  });
  hotkey_layout->addWidget(hotkey_edit_, 1);
  hotkey_layout->addWidget(customize_hotkey_button_);
  hotkey_layout->addWidget(reset_hotkey);
  //% "Capture shortcut"
  general_form_->addRow(qtTrId("settings.shortcut.label"), hotkey_row);

  const auto add_completion_items = [](QComboBox* combo) {
    combo->addItem(
        //% "Copy to clipboard"
        qtTrId("settings.completion.copy"),
        static_cast<int>(CompletionAction::copy_to_clipboard));
    combo->addItem(
        //% "Save to default folder"
        qtTrId("settings.completion.save_default"),
        static_cast<int>(CompletionAction::save_default));
    combo->addItem(
        //% "Save as…"
        qtTrId("settings.completion.save_as"),
        static_cast<int>(CompletionAction::save_as));
  };
  enter_completion_combo_ = new QComboBox(general);
  enter_completion_combo_->setObjectName(QStringLiteral("enterCompletionActionCombo"));
  enter_completion_combo_->setMinimumWidth(390);
  add_completion_items(enter_completion_combo_);
  //% "On Enter"
  general_form_->addRow(qtTrId("settings.completion.enter"), enter_completion_combo_);

  double_click_completion_combo_ = new QComboBox(general);
  double_click_completion_combo_->setObjectName(
      QStringLiteral("doubleClickCompletionActionCombo"));
  double_click_completion_combo_->setMinimumWidth(390);
  add_completion_items(double_click_completion_combo_);
  general_form_->addRow(
      //% "On double-click"
      qtTrId("settings.completion.double_click"), double_click_completion_combo_);

  auto* folder_row = new QWidget(general);
  auto* folder_layout = new QHBoxLayout(folder_row);
  folder_layout->setContentsMargins(0, 0, 0, 0);
  folder_edit_ = new QLineEdit(folder_row);
  folder_edit_->setObjectName(QStringLiteral("defaultSaveFolderEdit"));
  folder_edit_->setReadOnly(true);
  folder_edit_->setMinimumWidth(160);
  folder_edit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  //% "Choose"
  auto* choose = new QPushButton(qtTrId("settings.folder.choose"), folder_row);
  choose->setObjectName(QStringLiteral("chooseDefaultSaveFolderButton"));
  choose->setFixedWidth(96);
  //% "Open"
  auto* open = new QPushButton(qtTrId("settings.folder.open"), folder_row);
  open->setObjectName(QStringLiteral("openDefaultSaveFolderButton"));
  open->setFixedWidth(96);
  connect(choose, &QPushButton::clicked, this, [this] { choose_folder(); });
  connect(open, &QPushButton::clicked, this, [this] {
    //% "Default save folder"
    open_folder(folder_edit_->text().toStdString(), qtTrId("settings.folder.default_name"));
  });
  folder_layout->addWidget(folder_edit_, 1);
  folder_layout->addWidget(choose);
  folder_layout->addWidget(open);
  //% "Save folder"
  general_form_->addRow(qtTrId("settings.folder.label"), folder_row);

  save_format_combo_ = new QComboBox(general);
  save_format_combo_->setObjectName(QStringLiteral("saveFormatCombo"));
  save_format_combo_->setMinimumWidth(390);
  save_format_combo_->addItem(
      //% "PNG (SDR: Display P3 ICC; HDR: Display P3 PQ)"
      qtTrId("settings.format.png"),
      static_cast<int>(SaveFormat::png_display_p3_dual_range));
  save_format_combo_->addItem(
      //% "JPEG (ISO 21496-1 + XMP)"
      qtTrId("settings.format.jpeg"),
      static_cast<int>(SaveFormat::ultra_hdr_jpeg));
  //% "Format"
  general_form_->addRow(qtTrId("settings.format.label"), save_format_combo_);
  pq_diffuse_white_combo_ = new QComboBox(general);
  pq_diffuse_white_combo_->setObjectName(QStringLiteral("pqDiffuseWhiteCombo"));
  pq_diffuse_white_combo_->setMinimumWidth(390);
  pq_diffuse_white_combo_->addItem(
      QStringLiteral("100 nit"), static_cast<int>(PqDiffuseWhite::nits_100));
  pq_diffuse_white_combo_->addItem(
      QStringLiteral("203 nit"), static_cast<int>(PqDiffuseWhite::nits_203));
  pq_diffuse_white_combo_->setToolTip(
      //% "Applies to HDR Display P3 PQ output only."
      qtTrId("settings.diffuse_white.hint"));
  general_form_->addRow(
      //% "PQ Diffuse White"
      qtTrId("settings.diffuse_white.label"), pq_diffuse_white_combo_);
  hdr_pq_precision_combo_ = new QComboBox(general);
  hdr_pq_precision_combo_->setObjectName(QStringLiteral("hdrPqPrecisionCombo"));
  hdr_pq_precision_combo_->setMinimumWidth(390);
  hdr_pq_precision_combo_->addItem(
      //% "Very high (16-bit PQ)"
      qtTrId("settings.png_precision.highest"),
      static_cast<int>(HdrPqPrecision::bits_16));
  hdr_pq_precision_combo_->addItem(
      //% "High (12-bit PQ)"
      qtTrId("settings.png_precision.high"),
      static_cast<int>(HdrPqPrecision::bits_12));
  hdr_pq_precision_combo_->addItem(
      //% "Compact (10-bit PQ, default)"
      qtTrId("settings.png_precision.compact"),
      static_cast<int>(HdrPqPrecision::bits_10));
  hdr_pq_precision_combo_->setToolTip(
      //% "Affects HDR PQ code precision. The PNG container still uses standard 16-bit RGB."
      qtTrId("settings.png_precision.hint"));
  //% "HDR PNG precision"
  general_form_->addRow(qtTrId("settings.png_precision.label"), hdr_pq_precision_combo_);
  ultra_hdr_jpeg_quality_combo_ = new QComboBox(general);
  ultra_hdr_jpeg_quality_combo_->setObjectName(
      QStringLiteral("ultraHdrJpegQualityCombo"));
  ultra_hdr_jpeg_quality_combo_->setMinimumWidth(390);
  ultra_hdr_jpeg_quality_combo_->addItem(
      //% "Very high (100)"
      qtTrId("settings.jpeg_quality.highest"),
      static_cast<int>(UltraHdrJpegQuality::maximum));
  ultra_hdr_jpeg_quality_combo_->addItem(
      //% "Balanced (95, default)"
      qtTrId("settings.jpeg_quality.balanced"),
      static_cast<int>(UltraHdrJpegQuality::balanced));
  ultra_hdr_jpeg_quality_combo_->addItem(
      //% "Compact (85)"
      qtTrId("settings.jpeg_quality.compact"),
      static_cast<int>(UltraHdrJpegQuality::compact));
  ultra_hdr_jpeg_quality_combo_->setToolTip(
      //% "Applies to both the SDR base JPEG and the gain map JPEG. SDR-only content produces a standard JPEG."
      qtTrId("settings.jpeg_quality.hint"));
  general_form_->addRow(
      //% "JPEG quality"
      qtTrId("settings.jpeg_quality.label"), ultra_hdr_jpeg_quality_combo_);
#ifdef Q_OS_WIN
  auto* compatibility_row = new QWidget(general);
  auto* compatibility_layout = new QHBoxLayout(compatibility_row);
  compatibility_layout->setContentsMargins(0, 0, 0, 0);
  windows_capture_compatibility_check_ = new QCheckBox(
      QString{}, compatibility_row);
  windows_capture_compatibility_check_->setObjectName(
      QStringLiteral("windowsCaptureCompatibilityCheckBox"));
  //% "Win 10 capture brightness fix"
  windows_capture_compatibility_check_->setAccessibleName(qtTrId("settings.windows_compatibility.accessible"));
  windows_capture_compatibility_check_->setTristate(true);
  compatibility_layout->addWidget(windows_capture_compatibility_check_);
  auto* compatibility_help = new QLabel(
      //% "Enable if brightness is incorrect"
      qtTrId("settings.windows_compatibility.hint"),
      compatibility_row);
  compatibility_help->setObjectName(QStringLiteral("windowsCaptureCompatibilityHelp"));
  auto help_palette = compatibility_help->palette();
  auto help_color = help_palette.color(QPalette::WindowText);
  help_color.setAlphaF(0.68f);
  help_palette.setColor(QPalette::WindowText, help_color);
  compatibility_help->setPalette(help_palette);
  compatibility_layout->addWidget(compatibility_help);
  compatibility_layout->addStretch(1);
  //% "Win 10 brightness fix"
  general_form_->addRow(qtTrId("settings.windows_compatibility.label"), compatibility_row);

  auto* compatibility_options = new QWidget(general);
  auto* options_layout = new QHBoxLayout(compatibility_options);
  options_layout->setContentsMargins(24, 0, 0, 0);
  windows_capture_gain_check_ = new QCheckBox(
      QStringLiteral("scRGB gain ="), compatibility_options);
  windows_capture_gain_check_->setObjectName(QStringLiteral("windowsCaptureGainCheckBox"));
  options_layout->addWidget(windows_capture_gain_check_);
  windows_scrgb_gain_edit_ = new QLineEdit(compatibility_options);
  windows_scrgb_gain_edit_->setObjectName(QStringLiteral("windowsScRgbGainEdit"));
  windows_scrgb_gain_edit_->setFixedWidth(110);
  auto* gain_validator = new QDoubleValidator(0.0, 3.0, 12, windows_scrgb_gain_edit_);
  gain_validator->setLocale(QLocale::c());
  gain_validator->setNotation(QDoubleValidator::StandardNotation);
  windows_scrgb_gain_edit_->setValidator(gain_validator);
  //% "scRGB RGB gain for Windows capture (0–3). Default: 0.5."
  windows_scrgb_gain_edit_->setToolTip(qtTrId("settings.windows_compatibility.gain_hint"));
  windows_scrgb_gain_edit_->installEventFilter(this);
  options_layout->addWidget(windows_scrgb_gain_edit_);
  options_layout->addSpacing(18);
  windows_capture_bypass_sdr_white_check_ = new QCheckBox(
      //% "Skip SDRWhiteLevel normalization for HDR capture."
      qtTrId("settings.windows_compatibility.bypass"), compatibility_options);
  windows_capture_bypass_sdr_white_check_->setObjectName(
      QStringLiteral("windowsCaptureBypassSdrWhiteCheckBox"));
  options_layout->addWidget(windows_capture_bypass_sdr_white_check_);
  options_layout->addStretch(1);
  general_form_->addRow(compatibility_options);
#endif
  general_form_->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
  update_general_label_width();
  general_section->addWidget(general);
  auto* shortcuts_section = new QVBoxLayout();
  shortcuts_section->setContentsMargins(0, 0, 0, 0);
  shortcuts_section->setSpacing(6);
  //% "Capture shortcuts"
  auto* shortcuts_label = new QLabel(qtTrId("settings.fixed_shortcuts.title"), this);
  shortcuts_label->setObjectName(QStringLiteral("fixedOperationsSectionLabel"));
  shortcuts_label->setFont(font());
  shortcuts_section->addWidget(shortcuts_label);
  auto* shortcuts = new QGroupBox(this);
  shortcuts->setObjectName(QStringLiteral("fixedOperationsContainer"));
  auto* shortcuts_grid = new QGridLayout(shortcuts);
  shortcuts_grid->setHorizontalSpacing(24);
  shortcuts_grid->setVerticalSpacing(10);
  constexpr std::array<const char*, 5> names{
      //% "Cancel"
      QT_TRID_NOOP("settings.fixed_shortcuts.cancel"),
      //% "Save"
      QT_TRID_NOOP("settings.fixed_shortcuts.save"),
      //% "Save As"
      QT_TRID_NOOP("settings.fixed_shortcuts.save_as"),
      //% "Undo"
      QT_TRID_NOOP("settings.fixed_shortcuts.undo"),
      //% "Redo"
      QT_TRID_NOOP("settings.fixed_shortcuts.redo"),
  };
  const std::array<UiCommand, 5> commands{UiCommand::cancel_capture, UiCommand::save_default, UiCommand::save_as, UiCommand::undo, UiCommand::redo};
  std::array<QGridLayout*, 3> groups{};
  for (int col=0;col<3;++col) {
    auto* group = new QWidget(shortcuts);
    group->setObjectName(QString("fixedShortcutGroup%1").arg(col));
    groups[col] = new QGridLayout(group);
    groups[col]->setContentsMargins(0,0,0,0);
    groups[col]->setHorizontalSpacing(12);
    groups[col]->setVerticalSpacing(10);
    groups[col]->setSizeConstraint(QLayout::SetFixedSize);
    groups[col]->setRowMinimumHeight(1,QFontMetrics(shortcuts->font()).height());
    shortcuts_grid->addWidget(group,0,col,Qt::AlignLeft | Qt::AlignTop);
    shortcuts_grid->setColumnStretch(col,1);
  }
  for (int i=0;i<5;++i) {
    const int col=i<3?i:i-2;
    auto* label = new QLabel(qtTrId(names[i]),shortcuts);
    auto* value = shortcut_value(input_platform_.fixed_shortcut_label(commands[i]),shortcuts);
    label->setObjectName(QString("fixedOperation%1").arg(i));
    value->setObjectName(QString("fixedShortcut%1").arg(i));
    label->setAlignment(Qt::AlignCenter);
    value->setAlignment(Qt::AlignCenter);
    groups[col]->addWidget(label,i/3,0);
    groups[col]->addWidget(value,i/3,1);
  }
  shortcuts_section->addWidget(shortcuts);
  auto* general_scroll_content = new QWidget(general_page);
  auto* content_layout = new QVBoxLayout(general_scroll_content);
  content_layout->setContentsMargins(0, 0, 0, 0);
  content_layout->setSpacing(18);
  content_layout->setSizeConstraint(QLayout::SetMinimumSize);
  content_layout->addLayout(general_section);
  content_layout->addLayout(shortcuts_section);
  content_layout->addStretch(1);
  auto* general_scroll = new QScrollArea(general_page);
  general_scroll->setObjectName(QStringLiteral("generalSettingsScrollArea"));
  general_scroll->setWidgetResizable(true);
  general_scroll->setFrameShape(QFrame::NoFrame);
  general_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  general_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  general_scroll->setWidget(general_scroll_content);
  general_scroll_content->setAutoFillBackground(false);
  general_scroll->setAutoFillBackground(false);
  general_scroll->viewport()->setAutoFillBackground(false);
  general_root->addWidget(general_scroll);

  status_label_ = new QLabel(this);
  status_label_->setObjectName(QStringLiteral("settingsStatusLabel"));
  status_label_->setWordWrap(true);
  status_label_->hide();
  root->addWidget(status_label_);

  auto* identity_row = new QHBoxLayout();
  //% "Version %1"
  auto* identity_label = new QLabel(qtTrId("settings.build.version").arg(QString::fromLatin1(product_version().data(),static_cast<qsizetype>(product_version().size()))),this);
  identity_label->setObjectName(QStringLiteral("buildIdentityLabel"));
  identity_label->setWordWrap(true);
  identity_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  identity_label->setToolTip(source_commit().empty()
      //% "Trusted source revision unavailable."
      ? qtTrId("settings.build.revision_missing")
      //% "Snapshot SHA-256: %1"
      : (source_commit().size() == 64 ? qtTrId("settings.build.snapshot")
                                      //% "Source commit: %1"
                                      : qtTrId("settings.build.commit")).arg(QString::fromLatin1(
            source_commit().data(), static_cast<qsizetype>(source_commit().size()))));
  const auto source_time=QDateTime::fromString(QString::fromLatin1(source_commit_timestamp().data(),static_cast<qsizetype>(source_commit_timestamp().size())),Qt::ISODate);
  const auto source_time_text = source_time.isValid()
      ? source_time.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'"))
      //% "Unknown"
      : qtTrId("settings.build.source_time_unknown");
  //% "\nSource time: %1"
  auto source_tooltip = identity_label->toolTip() + qtTrId("settings.build.source_time").arg(source_time_text);
  if (source_modified()) {
    //% "\nIncludes uncommitted changes"
    source_tooltip += qtTrId("settings.build.modified");
  }
  identity_label->setToolTip(source_tooltip);
  identity_row->addWidget(identity_label, 1);
  //% "Check for Updates"
  update_entry_=new QPushButton(qtTrId("settings.update.check"),this);
  update_entry_->setObjectName(QStringLiteral("checkUpdatesButton"));update_entry_->setFlat(true);update_entry_->setEnabled(false);
  identity_row->addWidget(update_entry_);
  connect(update_entry_,&QPushButton::clicked,this,[this]{if(update_opener_)update_opener_();});
  root->addLayout(identity_row);

  auto* actions = new QHBoxLayout();
  actions->addStretch(1);
  //% "Verbose logs (debug)"
  detailed_logging_check_ = new QCheckBox(qtTrId("settings.logging.verbose"), this);
  detailed_logging_check_->setObjectName(QStringLiteral("detailedLoggingCheckBox"));
  actions->addWidget(detailed_logging_check_);
  connect(detailed_logging_check_, &QCheckBox::toggled, this, [this](bool enabled) {
    if (loading_controls_) return;
    const auto saved = SettingsWorkflow::change_detailed_logging(enabled, settings_store_);
    if (!saved) {
      restore_controls_from_snapshot();
      //% "Could not save logging settings: %1"
      show_status(qtTrId("settings.logging.save_failed")
          .arg(QString::fromStdString(to_string(saved.error().code))), true);
      return;
    }
    //% "Logging settings applied."
    finish_saved_change(qtTrId("settings.logging.saved"));
  });
  //% "Open Log Folder"
  auto* open_logs = new QPushButton(qtTrId("settings.logging.open_folder"), this);
  open_logs->setObjectName(QStringLiteral("openLogsFolderButton"));
  connect(open_logs, &QPushButton::clicked, this, [this] {
    //% "Log folder"
    open_folder(diagnostics_folder_, qtTrId("settings.logging.folder_name"));
  });
  //% "Close"
  auto* close = new QPushButton(qtTrId("settings.close"), this);
  close->setObjectName(QStringLiteral("closeSettingsButton"));
  close->setDefault(true);
  connect(close, &QPushButton::clicked, this, &QWidget::hide);
  actions->addWidget(open_logs);
  actions->addWidget(close);
  root->addLayout(actions);

  connect(save_format_combo_, &QComboBox::currentIndexChanged, this,
      [this] { persist_save_format(); });
  connect(pq_diffuse_white_combo_, &QComboBox::currentIndexChanged, this,
      [this] { persist_pq_diffuse_white(); });
#ifdef Q_OS_WIN
  connect(windows_scrgb_gain_edit_, &QLineEdit::editingFinished, this,
      [this] { persist_windows_scrgb_gain(); });
  connect(windows_capture_compatibility_check_, &QCheckBox::clicked, this,
      [this] {
        const bool all_enabled = snapshot_.windows_capture_gain_enabled &&
            snapshot_.windows_capture_bypass_sdr_white_enabled;
        persist_windows_capture_compatibility(!all_enabled, !all_enabled);
      });
  connect(windows_capture_gain_check_, &QCheckBox::clicked, this,
      [this](bool checked) {
        persist_windows_capture_compatibility(
            checked, snapshot_.windows_capture_bypass_sdr_white_enabled);
      });
  connect(windows_capture_bypass_sdr_white_check_, &QCheckBox::clicked, this,
      [this](bool checked) {
        persist_windows_capture_compatibility(
            snapshot_.windows_capture_gain_enabled, checked);
      });
#endif
  connect(hdr_pq_precision_combo_, &QComboBox::currentIndexChanged, this,
      [this] { persist_hdr_pq_precision(); });
  connect(ultra_hdr_jpeg_quality_combo_, &QComboBox::currentIndexChanged, this,
      [this] { persist_ultra_hdr_jpeg_quality(); });
  connect(enter_completion_combo_, &QComboBox::currentIndexChanged, this,
      [this] { persist_enter_completion_action(); });
  connect(double_click_completion_combo_, &QComboBox::currentIndexChanged, this,
      [this] { persist_double_click_completion_action(); });
}

void SettingsWindow::set_language_runtime(QtUiLanguageRuntime& runtime) {
  language_runtime_ = &runtime;
  chinese_language_button_->setEnabled(true);
  english_language_button_->setEnabled(true);
  render_language_choice();
}

void SettingsWindow::render_language_choice() {
  if (!language_runtime_) return;
  const auto active = language_runtime_->active_language();
  const auto preferred = snapshot_.ui_language.value_or(active);
  chinese_language_button_->setChecked(preferred == UiLanguage::simplified_chinese);
  english_language_button_->setChecked(preferred == UiLanguage::english);
  if (preferred == active) {
    language_restart_hint_->hide();
  } else {
    language_restart_hint_->setText(language_runtime_->restart_hint(preferred));
    language_restart_hint_->setEnabled(!restart_pending_ &&
        language_runtime_->restart_available(preferred));
    language_restart_hint_->show();
  }
}

void SettingsWindow::set_restart_requester(
    std::function<UiRestartOutcome(UiLanguage)> callback) {
  restart_requester_ = std::move(callback);
}

void SettingsWindow::confirm_language_restart() {
  if (!language_runtime_ || !restart_requester_ || restart_pending_ || !snapshot_.ui_language)
    return;
  const auto target = *snapshot_.ui_language;
  if (target == language_runtime_->active_language() ||
      !language_runtime_->restart_available(target)) return;
  restart_pending_ = true;
  render_language_choice();
  //% "Restart SeriousShot"
  const auto title = language_runtime_->text(target, QT_TRID_NOOP("language.restart.title"));
  //% "Restart SeriousShot and switch to Simplified Chinese?"
  const auto* chinese_question = QT_TRID_NOOP("language.restart.confirm_zh");
  //% "Restart SeriousShot and switch to English?"
  const auto* english_question = QT_TRID_NOOP("language.restart.confirm_en");
  auto* dialog = new QMessageBox(QMessageBox::Question, title,
      language_runtime_->text(target, target == UiLanguage::simplified_chinese
          ? chinese_question : english_question), QMessageBox::NoButton, this);
  dialog->setObjectName(QStringLiteral("settingsLanguageRestartConfirmation"));
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setTextFormat(Qt::PlainText);
  //% "Restart"
  auto* restart = dialog->addButton(
      language_runtime_->text(target, QT_TRID_NOOP("language.restart.action")),
      QMessageBox::AcceptRole);
  restart->setObjectName(QStringLiteral("settingsRestartConfirmButton"));
  //% "Cancel"
  auto* cancel = dialog->addButton(
      language_runtime_->text(target, QT_TRID_NOOP("language.restart.cancel")),
      QMessageBox::RejectRole);
  dialog->setDefaultButton(cancel);
  dialog->setEscapeButton(cancel);
  restart_dialog_ = dialog;
  connect(dialog, &QMessageBox::finished, this, [this, dialog, restart, target] {
    const bool accepted = dialog->clickedButton() == restart;
    restart_dialog_.clear();
    // Leave the dialog's finished dispatch before asking the native shell to exit.
    QTimer::singleShot(0, this, [this, accepted, target] {
      if (!accepted || snapshot_.ui_language != target ||
          target == language_runtime_->active_language() ||
          !language_runtime_->restart_available(target)) {
        restart_pending_ = false;
        render_language_choice();
        return;
      }
      const auto outcome = restart_requester_ ? restart_requester_(target) : UiRestartOutcome::cancelled;
      if (outcome == UiRestartOutcome::accepted) return;
      restart_pending_ = false;
      render_language_choice();
      if (outcome == UiRestartOutcome::busy) {
        //% "Finish the capture, save, or update and close analysis and pinned windows, then try restarting again."
        show_status(language_runtime_->text(target, QT_TRID_NOOP("language.restart.busy")), true);
      }
    });
  });
  dialog->open();
}

void SettingsWindow::persist_ui_language(UiLanguage language) {
  if (!language_runtime_) return;
  if (snapshot_.ui_language == language) {
    render_language_choice();
    return;
  }
  const auto saved = SettingsWorkflow::change_ui_language(language, settings_store_);
  if (!saved) {
    render_language_choice();
    //% "Could not save the display language: %1"
    show_status(qtTrId("language.save_failed")
                    .arg(QString::fromStdString(to_string(saved.error().code))),
                true);
    return;
  }
  snapshot_.ui_language = language;
  snapshot_.revision = saved.value().settings_revision;
  render_language_choice();
  status_label_->hide();
}

void SettingsWindow::set_applied(Applied callback) {
  applied_ = std::move(callback);
}

void SettingsWindow::set_login_startup_port(LoginStartupPort* port) {
  login_startup_port_ = port;
  general_form_->setRowVisible(login_startup_check_, port != nullptr);
  if (port) refresh_login_startup();
}
void SettingsWindow::refresh_login_startup() {
  if (!login_startup_port_ || login_startup_pending_) return;
  login_startup_pending_ = true;
  const auto read = login_startup_port_->query();
  login_startup_error_.reset();
  if (read) login_startup_snapshot_ = read.value();
  else {
    login_startup_snapshot_.state = LoginStartupState::unknown;
    login_startup_snapshot_.can_enable = login_startup_snapshot_.can_disable = false;
    login_startup_error_ = read.error();
  }
  login_startup_pending_ = false;
  render_login_startup();
}
void SettingsWindow::persist_login_startup(bool enabled) {
  if (!login_startup_port_ || login_startup_pending_ || login_startup_dialog_) return;
  awaiting_login_startup_user_retry_ = false;
  login_startup_pending_ = true;
  login_startup_check_->setEnabled(false);
  const auto changed = LoginStartupWorkflow(*login_startup_port_).change_enabled(enabled);
  login_startup_snapshot_ = changed.snapshot;
  login_startup_error_ = changed.operation_error ? changed.operation_error : changed.query_error;
  login_startup_pending_ = false;
  const bool actual_enabled = login_startup_snapshot_.state == LoginStartupState::enabled;
  if (login_startup_error_ || actual_enabled != enabled) {
    if (enabled && !actual_enabled) awaiting_login_startup_user_retry_ = true;
    render_login_startup();
    show_login_startup_result();
  } else render_login_startup();
}
void SettingsWindow::render_login_startup() {
  const QSignalBlocker blocker(login_startup_check_);
  const auto state = login_startup_snapshot_.state;
  login_startup_check_->setChecked(state == LoginStartupState::enabled &&
                                  !awaiting_login_startup_user_retry_);
  // Unknown is preserved as a model state and tooltip. A user click can query
  // it again, but never writes unless that query establishes a capability.
  login_startup_check_->setEnabled(!login_startup_pending_ && login_startup_port_ != nullptr);
  QString hint;
  //% "Check the box again to verify launch at login."
  if (awaiting_login_startup_user_retry_) hint = qtTrId("settings.login.retry");
  //% "Launch at login could not be verified. Check the box to retry."
  else if (state == LoginStartupState::unknown) hint = qtTrId("settings.login.unknown");
  //% "Launch at login has yet to be verified. Check the box to try enabling it."
  else if (state == LoginStartupState::service_not_found) hint = qtTrId("settings.login.unconfirmed");
  else if (state == LoginStartupState::requires_approval || state == LoginStartupState::blocked)
    hint = login_startup_text(login_startup_snapshot_.approval_text, login_startup_snapshot_.approval_message);
  else hint = login_startup_diagnostic(login_startup_snapshot_);
  login_startup_check_->setToolTip(hint);
  login_startup_check_->setAccessibleDescription(hint);
}
void SettingsWindow::show_login_startup_result() {
  if (login_startup_dialog_) return;
  const auto& snapshot = login_startup_snapshot_;
  QString message;
  if (snapshot.state == LoginStartupState::requires_approval || snapshot.state == LoginStartupState::blocked)
    message = login_startup_text(snapshot.approval_text, snapshot.approval_message);
  if (message.isEmpty()) message = login_startup_diagnostic(snapshot);
  //% "Launch at login could not be changed. Check its status and try again."
  if (message.isEmpty()) message = qtTrId("settings.login.not_completed");
  if (login_startup_error_) {
    const auto& error = *login_startup_error_;
    const auto description = error.safe_context.find("description");
    const auto reason = error.safe_context.find("reason");
    if (description != error.safe_context.end()) message += "\n" + login_startup_error_field(error, "description");
    else if (reason != error.safe_context.end() && snapshot.approval_message.empty())
      message += "\n" + login_startup_error_field(error, "reason");
    const auto domain = error.safe_context.find("domain"), code = error.safe_context.find("native_code");
    if (domain != error.safe_context.end()) message += "\n" + QString::fromStdString(domain->second);
    //% " (%1)"
    if (code != error.safe_context.end()) message += qtTrId("settings.login.native_code").arg(QString::fromStdString(code->second));
  }
  if (snapshot.state == LoginStartupState::invalid_target && !snapshot.current_bundle_path.empty())
    //% "\nCurrent app: %1"
    message += qtTrId("settings.login.current_app").arg(QString::fromStdString(snapshot.current_bundle_path));
  //% "Launch at Login"
  auto* dialog = new QMessageBox(QMessageBox::Information, qtTrId("settings.login.dialog_title"), message,
                                QMessageBox::NoButton, this);
  dialog->setObjectName(QStringLiteral("loginStartupResultDialog"));
  dialog->setTextFormat(Qt::PlainText);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  //% "Cancel"
  auto* cancel = dialog->addButton(qtTrId("settings.login.cancel"), QMessageBox::RejectRole);
  dialog->setDefaultButton(cancel);
  QPushButton* system_settings = nullptr;
  if (snapshot.can_open_system_settings && snapshot.state != LoginStartupState::invalid_target) {
    //% "Open System Settings"
    system_settings = dialog->addButton(qtTrId("settings.login.open_system_settings"), QMessageBox::ActionRole);
  }
  login_startup_dialog_ = dialog;
  connect(dialog, &QMessageBox::finished, this, [this, dialog, system_settings](int) {
    const bool open = system_settings && dialog->clickedButton() == system_settings;
    dialog->hide();
    login_startup_dialog_ = nullptr;
    if (open) {
      awaiting_login_startup_user_retry_ = true;
      render_login_startup();
      // Finish dismissing the sheet before handing focus to the OS. The port
      // remains owned by the shell; this context cancels the task on deletion.
      QTimer::singleShot(0, this, [this] {
        if (!login_startup_port_) return;
        const auto opened = login_startup_port_->open_system_settings();
        if (!opened) { login_startup_error_ = opened.error(); show_login_startup_result(); }
      });
    }
  });
  dialog->open();
}

void SettingsWindow::reload_and_show() {
  if (!isVisible()) awaiting_login_startup_user_retry_ = false;
  refresh_login_startup();
  finish_hotkey_recording(false);
  const auto loaded = settings_store_.load();
  if (!loaded) {
    show_status(
        //% "Could not load settings: %1"
        qtTrId("settings.load_failed")
            .arg(QString::fromStdString(to_string(loaded.error().code))),
        true);
  } else {
    snapshot_ = loaded.value();
    restore_controls_from_snapshot();
    status_label_->hide();
  }
  QtDefaultWindowActivation fallback;
  auto& activation = activation_ != nullptr ? *activation_ :
      static_cast<QtWindowActivationPort&>(fallback);
  activation.activate(*this);
  setFocus(Qt::OtherFocusReason);
}

void SettingsWindow::restore_controls_from_snapshot() {
  render_language_choice();
  const auto& d=snapshot_.analysis_defaults;
  {QSignalBlocker b(magnifier_enabled_); magnifier_enabled_->setChecked(d.magnifier_enabled);}
  const double values[]{double(d.magnifier_zoom),double(int(d.hdr_space)),double(int(d.sdr_space)),d.reference_white_nits,d.blur_sigma_px,double(d.sample_side)};
  for(int i=0;i<6;++i) {QSignalBlocker b(analysis_combos_[i]);analysis_combos_[i]->setCurrentIndex(std::max(0,analysis_combos_[i]->findData(values[i])));}
  magnifier_readings_->set_selected(magnifier_menu_selection(d.magnifier));
  analyzer_readings_->set_selected(d.analyzer.selected);

  loading_controls_ = true;
  const QSignalBlocker pinned_topmost_blocker{pinned_always_on_top_check_};
  const QSignalBlocker pinned_edge_blocker{pinned_short_edge_combo_};
  pinned_always_on_top_check_->setChecked(snapshot_.pinned_screenshot_defaults.always_on_top);
  pinned_short_edge_combo_->setCurrentIndex(std::max(
      0, pinned_short_edge_combo_->findData(snapshot_.pinned_screenshot_defaults.short_edge)));
  const QSignalBlocker log_blocker{detailed_logging_check_};
  detailed_logging_check_->setChecked(snapshot_.detailed_logging);
  const QSignalBlocker hotkey_blocker{hotkey_edit_};
  const QSignalBlocker format_blocker{save_format_combo_};
  const QSignalBlocker diffuse_white_blocker{pq_diffuse_white_combo_};
#ifdef Q_OS_WIN
  const QSignalBlocker compatibility_blocker{windows_capture_compatibility_check_};
  const QSignalBlocker gain_check_blocker{windows_capture_gain_check_};
  const QSignalBlocker bypass_blocker{windows_capture_bypass_sdr_white_check_};
  const bool gain_enabled = snapshot_.windows_capture_gain_enabled;
  const bool bypass_enabled = snapshot_.windows_capture_bypass_sdr_white_enabled;
  windows_capture_compatibility_check_->setCheckState(
      gain_enabled && bypass_enabled ? Qt::Checked :
      gain_enabled || bypass_enabled ? Qt::PartiallyChecked : Qt::Unchecked);
  windows_capture_gain_check_->setChecked(gain_enabled);
  windows_capture_bypass_sdr_white_check_->setChecked(bypass_enabled);
  windows_capture_gain_check_->setEnabled(gain_enabled || bypass_enabled);
  windows_capture_bypass_sdr_white_check_->setEnabled(gain_enabled || bypass_enabled);
  windows_scrgb_gain_edit_->setEnabled(gain_enabled);
  const QSignalBlocker gain_blocker{windows_scrgb_gain_edit_};
  windows_scrgb_gain_edit_->setText(windows_gain_text(snapshot_.windows_scrgb_gain));
#endif
  const QSignalBlocker precision_blocker{hdr_pq_precision_combo_};
  const QSignalBlocker ultra_hdr_quality_blocker{ultra_hdr_jpeg_quality_combo_};
  const QSignalBlocker enter_blocker{enter_completion_combo_};
  const QSignalBlocker double_click_blocker{double_click_completion_combo_};

  const auto sequence = input_platform_.display_global_hotkey(
      snapshot_.global_capture_hotkey);
  if (sequence) {
    hotkey_edit_->setKeySequence(sequence.value());
  }
  folder_edit_->setText(QString::fromStdString(snapshot_.default_save_folder));
  folder_edit_->setToolTip(folder_edit_->text());
  save_format_combo_->setCurrentIndex(std::max(
      0, save_format_combo_->findData(static_cast<int>(snapshot_.save_format))));
  pq_diffuse_white_combo_->setCurrentIndex(std::max(
      0,
      pq_diffuse_white_combo_->findData(
          static_cast<int>(snapshot_.pq_diffuse_white))));
  hdr_pq_precision_combo_->setCurrentIndex(std::max(
      0,
      hdr_pq_precision_combo_->findData(
          static_cast<int>(snapshot_.hdr_pq_precision))));
  ultra_hdr_jpeg_quality_combo_->setCurrentIndex(std::max(
      0,
      ultra_hdr_jpeg_quality_combo_->findData(
          static_cast<int>(snapshot_.ultra_hdr_jpeg_quality))));
  enter_completion_combo_->setCurrentIndex(std::max(
      0,
      enter_completion_combo_->findData(
          static_cast<int>(snapshot_.enter_completion_action))));
  double_click_completion_combo_->setCurrentIndex(std::max(
      0,
      double_click_completion_combo_->findData(
          static_cast<int>(snapshot_.double_click_completion_action))));
  update_format_specific_controls();
  loading_controls_ = false;
}

void SettingsWindow::finish_saved_change(const QString& success_message) {
  const auto loaded = settings_store_.load();
  if (!loaded) {
    //% "Settings were saved, but could not be reloaded."
    show_status(qtTrId("settings.reload_failed"), true);
    return;
  }
  snapshot_ = loaded.value();
  restore_controls_from_snapshot();
  show_status(success_message, false);
  if (applied_) {
    applied_(snapshot_);
  }
}

bool SettingsWindow::event(QEvent* event) {
  if (event->type() == QEvent::WindowActivate) {
#ifdef Q_OS_WIN
    // Read the actual Windows state on return, without a second click or polling.
    awaiting_login_startup_user_retry_ = false;
    refresh_login_startup();
#else
    if (!awaiting_login_startup_user_retry_) refresh_login_startup();
#endif
  }
  if (event->type() == QEvent::Hide && login_startup_dialog_) login_startup_dialog_->reject();
  if (event->type() == QEvent::WindowDeactivate) {
    finish_hotkey_recording(false);
  }
  const auto handled = QWidget::event(event);
  if (event->type() == QEvent::FontChange ||
      event->type() == QEvent::ApplicationFontChange ||
      event->type() == QEvent::StyleChange) {
    update_general_label_width();
  }
  return handled;
}

void SettingsWindow::update_general_label_width() {
  if (general_form_ == nullptr) return;
  int width = 0;
  // Include hidden format-specific labels, so changing format never moves
  // the common controls. Recompute from the actual font, not a fixed px value.
  for (int row = 0; row < general_form_->rowCount(); ++row) {
    auto* item = general_form_->itemAt(row, QFormLayout::LabelRole);
    if (item == nullptr || item->widget() == nullptr) continue;
    item->widget()->setMinimumWidth(0);
    width = std::max(width, item->widget()->sizeHint().width());
  }
  for (int row = 0; row < general_form_->rowCount(); ++row) {
    auto* item = general_form_->itemAt(row, QFormLayout::LabelRole);
    if (item != nullptr && item->widget() != nullptr) {
      item->widget()->setMinimumWidth(width);
      if (auto* label = qobject_cast<QLabel*>(item->widget())) {
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      }
    }
  }
}

bool SettingsWindow::eventFilter(QObject* watched, QEvent* event) {
#ifdef Q_OS_WIN
  const bool mouse_button_event = event->type() == QEvent::MouseButtonPress ||
      event->type() == QEvent::MouseButtonRelease;
  QWidget* click_target = qobject_cast<QWidget*>(watched);
  if (mouse_button_event && watched == windowHandle()) {
    click_target = childAt(static_cast<QMouseEvent*>(event)->position().toPoint());
  }
  const bool compatibility_click = click_target == windows_capture_compatibility_check_ ||
      click_target == windows_capture_gain_check_ ||
      click_target == windows_capture_bypass_sdr_white_check_;
  const bool left_compatibility_click = compatibility_click && mouse_button_event &&
      static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton;
  if (left_compatibility_click && event->type() == QEvent::MouseButtonPress) {
    // Focus loss occurs before clicked. Fold pending gain text into the click's patch.
    suppress_gain_editing_finished_ = true;
  }
  if (left_compatibility_click && event->type() == QEvent::MouseButtonRelease) {
    QTimer::singleShot(0, this, [this] { suppress_gain_editing_finished_ = false; });
  }
  if (watched == windows_scrgb_gain_edit_ && event->type() == QEvent::KeyPress &&
      static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
    restore_controls_from_snapshot();
    return true;
  }
#endif
  if (recording_hotkey_ && event->type() == QEvent::MouseButtonPress) {
    auto* widget = qobject_cast<QWidget*>(watched);
    const bool inside_editor = widget == hotkey_edit_ ||
        (widget != nullptr && hotkey_edit_->isAncestorOf(widget));
    const bool inside_customize = widget == customize_hotkey_button_ ||
        (widget != nullptr && customize_hotkey_button_->isAncestorOf(widget));
    if (!inside_editor && !inside_customize) {
      finish_hotkey_recording(false);
    }
  }
  return QWidget::eventFilter(watched, event);
}


void SettingsWindow::build_pinned_screenshot_tab(QWidget* page) {
  auto* root = new QVBoxLayout(page);
  auto* scroll = new QScrollArea(page);
  scroll->setObjectName(QStringLiteral("pinnedScreenshotSettingsScrollArea"));
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->setAutoFillBackground(false);
  scroll->viewport()->setAutoFillBackground(false);
  root->addWidget(scroll);

  auto* content = new QWidget(scroll);
  auto* layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(12);
  auto* section = new QVBoxLayout();
  section->setContentsMargins(0, 0, 0, 0);
  section->setSpacing(6);
  //% "New pinned windows"
  auto* title = new QLabel(qtTrId("settings.pinned.title"), content);
  title->setObjectName(QStringLiteral("pinnedScreenshotSectionTitle"));
  section->addWidget(title);
  auto* group = new QGroupBox(content);
  group->setObjectName(QStringLiteral("pinnedScreenshotSettingsContainer"));
  auto* form = new QFormLayout(group);
  form->setHorizontalSpacing(18);
  form->setVerticalSpacing(14);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->setRowWrapPolicy(QFormLayout::DontWrapRows);
  pinned_always_on_top_check_ = new QCheckBox(group);
  //% "Always on top by default"
  pinned_always_on_top_check_->setAccessibleName(qtTrId("settings.pinned.topmost_accessible"));
  pinned_always_on_top_check_->setObjectName(QStringLiteral("pinnedScreenshotAlwaysOnTopCheckBox"));
  //% "Always on top"
  form->addRow(qtTrId("settings.pinned.topmost"), pinned_always_on_top_check_);
  pinned_short_edge_combo_ = new QComboBox(group);
  pinned_short_edge_combo_->setObjectName(QStringLiteral("pinnedScreenshotShortEdgeCombo"));
  for (int edge = minimum_pinned_screenshot_short_edge; edge <= 800; edge += 50) {
    pinned_short_edge_combo_->addItem(edge == PinnedScreenshotDefaults{}.short_edge
        //% "%1 DIP (default)"
        ? qtTrId("settings.pinned.default_size").arg(edge) : QStringLiteral("%1 DIP").arg(edge), edge);
  }
  //% "Default short edge"
  form->addRow(qtTrId("settings.pinned.short_edge"), pinned_short_edge_combo_);
  //% "Window size uses device-independent pixels (DIP) to account for display scaling."
  auto* hint = new QLabel(qtTrId("settings.pinned.scale_hint"), group);
  hint->setObjectName(QStringLiteral("pinnedScreenshotLogicalPixelHint"));
  hint->setWordWrap(true);
  form->addRow(QString(), hint);
  for (int row = 0; row < form->rowCount(); ++row) {
    auto* label = form->itemAt(row, QFormLayout::LabelRole);
    if (label && label->widget()) label->widget()->setMinimumWidth(140);
  }
  section->addWidget(group);
  layout->addLayout(section);
  auto* note = new QLabel(
      //% "Resize or change the always-on-top setting for each pinned window after opening it."
      qtTrId("settings.pinned.per_window_hint"), content);
  note->setObjectName(QStringLiteral("pinnedScreenshotDefaultsNote"));
  note->setWordWrap(true);
  layout->addWidget(note);
  layout->addStretch(1);
  scroll->setWidget(content);
  // QScrollArea::setWidget enables auto-fill; keep the native tab surface visible.
  content->setAutoFillBackground(false);
  connect(pinned_always_on_top_check_, &QCheckBox::toggled, this,
      [this] { persist_pinned_screenshot_defaults(); });
  connect(pinned_short_edge_combo_, &QComboBox::currentIndexChanged, this,
      [this] { persist_pinned_screenshot_defaults(); });
}

void SettingsWindow::persist_pinned_screenshot_defaults() {
  if (loading_controls_) return;
  const PinnedScreenshotDefaults requested{
      pinned_always_on_top_check_->isChecked(),
      pinned_short_edge_combo_->currentData().toInt()};
  if (requested == snapshot_.pinned_screenshot_defaults) return;
  const auto changed = SettingsWorkflow::change_pinned_screenshot_defaults(
      requested, settings_store_);
  if (!changed) {
    restore_controls_from_snapshot();
    //% "Could not save pinned-window settings. Previous settings restored."
    show_status(qtTrId("settings.pinned.save_failed"), true);
    return;
  }
  //% "New pinned windows will use the updated settings."
  finish_saved_change(qtTrId("settings.pinned.saved"));
}

void SettingsWindow::build_analysis_tab(QWidget* page) {
  auto* root=new QVBoxLayout(page);auto* scroll=new QScrollArea(page);scroll->setObjectName("analysisSettingsScrollArea");scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);root->addWidget(scroll);
  // Match the general tab's native group surface and contextual control colors.
  // The scroll viewport must not paint a separate Window-colored panel.
  auto* content=new QGroupBox(scroll);content->setObjectName("analysisSettingsContainer");
  scroll->setWidget(content);content->setAutoFillBackground(false);
  scroll->setAutoFillBackground(false);scroll->viewport()->setAutoFillBackground(false);
  auto* v=new QVBoxLayout(content);v->setSpacing(14);
  magnifier_enabled_=new QCheckBox(content);
  magnifier_enabled_->setObjectName("magnifierEnabledCheck");
  //% "Capture magnifier"
  magnifier_enabled_->setAccessibleName(qtTrId("settings.analysis.magnifier"));
  connect(magnifier_enabled_,&QCheckBox::clicked,this,[this]{persist_analysis_defaults();});
  auto combo=[&](int index,const char* name,const QString& label,const QStringList& labels,const std::vector<double>& values,QFormLayout* form) {
    auto* c=new QComboBox(content);c->setObjectName(name);analysis_combos_[index]=c;
    for(int i=0;i<labels.size();++i)c->addItem(labels[i],values[std::size_t(i)]);
    c->setMinimumHeight(28);form->setHorizontalSpacing(18);form->setVerticalSpacing(14);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);form->addRow(label,c);connect(c,&QComboBox::currentIndexChanged,this,[this]{persist_analysis_defaults();});
  };
  auto* mf=new QFormLayout;v->addLayout(mf);
  //% "Capture magnifier"
  mf->addRow(qtTrId("settings.analysis.magnifier"),magnifier_enabled_);
  //% "Zoom"
  combo(0,"magnifierZoomCombo",qtTrId("settings.analysis.zoom"),{"4×","8×","12×","16×"},{4,8,12,16},mf);
  magnifier_readings_ = new ReadingSelector(magnifier_reading_names(), "magnifierReadings", content);
  //% "Default readings"
  mf->addRow(qtTrId("settings.analysis.default_readings"), magnifier_readings_);
  magnifier_readings_->changed=[this]{persist_analysis_defaults();};
  auto* line=new QFrame(content);line->setFrameShape(QFrame::HLine);line->setStyleSheet("border:0;border-top:1px solid palette(mid);");v->addWidget(line);
  //% "Analysis defaults"
  auto* heading=new QLabel(qtTrId("settings.analysis.defaults"),content);auto heading_font=heading->font();heading_font.setBold(true);heading->setFont(heading_font);v->addWidget(heading);
  auto* f=new QFormLayout;v->addLayout(f);
  //% "HDR Working Space"
  combo(1,"analysisHdrSpaceCombo",qtTrId("settings.analysis.hdr_space"),{"P3-PQ","BT.2020-PQ"},{2,3},f);
  //% "SDR Working Space"
  combo(2,"analysisSdrSpaceCombo",qtTrId("settings.analysis.sdr_space"),{"P3-sRGB","sRGB-sRGB"},{1,0},f);
  //% "Reference white"
  combo(3,"analysisWhiteCombo",qtTrId("settings.analysis.reference_white"),{"203 nit","100 nit"},{203,100},f);
  //% "Analyzer processing"
  v->addWidget(new QLabel(qtTrId("settings.analysis.processing"),content));auto* processing=new QFormLayout;v->addLayout(processing);
  combo(4,"analysisBlurCombo",
      //% "Blur (σ)"
      qtTrId("settings.analysis.blur"),{
      //% "Off"
      qtTrId("settings.analysis.blur_off"),"0.5 px","1 px","2 px","4 px","8 px"},{0,.5,1,2,4,8},processing);
  //% "Analyzer eyedropper"
  v->addWidget(new QLabel(qtTrId("settings.analysis.eyedropper"),content));auto* eyedropper=new QFormLayout;v->addLayout(eyedropper);
  //% "Sample area"
  combo(5,"analysisSampleCombo",qtTrId("settings.analysis.sample_area"),{"1×1","3×3","5×5","11×11","31×31","101×101"},{1,3,5,11,31,101},eyedropper);
  analyzer_readings_ = new ReadingSelector(analyzer_reading_names(), "analyzerReadings", content);
  //% "Default readings"
  eyedropper->addRow(qtTrId("settings.analysis.default_readings"), analyzer_readings_);
  analyzer_readings_->changed=[this]{persist_analysis_defaults();};
  for(auto* form:{mf,f,processing,eyedropper})for(int row=0;row<form->rowCount();++row){auto* item=form->itemAt(row,QFormLayout::LabelRole);if(item&&item->widget())item->widget()->setMinimumWidth(130);}
  v->addStretch();
}
void SettingsWindow::persist_analysis_defaults() {
  if(loading_controls_)return;
  auto d=snapshot_.analysis_defaults;d.magnifier_enabled=magnifier_enabled_->isChecked();d.magnifier_zoom=analysis_combos_[0]->currentData().toInt();
  d.hdr_space=analysis::WorkingSpace(analysis_combos_[1]->currentData().toInt());d.sdr_space=analysis::WorkingSpace(analysis_combos_[2]->currentData().toInt());
  d.reference_white_nits=analysis_combos_[3]->currentData().toDouble();d.blur_sigma_px=analysis_combos_[4]->currentData().toDouble();d.sample_side=analysis_combos_[5]->currentData().toInt();
  d.magnifier=magnifier_readings_from_menu(magnifier_readings_->selected<MagnifierReadings::count>());d.analyzer.selected=analyzer_readings_->selected<7>();
  if(d==snapshot_.analysis_defaults)return;
  SettingsPatch patch;patch.analysis_defaults=d;auto saved=settings_store_.save(patch);
  if (!saved) {
    restore_controls_from_snapshot();
    //% "Could not save analysis settings. Previous settings restored."
    show_status(qtTrId("settings.analysis.save_failed"), true);
    return;
  }
  snapshot_.analysis_defaults = d;
  snapshot_.revision = saved.value().revision;
  //% "Analysis defaults applied."
  finish_saved_change(qtTrId("settings.analysis.saved"));
}

void SettingsWindow::choose_folder() {
  const auto chosen = QFileDialog::getExistingDirectory(
      this,
      //% "Choose Default Save Folder"
      qtTrId("settings.folder.dialog_title"),
      folder_edit_->text(),
      QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
  if (!chosen.isEmpty()) {
    const auto changed = SettingsWorkflow::change_default_folder(
        chosen.toStdString(), settings_store_);
    if (!changed) {
      restore_controls_from_snapshot();
      show_status(
          //% "Could not change the save folder: %1"
          qtTrId("settings.folder.save_failed")
              .arg(QString::fromStdString(to_string(changed.error().code))),
          true);
      return;
    }
    //% "Default save folder updated."
    finish_saved_change(qtTrId("settings.folder.saved"));
  }
}

void SettingsWindow::open_folder(const std::string& folder, const QString& label) {
  const auto opened = folder_opener_.open_folder(OpenFolderRequest{folder, true});
  if (!opened) {
    show_status(
        //% "Could not open %1: %2"
        qtTrId("settings.folder.open_failed")
            .arg(label, QString::fromStdString(to_string(opened.error().code))),
        true);
  }
}

void SettingsWindow::persist_hotkey(const std::string& canonical_hotkey) {
  const auto displayed = input_platform_.display_global_hotkey(canonical_hotkey);
  if (!displayed) {
    restore_controls_from_snapshot();
    //% "Use a modifier and a key from A–Z or 0–9."
    show_status(qtTrId("settings.shortcut.invalid"), true);
    return;
  }
  if (canonical_hotkey == snapshot_.global_capture_hotkey) {
    restore_controls_from_snapshot();
    //% "Capture shortcut is unchanged."
    show_status(qtTrId("settings.shortcut.unchanged"), false);
    return;
  }
  const auto replaced = SettingsWorkflow::replace_global_hotkey(
      snapshot_.global_capture_hotkey,
      canonical_hotkey,
      hotkey_port_,
      settings_store_);
  if (!replaced) {
    restore_controls_from_snapshot();
    show_status(
        //% "Could not change the capture shortcut: %1"
        qtTrId("settings.shortcut.save_failed")
            .arg(QString::fromStdString(to_string(replaced.error().code))),
        true);
    return;
  }
  //% "Capture shortcut updated."
  finish_saved_change(qtTrId("settings.shortcut.saved"));
}

void SettingsWindow::persist_save_format() {
  if (loading_controls_) return;
  const auto requested = static_cast<SaveFormat>(save_format_combo_->currentData().toInt());
  if (requested == snapshot_.save_format) return;
  const auto changed = SettingsWorkflow::change_save_format(requested, settings_store_);
  if (!changed) {
    restore_controls_from_snapshot();
    //% "Could not change the output format."
    show_status(qtTrId("settings.format.save_failed"), true);
    return;
  }
  //% "Output format updated."
  finish_saved_change(qtTrId("settings.format.saved"));
}

void SettingsWindow::update_format_specific_controls() {
  if (general_form_ == nullptr) return;
  const auto selected = static_cast<SaveFormat>(
      save_format_combo_->currentData().toInt());
  const bool is_png = selected == SaveFormat::png_display_p3_dual_range;
  general_form_->setRowVisible(pq_diffuse_white_combo_, is_png);
  general_form_->setRowVisible(hdr_pq_precision_combo_, is_png);
  general_form_->setRowVisible(ultra_hdr_jpeg_quality_combo_, !is_png);
}

void SettingsWindow::persist_pq_diffuse_white() {
  if (loading_controls_) return;
  const auto requested = static_cast<PqDiffuseWhite>(
      pq_diffuse_white_combo_->currentData().toInt());
  if (requested == snapshot_.pq_diffuse_white) return;
  const auto changed = SettingsWorkflow::change_pq_diffuse_white(
      requested, settings_store_);
  if (!changed) {
    restore_controls_from_snapshot();
    //% "Could not update PQ Diffuse White."
    show_status(qtTrId("settings.diffuse_white.save_failed"), true);
    return;
  }
  //% "PQ Diffuse White updated."
  finish_saved_change(qtTrId("settings.diffuse_white.saved"));
}

void SettingsWindow::persist_windows_scrgb_gain() {
#ifdef Q_OS_WIN
  if (loading_controls_ || suppress_gain_editing_finished_) return;
  persist_windows_capture_compatibility(
      snapshot_.windows_capture_gain_enabled,
      snapshot_.windows_capture_bypass_sdr_white_enabled);
#endif
}

void SettingsWindow::persist_windows_capture_compatibility(
    bool gain_enabled, bool bypass_sdr_white_enabled) {
#ifdef Q_OS_WIN
  if (loading_controls_) return;
  bool parsed = false;
  const auto editor_text = windows_scrgb_gain_edit_->text();
  double requested = QLocale::c().toDouble(editor_text, &parsed);
  if (editor_text == windows_gain_text(snapshot_.windows_scrgb_gain)) {
    requested = snapshot_.windows_scrgb_gain;
    parsed = true;
  }
  if (!parsed || !valid_windows_scrgb_gain(requested)) {
    restore_controls_from_snapshot();
    //% "Win 10 brightness fix: scRGB gain must be a finite number from 0 to 3."
    show_status(qtTrId("settings.windows_compatibility.gain_invalid"), true);
    return;
  }
  if (gain_enabled == snapshot_.windows_capture_gain_enabled &&
      bypass_sdr_white_enabled == snapshot_.windows_capture_bypass_sdr_white_enabled &&
      requested == snapshot_.windows_scrgb_gain) return;
  const auto changed = SettingsWorkflow::change_windows_capture_compatibility(
      gain_enabled, bypass_sdr_white_enabled, requested, settings_store_);
  if (!changed) {
    restore_controls_from_snapshot();
    //% "Could not update Win 10 capture compatibility."
    show_status(qtTrId("settings.windows_compatibility.save_failed"), true);
    return;
  }
  //% "Win 10 capture compatibility updated."
  finish_saved_change(qtTrId("settings.windows_compatibility.saved"));
#endif
}

void SettingsWindow::persist_hdr_pq_precision() {
  if (loading_controls_) return;
  const auto requested = static_cast<HdrPqPrecision>(
      hdr_pq_precision_combo_->currentData().toInt());
  if (requested == snapshot_.hdr_pq_precision) return;
  const auto changed = SettingsWorkflow::change_hdr_pq_precision(
      requested, settings_store_);
  if (!changed) {
    restore_controls_from_snapshot();
    //% "Could not update HDR PNG precision."
    show_status(qtTrId("settings.png_precision.save_failed"), true);
    return;
  }
  //% "HDR PNG precision updated."
  finish_saved_change(qtTrId("settings.png_precision.saved"));
}

void SettingsWindow::persist_ultra_hdr_jpeg_quality() {
  if (loading_controls_) return;
  const auto requested = static_cast<UltraHdrJpegQuality>(
      ultra_hdr_jpeg_quality_combo_->currentData().toInt());
  if (requested == snapshot_.ultra_hdr_jpeg_quality) return;
  const auto changed = SettingsWorkflow::change_ultra_hdr_jpeg_quality(
      requested, settings_store_);
  if (!changed) {
    restore_controls_from_snapshot();
    //% "Could not update JPEG quality."
    show_status(qtTrId("settings.jpeg_quality.save_failed"), true);
    return;
  }
  //% "JPEG quality updated."
  finish_saved_change(qtTrId("settings.jpeg_quality.saved"));
}

void SettingsWindow::persist_enter_completion_action() {
  if (loading_controls_) return;
  const auto requested = static_cast<CompletionAction>(
      enter_completion_combo_->currentData().toInt());
  if (requested == snapshot_.enter_completion_action) return;
  const auto changed = SettingsWorkflow::change_enter_completion_action(
      requested, settings_store_);
  if (!changed) {
    restore_controls_from_snapshot();
    //% "Could not update the Enter action."
    show_status(qtTrId("settings.completion.enter_save_failed"), true);
    return;
  }
  //% "Enter action updated."
  finish_saved_change(qtTrId("settings.completion.enter_saved"));
}

void SettingsWindow::persist_double_click_completion_action() {
  if (loading_controls_) return;
  const auto requested = static_cast<CompletionAction>(
      double_click_completion_combo_->currentData().toInt());
  if (requested == snapshot_.double_click_completion_action) return;
  const auto changed = SettingsWorkflow::change_double_click_completion_action(
      requested, settings_store_);
  if (!changed) {
    restore_controls_from_snapshot();
    //% "Could not update the double-click action."
    show_status(qtTrId("settings.completion.double_click_save_failed"), true);
    return;
  }
  //% "Double-click action updated."
  finish_saved_change(qtTrId("settings.completion.double_click_saved"));
}

void SettingsWindow::start_hotkey_recording() {
  hotkey_before_recording_ = hotkey_edit_->keySequence();
  recording_hotkey_ = true;
  //% "Cancel"
  customize_hotkey_button_->setText(qtTrId("settings.shortcut.cancel_recording"));
  hotkey_edit_->setAttribute(Qt::WA_TransparentForMouseEvents, false);
  hotkey_edit_->setFocusPolicy(Qt::StrongFocus);
  hotkey_edit_->clear();
  hotkey_edit_->setFocus(Qt::OtherFocusReason);
  //% "Press the new capture shortcut."
  show_status(qtTrId("settings.shortcut.recording"), false);
}

void SettingsWindow::finish_hotkey_recording(const bool keep_new_sequence) {
  if (!recording_hotkey_) {
    if (hotkey_edit_ != nullptr) {
      hotkey_edit_->clearFocus();
    }
    return;
  }
  const auto requested_sequence = hotkey_edit_->keySequence();
  if (!keep_new_sequence || requested_sequence.isEmpty()) {
    hotkey_edit_->setKeySequence(hotkey_before_recording_);
  }
  recording_hotkey_ = false;
  hotkey_edit_->clearFocus();
  hotkey_edit_->setFocusPolicy(Qt::NoFocus);
  hotkey_edit_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
  //% "Customize"
  customize_hotkey_button_->setText(qtTrId("settings.shortcut.customize"));
  if (keep_new_sequence) {
    const auto hotkey = input_platform_.canonical_global_hotkey(requested_sequence);
    if (!hotkey) {
      hotkey_edit_->setKeySequence(hotkey_before_recording_);
      //% "Use a modifier and a key from A–Z or 0–9."
      show_status(qtTrId("settings.shortcut.invalid"), true);
      return;
    }
    persist_hotkey(hotkey.value());
  }
}

void SettingsWindow::show_status(const QString& text, const bool error) {
  status_label_->setText(text);
  status_label_->setStyleSheet(error
      ? QStringLiteral("color: #E5484D; font-weight: 600;")
      : QStringLiteral("color: #2E9B62; font-weight: 600;"));
  status_label_->show();
}

}  // namespace hdrshot

namespace hdrshot {
void SettingsWindow::set_update_opener(std::function<void()> open){update_opener_=std::move(open);update_entry_->setEnabled(bool(update_opener_));}
void SettingsWindow::set_update_view(const update::View& view,bool){
  update_entry_->setText(view.candidate?
      //% "Update %1 available"
      qtTrId("settings.update.available").arg(QString::fromStdString(view.candidate->version.text())):
      //% "Check for Updates"
      qtTrId("settings.update.check"));
}
}
