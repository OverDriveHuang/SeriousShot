#include "ui/qt/settings_window.hpp"
#include <QCheckBox>
#include "core/build_metadata.hpp"

#include <QFileDialog>
#include <QDateTime>
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
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

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
  setMinimumSize(800, 730);
  resize(860, 770);

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
  general_form_->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
  update_general_label_width();
  general_section->addWidget(general);
  root->addLayout(general_section);

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
  root->addLayout(shortcuts_section);

  status_label_ = new QLabel(this);
  status_label_->setObjectName(QStringLiteral("settingsStatusLabel"));
  status_label_->setWordWrap(true);
  status_label_->hide();
  root->addWidget(status_label_);
  root->addStretch(1);

  auto* identity_row = new QHBoxLayout();
  const auto source_time = QDateTime::fromString(
      QString::fromLatin1(source_commit_timestamp().data(),
                         static_cast<qsizetype>(source_commit_timestamp().size())), Qt::ISODate);
  auto* identity_label = new QLabel(QStringLiteral("版本 %1 · 代码提交：%2%3")
      .arg(QString::fromLatin1(product_version().data(), static_cast<qsizetype>(product_version().size())),
           source_time.isValid() ? source_time.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'"))
                                 : QStringLiteral("未知"),
           source_modified() ? QStringLiteral(" · 含未提交修改") : QString()), this);
  identity_label->setObjectName(QStringLiteral("buildIdentityLabel"));
  identity_label->setWordWrap(true);
  identity_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  identity_label->setToolTip(source_commit().empty()
      ? QStringLiteral("未找到可信的源代码提交信息；不会以构建时间代替。")
      : QStringLiteral("源代码提交：%1").arg(QString::fromLatin1(
            source_commit().data(), static_cast<qsizetype>(source_commit().size()))));
  identity_row->addWidget(identity_label, 1);
  auto* releases = new QPushButton(QStringLiteral("查看发布版本 ↗"), this);
  releases->setObjectName(QStringLiteral("openReleasesButton"));
  releases->setFlat(true);
  connect(releases, &QPushButton::clicked, this, [this] {
    const QUrl url(QStringLiteral("https://github.com/OverDriveHuang/SeriousShot/releases"));
    const bool opened = release_page_opener_ ? release_page_opener_(url) : QDesktopServices::openUrl(url);
    if (!opened) show_status(QStringLiteral("无法打开浏览器，请稍后重试。"), true);
  });
  identity_row->addWidget(releases);
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
  connect(hdr_pq_precision_combo_, &QComboBox::currentIndexChanged, this,
      [this] { persist_hdr_pq_precision(); });
  connect(ultra_hdr_jpeg_quality_combo_, &QComboBox::currentIndexChanged, this,
      [this] { persist_ultra_hdr_jpeg_quality(); });
  connect(enter_completion_combo_, &QComboBox::currentIndexChanged, this,
      [this] { persist_enter_completion_action(); });
  connect(double_click_completion_combo_, &QComboBox::currentIndexChanged, this,
      [this] { persist_double_click_completion_action(); });
}

void SettingsWindow::set_release_page_opener(std::function<bool(const QUrl&)> opener) {
  release_page_opener_ = std::move(opener);
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
  loading_controls_ = true;
  const QSignalBlocker log_blocker{detailed_logging_check_};
  detailed_logging_check_->setChecked(snapshot_.detailed_logging);
  const QSignalBlocker hotkey_blocker{hotkey_edit_};
  const QSignalBlocker format_blocker{save_format_combo_};
  const QSignalBlocker diffuse_white_blocker{pq_diffuse_white_combo_};
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
