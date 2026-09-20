#pragma once

#include "application/settings_workflow.hpp"
#include "ports/system_shell_port.hpp"
#include "ui/qt/qt_input_platform_adapter.hpp"
#include "ui/qt/qt_window_activation_port.hpp"

#include <QKeySequence>
#include <QWidget>

#include <functional>
#include <string>

class QEvent;
class QObject;
class QLabel;
class QKeySequenceEdit;
class QLineEdit;
class QPushButton;
class QComboBox;
class QFormLayout;
class QCheckBox;

namespace hdrshot {

class SettingsWindow final : public QWidget {
 public:
  using Applied = std::function<void(const SettingsSnapshot&)>;

  SettingsWindow(
      SettingsStorePort& settings_store,
      GlobalHotkeyPort& hotkey_port,
      FolderOpenerPort& folder_opener,
      QtInputPlatformAdapter& input_platform,
      std::string default_global_hotkey,
      std::string diagnostics_folder,
      QWidget* parent = nullptr);

  void reload_and_show();
  void set_applied(Applied callback);
  void set_window_activation_port(QtWindowActivationPort* port) { activation_ = port; }

 protected:
  bool event(QEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void choose_folder();
  void open_folder(const std::string& folder, const QString& label);
  void restore_controls_from_snapshot();
  void finish_saved_change(const QString& success_message);
  void persist_hotkey(const std::string& canonical_hotkey);
  void persist_save_format();
  void persist_pq_diffuse_white();
  void persist_hdr_pq_precision();
  void persist_ultra_hdr_jpeg_quality();
  void persist_enter_completion_action();
  void persist_double_click_completion_action();
  void show_status(const QString& text, bool error);
  void start_hotkey_recording();
  void finish_hotkey_recording(bool keep_new_sequence);
  void update_format_specific_controls();
  void update_general_label_width();

  SettingsStorePort& settings_store_;
  GlobalHotkeyPort& hotkey_port_;
  FolderOpenerPort& folder_opener_;
  QtInputPlatformAdapter& input_platform_;
  std::string default_global_hotkey_;
  std::string diagnostics_folder_;
  SettingsSnapshot snapshot_{};
  Applied applied_;
  QKeySequenceEdit* hotkey_edit_{};
  QPushButton* customize_hotkey_button_{};
  QComboBox* enter_completion_combo_{};
  QComboBox* double_click_completion_combo_{};
  QLineEdit* folder_edit_{};
  QComboBox* save_format_combo_{};
  QComboBox* pq_diffuse_white_combo_{};
  QComboBox* hdr_pq_precision_combo_{};
  QComboBox* ultra_hdr_jpeg_quality_combo_{};
  QFormLayout* general_form_{};
  QCheckBox* detailed_logging_check_{};
  QLabel* status_label_{};
  QKeySequence hotkey_before_recording_;
  bool recording_hotkey_{};
  bool loading_controls_{};
  QtWindowActivationPort* activation_{};
};

}  // namespace hdrshot
