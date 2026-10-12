#include <QTimer>
#include "ui/qt/analysis_readings.hpp"
#include "ui/qt/settings_window.hpp"
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
  setWindowTitle(QStringLiteral("SeriousShot 设置"));
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
  auto* author = new QLabel(QStringLiteral("by Overdrive"), this);
  author->setObjectName(QStringLiteral("authorLabel"));
  heading->addWidget(title);
  heading->addWidget(author, 0, Qt::AlignBaseline);
  heading->addStretch(1);
  root->addLayout(heading);

  auto* description = new QLabel(
      QStringLiteral("配置截图入口、完成动作与默认输出。修改后立即生效。"),
      this);
  description->setObjectName(QStringLiteral("settingsDescriptionLabel"));
  description->setWordWrap(true);
  root->addWidget(description);

  auto* tabs=new QTabWidget(this);
  tabs->setObjectName("settingsTabs");
  root->addWidget(tabs,1);
  auto* general_page=new QWidget(tabs);
  auto* general_root=new QVBoxLayout(general_page);
  general_root->setContentsMargins(0,0,0,0);
  tabs->addTab(general_page,"通用");
  auto* analysis_page=new QWidget(tabs);
  tabs->addTab(analysis_page,"分析");
  build_analysis_tab(analysis_page);
  const auto analysis_margins = analysis_page->layout()->contentsMargins();
  general_root->setContentsMargins(analysis_margins);
  auto* general_section = new QVBoxLayout();
  general_section->setContentsMargins(0, 0, 0, 0);
  general_section->setSpacing(6);
  auto* general_label = new QLabel(QStringLiteral("通用"), this);
  general_label->setObjectName(QStringLiteral("generalSectionLabel"));
  general_label->setFont(description->font());
  general_section->addWidget(general_label);
  auto* general = new QGroupBox(this);
  general->setObjectName(QStringLiteral("generalSettingsContainer"));
  general_form_ = new QFormLayout(general);
  general_form_->setSizeConstraint(QLayout::SetMinimumSize);
  general_form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  general_form_->setRowWrapPolicy(QFormLayout::DontWrapRows);
  general_form_->setHorizontalSpacing(18);
  general_form_->setVerticalSpacing(14);
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
  customize_hotkey_button_ = new QPushButton(QStringLiteral("自定义"), hotkey_row);
  customize_hotkey_button_->setObjectName(QStringLiteral("customizeGlobalHotkeyButton"));
  connect(customize_hotkey_button_, &QPushButton::clicked, this, [this] {
    if (recording_hotkey_) {
      finish_hotkey_recording(false);
    } else {
      start_hotkey_recording();
    }
  });
  auto* reset_hotkey = new QPushButton(QStringLiteral("恢复默认"), hotkey_row);
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
  general_form_->addRow(QStringLiteral("全局截图快捷键"), hotkey_row);

  const auto add_completion_items = [](QComboBox* combo) {
    combo->addItem(
        QStringLiteral("复制到剪贴板"),
        static_cast<int>(CompletionAction::copy_to_clipboard));
    combo->addItem(
        QStringLiteral("保存到默认文件夹"),
        static_cast<int>(CompletionAction::save_default));
    combo->addItem(
        QStringLiteral("另存为…"),
        static_cast<int>(CompletionAction::save_as));
  };
  enter_completion_combo_ = new QComboBox(general);
  enter_completion_combo_->setObjectName(QStringLiteral("enterCompletionActionCombo"));
  enter_completion_combo_->setMinimumWidth(390);
  add_completion_items(enter_completion_combo_);
  general_form_->addRow(QStringLiteral("按 Enter 时"), enter_completion_combo_);

  double_click_completion_combo_ = new QComboBox(general);
  double_click_completion_combo_->setObjectName(
      QStringLiteral("doubleClickCompletionActionCombo"));
  double_click_completion_combo_->setMinimumWidth(390);
  add_completion_items(double_click_completion_combo_);
  general_form_->addRow(
      QStringLiteral("双击选区时"), double_click_completion_combo_);

  auto* folder_row = new QWidget(general);
  auto* folder_layout = new QHBoxLayout(folder_row);
  folder_layout->setContentsMargins(0, 0, 0, 0);
  folder_edit_ = new QLineEdit(folder_row);
  folder_edit_->setObjectName(QStringLiteral("defaultSaveFolderEdit"));
  folder_edit_->setReadOnly(true);
  folder_edit_->setMinimumWidth(160);
  folder_edit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  auto* choose = new QPushButton(QStringLiteral("选择"), folder_row);
  choose->setObjectName(QStringLiteral("chooseDefaultSaveFolderButton"));
  choose->setFixedWidth(96);
  auto* open = new QPushButton(QStringLiteral("打开"), folder_row);
  open->setObjectName(QStringLiteral("openDefaultSaveFolderButton"));
  open->setFixedWidth(96);
  connect(choose, &QPushButton::clicked, this, [this] { choose_folder(); });
  connect(open, &QPushButton::clicked, this, [this] {
    open_folder(folder_edit_->text().toStdString(), QStringLiteral("默认保存文件夹"));
  });
  folder_layout->addWidget(folder_edit_, 1);
  folder_layout->addWidget(choose);
  folder_layout->addWidget(open);
  general_form_->addRow(QStringLiteral("默认保存文件夹"), folder_row);

  save_format_combo_ = new QComboBox(general);
  save_format_combo_->setObjectName(QStringLiteral("saveFormatCombo"));
  save_format_combo_->setMinimumWidth(390);
  save_format_combo_->addItem(
      QStringLiteral("PNG（SDR：Display P3 ICC；HDR：Display P3 PQ）"),
      static_cast<int>(SaveFormat::png_display_p3_dual_range));
  save_format_combo_->addItem(
      QStringLiteral("JPEG（ISO 21496-1 + XMP）"),
      static_cast<int>(SaveFormat::ultra_hdr_jpeg));
  general_form_->addRow(QStringLiteral("保存为"), save_format_combo_);
  pq_diffuse_white_combo_ = new QComboBox(general);
  pq_diffuse_white_combo_->setObjectName(QStringLiteral("pqDiffuseWhiteCombo"));
  pq_diffuse_white_combo_->setMinimumWidth(390);
  pq_diffuse_white_combo_->addItem(
      QStringLiteral("100 nit"), static_cast<int>(PqDiffuseWhite::nits_100));
  pq_diffuse_white_combo_->addItem(
      QStringLiteral("203 nit"), static_cast<int>(PqDiffuseWhite::nits_203));
  pq_diffuse_white_combo_->setToolTip(
      QStringLiteral("只影响 HDR 的 Display P3 PQ 输出；SDR 输出不使用此值。"));
  general_form_->addRow(
      QStringLiteral("Diffuse White 在 PQ 中的亮度"), pq_diffuse_white_combo_);
  hdr_pq_precision_combo_ = new QComboBox(general);
  hdr_pq_precision_combo_->setObjectName(QStringLiteral("hdrPqPrecisionCombo"));
  hdr_pq_precision_combo_->setMinimumWidth(390);
  hdr_pq_precision_combo_->addItem(
      QStringLiteral("超高质量（16-bit PQ）"),
      static_cast<int>(HdrPqPrecision::bits_16));
  hdr_pq_precision_combo_->addItem(
      QStringLiteral("高质量（12-bit PQ）"),
      static_cast<int>(HdrPqPrecision::bits_12));
  hdr_pq_precision_combo_->addItem(
      QStringLiteral("小体积（10-bit PQ，默认）"),
      static_cast<int>(HdrPqPrecision::bits_10));
  hdr_pq_precision_combo_->setToolTip(
      QStringLiteral("只影响 HDR PQ 的有效码值精度；PNG 容器仍为标准 16-bit RGB。"));
  general_form_->addRow(QStringLiteral("HDR PNG 数据精度"), hdr_pq_precision_combo_);
  ultra_hdr_jpeg_quality_combo_ = new QComboBox(general);
  ultra_hdr_jpeg_quality_combo_->setObjectName(
      QStringLiteral("ultraHdrJpegQualityCombo"));
  ultra_hdr_jpeg_quality_combo_->setMinimumWidth(390);
  ultra_hdr_jpeg_quality_combo_->addItem(
      QStringLiteral("超高质量（100）"),
      static_cast<int>(UltraHdrJpegQuality::maximum));
  ultra_hdr_jpeg_quality_combo_->addItem(
      QStringLiteral("均衡（95，默认）"),
      static_cast<int>(UltraHdrJpegQuality::balanced));
  ultra_hdr_jpeg_quality_combo_->addItem(
      QStringLiteral("小体积（85）"),
      static_cast<int>(UltraHdrJpegQuality::compact));
  ultra_hdr_jpeg_quality_combo_->setToolTip(
      QStringLiteral("同时用于 SDR 底图与增益图 JPEG；纯 SDR 内容仅生成普通 JPEG。"));
  general_form_->addRow(
      QStringLiteral("JPEG 质量"), ultra_hdr_jpeg_quality_combo_);
#ifdef Q_OS_WIN
  auto* compatibility_row = new QWidget(general);
  auto* compatibility_layout = new QHBoxLayout(compatibility_row);
  compatibility_layout->setContentsMargins(0, 0, 0, 0);
  windows_capture_compatibility_check_ = new QCheckBox(
      QStringLiteral("Windows 10 截图亮度兼容"), compatibility_row);
  windows_capture_compatibility_check_->setObjectName(
      QStringLiteral("windowsCaptureCompatibilityCheckBox"));
  windows_capture_compatibility_check_->setTristate(true);
  compatibility_layout->addWidget(windows_capture_compatibility_check_);
  auto* compatibility_help = new QLabel(
      QStringLiteral("部分 Windows 10 版本截图亮度异常时可尝试开启，默认关闭。"),
      compatibility_row);
  compatibility_help->setObjectName(QStringLiteral("windowsCaptureCompatibilityHelp"));
  auto help_palette = compatibility_help->palette();
  auto help_color = help_palette.color(QPalette::WindowText);
  help_color.setAlphaF(0.68f);
  help_palette.setColor(QPalette::WindowText, help_color);
  compatibility_help->setPalette(help_palette);
  compatibility_layout->addWidget(compatibility_help);
  compatibility_layout->addStretch(1);
  general_form_->addRow(compatibility_row);

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
  windows_scrgb_gain_edit_->setToolTip(QStringLiteral("Windows 截图源的 scRGB RGB 增益（0–3）；默认 0.5。"));
  windows_scrgb_gain_edit_->installEventFilter(this);
  options_layout->addWidget(windows_scrgb_gain_edit_);
  options_layout->addSpacing(18);
  windows_capture_bypass_sdr_white_check_ = new QCheckBox(
      QStringLiteral("HDR 捕获不使用 SDRWhiteLevel 归一化。"), compatibility_options);
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
  auto* shortcuts_label = new QLabel(QStringLiteral("截图界面固定操作"), this);
  shortcuts_label->setObjectName(QStringLiteral("fixedOperationsSectionLabel"));
  shortcuts_label->setFont(description->font());
  shortcuts_section->addWidget(shortcuts_label);
  auto* shortcuts = new QGroupBox(this);
  shortcuts->setObjectName(QStringLiteral("fixedOperationsContainer"));
  auto* shortcuts_form = new QFormLayout(shortcuts);
  shortcuts_form->setHorizontalSpacing(18);
  shortcuts_form->setVerticalSpacing(10);
  shortcuts_form->addRow(QStringLiteral("取消"), shortcut_value(input_platform_.fixed_shortcut_label(UiCommand::cancel_capture), shortcuts));
  shortcuts_form->addRow(QStringLiteral("保存"), shortcut_value(input_platform_.fixed_shortcut_label(UiCommand::save_default), shortcuts));
  shortcuts_form->addRow(QStringLiteral("另存为"), shortcut_value(input_platform_.fixed_shortcut_label(UiCommand::save_as), shortcuts));
  shortcuts_form->addRow(QStringLiteral("撤销"), shortcut_value(input_platform_.fixed_shortcut_label(UiCommand::undo), shortcuts));
  shortcuts_form->addRow(QStringLiteral("重做"), shortcut_value(input_platform_.fixed_shortcut_label(UiCommand::redo), shortcuts));
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
  auto* identity_label = new QLabel(QStringLiteral("版本 %1").arg(QString::fromLatin1(product_version().data(),static_cast<qsizetype>(product_version().size()))),this);
  identity_label->setObjectName(QStringLiteral("buildIdentityLabel"));
  identity_label->setWordWrap(true);
  identity_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  identity_label->setToolTip(source_commit().empty()
      ? QStringLiteral("未找到可信的源代码提交信息；不会以构建时间代替。")
      : (source_commit().size() == 64 ? QStringLiteral("源码快照 SHA-256：%1")
                                      : QStringLiteral("源代码提交：%1")).arg(QString::fromLatin1(
            source_commit().data(), static_cast<qsizetype>(source_commit().size()))));
  const auto source_time=QDateTime::fromString(QString::fromLatin1(source_commit_timestamp().data(),static_cast<qsizetype>(source_commit_timestamp().size())),Qt::ISODate);
  identity_label->setToolTip(identity_label->toolTip()+QStringLiteral("\n源代码时间：")+(source_time.isValid()?source_time.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'")):QStringLiteral("未知"))+(source_modified()?QStringLiteral("\n含未提交修改"):QString()));
  identity_row->addWidget(identity_label, 1);
  update_entry_=new QPushButton(QStringLiteral("查看更新"),this);
  update_entry_->setObjectName(QStringLiteral("checkUpdatesButton"));update_entry_->setFlat(true);update_entry_->setEnabled(false);
  identity_row->addWidget(update_entry_);
  connect(update_entry_,&QPushButton::clicked,this,[this]{if(update_opener_)update_opener_();});
  root->addLayout(identity_row);

  auto* actions = new QHBoxLayout();
  actions->addStretch(1);
  detailed_logging_check_ = new QCheckBox(QStringLiteral("详细日志（调试）"), this);
  detailed_logging_check_->setObjectName(QStringLiteral("detailedLoggingCheckBox"));
  actions->addWidget(detailed_logging_check_);
  connect(detailed_logging_check_, &QCheckBox::toggled, this, [this](bool enabled) {
    if (loading_controls_) return;
    const auto saved = SettingsWorkflow::change_detailed_logging(enabled, settings_store_);
    if (!saved) {
      restore_controls_from_snapshot();
      show_status(QStringLiteral("日志设置保存失败：%1")
          .arg(QString::fromStdString(to_string(saved.error().code))), true);
      return;
    }
    finish_saved_change(QStringLiteral("日志设置已生效"));
  });
  auto* open_logs = new QPushButton(QStringLiteral("打开日志文件夹"), this);
  open_logs->setObjectName(QStringLiteral("openLogsFolderButton"));
  connect(open_logs, &QPushButton::clicked, this, [this] {
    open_folder(diagnostics_folder_, QStringLiteral("日志文件夹"));
  });
  auto* close = new QPushButton(QStringLiteral("关闭"), this);
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

void SettingsWindow::set_applied(Applied callback) {
  applied_ = std::move(callback);
}

void SettingsWindow::reload_and_show() {
  finish_hotkey_recording(false);
  const auto loaded = settings_store_.load();
  if (!loaded) {
    show_status(
        QStringLiteral("设置读取失败：%1")
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
  const auto& d=snapshot_.analysis_defaults;
  {QSignalBlocker b(magnifier_enabled_); magnifier_enabled_->setChecked(d.magnifier_enabled);}
  const double values[]{double(d.magnifier_zoom),double(int(d.hdr_space)),double(int(d.sdr_space)),d.reference_white_nits,d.blur_sigma_px,double(d.sample_side)};
  for(int i=0;i<6;++i) {QSignalBlocker b(analysis_combos_[i]);analysis_combos_[i]->setCurrentIndex(std::max(0,analysis_combos_[i]->findData(values[i])));}
  magnifier_readings_->set_selected(d.magnifier.selected);
  analyzer_readings_->set_selected(d.analyzer.selected);

  loading_controls_ = true;
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
    show_status(QStringLiteral("设置已写入，但重新读取失败。"), true);
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


void SettingsWindow::build_analysis_tab(QWidget* page) {
  auto* root=new QVBoxLayout(page);auto* scroll=new QScrollArea(page);scroll->setObjectName("analysisSettingsScrollArea");scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);root->addWidget(scroll);
  // Match the general tab's native group surface and contextual control colors.
  // The scroll viewport must not paint a separate Window-colored panel.
  auto* content=new QGroupBox(scroll);content->setObjectName("analysisSettingsContainer");
  scroll->setWidget(content);content->setAutoFillBackground(false);
  scroll->setAutoFillBackground(false);scroll->viewport()->setAutoFillBackground(false);
  auto* v=new QVBoxLayout(content);v->setSpacing(14);
  auto* title=new QHBoxLayout;title->addWidget(new QLabel("截图放大镜",content));title->addStretch();
  magnifier_enabled_=new QCheckBox("启用",content);magnifier_enabled_->setObjectName("magnifierEnabledCheck");title->addWidget(magnifier_enabled_);v->addLayout(title);
  connect(magnifier_enabled_,&QCheckBox::clicked,this,[this]{persist_analysis_defaults();});
  auto combo=[&](int index,const char* name,const QString& label,const QStringList& labels,const std::vector<double>& values,QFormLayout* form) {
    auto* c=new QComboBox(content);c->setObjectName(name);analysis_combos_[index]=c;
    for(int i=0;i<labels.size();++i)c->addItem(labels[i],values[std::size_t(i)]);
    c->setMinimumHeight(28);form->setHorizontalSpacing(18);form->setVerticalSpacing(14);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);form->addRow(label,c);connect(c,&QComboBox::currentIndexChanged,this,[this]{persist_analysis_defaults();});
  };
  auto* mf=new QFormLayout;v->addLayout(mf);combo(0,"magnifierZoomCombo","放大倍率",{"4×","8×","12×","16×"},{4,8,12,16},mf);
  magnifier_readings_=new ReadingSelector(magnifier_reading_names(),"magnifierReadings",content);mf->addRow("默认读数",magnifier_readings_);
  magnifier_readings_->changed=[this]{persist_analysis_defaults();};
  auto* line=new QFrame(content);line->setFrameShape(QFrame::HLine);line->setStyleSheet("border:0;border-top:1px solid palette(mid);");v->addWidget(line);
  auto* heading=new QLabel("分析默认设置",content);auto heading_font=heading->font();heading_font.setBold(true);heading->setFont(heading_font);v->addWidget(heading);
  auto* f=new QFormLayout;v->addLayout(f);
  combo(1,"analysisHdrSpaceCombo","HDR 默认工作空间",{"P3-PQ","BT.2020-PQ"},{2,3},f);
  combo(2,"analysisSdrSpaceCombo","SDR 默认工作空间",{"P3-sRGB","sRGB-sRGB"},{1,0},f);
  combo(3,"analysisWhiteCombo","分析参考白",{"203 nit","100 nit"},{203,100},f);
  v->addWidget(new QLabel("分析器默认处理",content));auto* processing=new QFormLayout;v->addLayout(processing);
  combo(4,"analysisBlurCombo","Blur（σ）",{"关闭","0.5 px","1 px","2 px","4 px","8 px"},{0,.5,1,2,4,8},processing);
  combo(5,"analysisSampleCombo","采样范围",{"1×1","3×3","5×5","11×11","31×31","101×101"},{1,3,5,11,31,101},processing);
  analyzer_readings_=new ReadingSelector(analyzer_reading_names(),"analyzerReadings",content);processing->addRow("默认读数",analyzer_readings_);
  analyzer_readings_->changed=[this]{persist_analysis_defaults();};
  for(auto* form:{mf,f,processing})for(int row=0;row<form->rowCount();++row){auto* item=form->itemAt(row,QFormLayout::LabelRole);if(item&&item->widget())item->widget()->setMinimumWidth(130);}
  v->addStretch();
}
void SettingsWindow::persist_analysis_defaults() {
  if(loading_controls_)return;
  auto d=snapshot_.analysis_defaults;d.magnifier_enabled=magnifier_enabled_->isChecked();d.magnifier_zoom=analysis_combos_[0]->currentData().toInt();
  d.hdr_space=analysis::WorkingSpace(analysis_combos_[1]->currentData().toInt());d.sdr_space=analysis::WorkingSpace(analysis_combos_[2]->currentData().toInt());
  d.reference_white_nits=analysis_combos_[3]->currentData().toDouble();d.blur_sigma_px=analysis_combos_[4]->currentData().toDouble();d.sample_side=analysis_combos_[5]->currentData().toInt();
  d.magnifier.selected=magnifier_readings_->selected<9>();d.analyzer.selected=analyzer_readings_->selected<7>();
  if(d==snapshot_.analysis_defaults)return;
  SettingsPatch patch;patch.analysis_defaults=d;auto saved=settings_store_.save(patch);
  if(!saved){restore_controls_from_snapshot();show_status("分析设置保存失败，已恢复",true);return;}
  snapshot_.analysis_defaults=d;snapshot_.revision=saved.value().revision;finish_saved_change("分析默认设置已生效");
}

void SettingsWindow::choose_folder() {
  const auto chosen = QFileDialog::getExistingDirectory(
      this,
      QStringLiteral("选择默认保存文件夹"),
      folder_edit_->text(),
      QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
  if (!chosen.isEmpty()) {
    const auto changed = SettingsWorkflow::change_default_folder(
        chosen.toStdString(), settings_store_);
    if (!changed) {
      restore_controls_from_snapshot();
      show_status(
          QStringLiteral("保存位置更新失败：%1")
              .arg(QString::fromStdString(to_string(changed.error().code))),
          true);
      return;
    }
    finish_saved_change(QStringLiteral("默认保存位置已更新。"));
  }
}

void SettingsWindow::open_folder(const std::string& folder, const QString& label) {
  const auto opened = folder_opener_.open_folder(OpenFolderRequest{folder, true});
  if (!opened) {
    show_status(
        QStringLiteral("%1无法打开：%2")
            .arg(label, QString::fromStdString(to_string(opened.error().code))),
        true);
  }
}

void SettingsWindow::persist_hotkey(const std::string& canonical_hotkey) {
  const auto displayed = input_platform_.display_global_hotkey(canonical_hotkey);
  if (!displayed) {
    restore_controls_from_snapshot();
    show_status(QStringLiteral("快捷键必须包含修饰键，并使用 A–Z 或 0–9。"), true);
    return;
  }
  if (canonical_hotkey == snapshot_.global_capture_hotkey) {
    restore_controls_from_snapshot();
    show_status(QStringLiteral("全局截图快捷键未改变。"), false);
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
        QStringLiteral("快捷键更新失败：%1")
            .arg(QString::fromStdString(to_string(replaced.error().code))),
        true);
    return;
  }
  finish_saved_change(QStringLiteral("全局截图快捷键已更新。"));
}

void SettingsWindow::persist_save_format() {
  if (loading_controls_) return;
  const auto requested = static_cast<SaveFormat>(save_format_combo_->currentData().toInt());
  if (requested == snapshot_.save_format) return;
  const auto changed = SettingsWorkflow::change_save_format(requested, settings_store_);
  if (!changed) {
    restore_controls_from_snapshot();
    show_status(QStringLiteral("保存格式更新失败。"), true);
    return;
  }
  finish_saved_change(QStringLiteral("保存格式已更新。"));
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
    show_status(QStringLiteral("PQ Diffuse White 更新失败。"), true);
    return;
  }
  finish_saved_change(QStringLiteral("PQ Diffuse White 已更新。"));
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
    show_status(QStringLiteral("Windows 10 截图亮度兼容：scRGB gain 须为 0 至 3 的有限小数。"), true);
    return;
  }
  if (gain_enabled == snapshot_.windows_capture_gain_enabled &&
      bypass_sdr_white_enabled == snapshot_.windows_capture_bypass_sdr_white_enabled &&
      requested == snapshot_.windows_scrgb_gain) return;
  const auto changed = SettingsWorkflow::change_windows_capture_compatibility(
      gain_enabled, bypass_sdr_white_enabled, requested, settings_store_);
  if (!changed) {
    restore_controls_from_snapshot();
    show_status(QStringLiteral("Windows 10 截图亮度兼容设置更新失败。"), true);
    return;
  }
  finish_saved_change(QStringLiteral("Windows 10 截图亮度兼容设置已更新。"));
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
    show_status(QStringLiteral("HDR PNG 数据精度更新失败。"), true);
    return;
  }
  finish_saved_change(QStringLiteral("HDR PNG 数据精度已更新。"));
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
    show_status(QStringLiteral("JPEG 质量更新失败。"), true);
    return;
  }
  finish_saved_change(QStringLiteral("JPEG 质量已更新。"));
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
    show_status(QStringLiteral("Enter 行为更新失败。"), true);
    return;
  }
  finish_saved_change(QStringLiteral("Enter 行为已更新。"));
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
    show_status(QStringLiteral("双击行为更新失败。"), true);
    return;
  }
  finish_saved_change(QStringLiteral("双击行为已更新。"));
}

void SettingsWindow::start_hotkey_recording() {
  hotkey_before_recording_ = hotkey_edit_->keySequence();
  recording_hotkey_ = true;
  customize_hotkey_button_->setText(QStringLiteral("取消录制"));
  hotkey_edit_->setAttribute(Qt::WA_TransparentForMouseEvents, false);
  hotkey_edit_->setFocusPolicy(Qt::StrongFocus);
  hotkey_edit_->clear();
  hotkey_edit_->setFocus(Qt::OtherFocusReason);
  show_status(QStringLiteral("请按下新的全局截图快捷键。"), false);
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
  customize_hotkey_button_->setText(QStringLiteral("自定义"));
  if (keep_new_sequence) {
    const auto hotkey = input_platform_.canonical_global_hotkey(requested_sequence);
    if (!hotkey) {
      hotkey_edit_->setKeySequence(hotkey_before_recording_);
      show_status(QStringLiteral("快捷键必须包含修饰键，并使用 A–Z 或 0–9。"), true);
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
  update_entry_->setText(view.candidate?QStringLiteral("发现新版本 %1 · 查看更新").arg(QString::fromStdString(view.candidate->version.text())):QStringLiteral("查看更新"));
}
}
