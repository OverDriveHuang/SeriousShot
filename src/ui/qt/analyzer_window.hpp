#pragma once
#include "domain/analysis/types.hpp"
#include "ui/qt/analyzer_source_view.hpp"
#include <QWidget>
#include <functional>
#include <map>

class QLabel;
class QComboBox;
class QCheckBox;
class QToolButton;
class QFrame;
class QLineEdit;
class QVBoxLayout;
class QScrollArea;
class QCloseEvent;
class QTimer;
class QStyle;

namespace hdrshot {
class AnalyzerSplitPane;
class AnalyzerScopePlot;
class AnalyzerSwatchPanel;

class AnalyzerWindow final : public QWidget {
public:
  explicit AnalyzerWindow(analysis::Input input, std::string preferences = {},
                          QWidget *parent = nullptr);
  ~AnalyzerWindow() override;
  void set_request_handler(std::function<void(analysis::Request)> handler);
  void set_export_handler(
      std::function<void(analysis::ExportAction, analysis::ReportPlan)>
          handler);
  void set_present_handler(
      std::function<void(analysis::SourceView, std::uintptr_t)> handler);
  void set_preferences_changed(std::function<void(std::string)> handler);
  void accept_result(analysis::ResultRef result);
  void accept_error(std::uint64_t revision, const QString &message);
  void show_error(const QString &message);
  void set_export_busy(bool busy);
  [[nodiscard]] analysis::Request current_request() const { return request_; }
  [[nodiscard]] analysis::SourceView source_view() const;
  [[nodiscard]] analysis::ReportPlan report_plan();
  [[nodiscard]] std::string preferences_json() const;
  [[nodiscard]] AnalyzerSourceView *source_widget() const { return source_; }
  [[nodiscard]] AnalyzerScopePlot *waveform_plot() const { return wave_; }
  [[nodiscard]] AnalyzerScopePlot *histogram_plot() const { return hist_; }
  [[nodiscard]] AnalyzerScopePlot *vectorscope_plot() const { return vector_; }
  void set_fixture_image(QImage image);
  void add_swatch(QPointF source, int sample_size);
  void clear_swatches();
  void set_close_confirmation(
      std::function<bool()> confirm); // optional host/test policy

protected:
  void closeEvent(QCloseEvent *) override;
  void resizeEvent(QResizeEvent *) override;
  void showEvent(QShowEvent *) override;
  bool eventFilter(QObject *, QEvent *) override;

private:
  struct Reference {
    double value{};
    bool enabled{true};
  };
  struct ReferenceEditor {
    QFrame *frame{};
    QLabel *title{};
    QLabel *unit{};
    QLineEdit *input{};
    QLabel *error{};
    QComboBox *channel{};
    QWidget *list{};
    QVBoxLayout *list_layout{};
    std::size_t page{};
    QString bank_key;
  };
  void build_ui();
  void restore_preferences(const std::string &);
  void sync_controls();
  void schedule_request();
  void dispatch_request();
  void update_scope_resolution();
  void schedule_refinement();
  void persist();
  void present();
  void present_now();
  void apply_layout();
  void refresh_readouts();
  void refresh_swatches();
  QString reading_text(const analysis::Readout &readout,
                       bool fallback = false) const;
  void set_tool(int tool);
  void set_mask_shape(bool ellipse);
  void set_mask(std::optional<AnalyzerSourceMask> mask);
  void set_scope_options(const analysis::ScopeOptions &options);
  void open_references(bool histogram);
  void refresh_references(bool histogram);
  void add_reference(bool histogram);
  void update_reference_projection();
  QString reference_key(bool histogram, bool intensity) const;
  bool reference_intensity(bool histogram) const;
  void dismiss_menus();
  bool cancel_transient();
  void export_action(analysis::ExportAction action);
  void update_minimum_height();
  void update_export_buttons();
  analysis::Input input_;
  analysis::Request request_;
  analysis::ResultRef result_;
  std::optional<analysis::Request> painted_request_;
  std::optional<analysis::Request> dispatched_request_;
  std::uint64_t compatible_result_revision_{};
  std::function<void(analysis::Request)> request_handler_;
  std::function<void(analysis::ExportAction, analysis::ReportPlan)>
      export_handler_;
  std::function<void(analysis::SourceView, std::uintptr_t)> present_handler_;
  std::function<void(std::string)> preferences_changed_;
  std::function<bool()> confirm_close_;
  bool syncing_{}, report_mode_{}, gesture_active_{}, picker_{}, mask_armed_{},
      ellipse_{}, false_color_{}, closed_{};
  bool readings_pending_{true};
  bool initialized_{}, first_show_{true}, adapting_size_{};
  QSize preferred_size_{1280, 720};
  double scope_gain_{1.};
  std::array<bool, 2> export_original_{};
  std::array<bool, 5> readings_{false, false, true, false, false};
  int sample_size_{1};
  std::uint64_t next_swatch_id_{1};
  std::optional<QPointF> hover_;
  std::map<QString, std::vector<Reference>> references_;
  analysis::WaveMode last_wave_{analysis::WaveMode::intensity},
      last_parade_{analysis::WaveMode::parade_rgb};
  std::array<double, 4> ratios_{2. / 3., .43, .43, .5};
  AnalyzerSourceView *source_{};
  AnalyzerScopePlot *wave_{};
  AnalyzerScopePlot *hist_{};
  AnalyzerScopePlot *vector_{};
  std::array<AnalyzerSplitPane *, 4> split_{};
  QWidget *source_panel_{};
  QWidget *wave_panel_{};
  QWidget *hist_panel_{};
  QWidget *vector_panel_{};
  QWidget *swatch_panel_{};
  AnalyzerSwatchPanel *swatches_{};
  QScrollArea *swatch_scroll_{};
  QLabel *readout_{};
  QLabel *coordinate_{};
  QLabel *mask_readout_{};
  QLabel *false_legend_{};
  QLabel *status_{};
  QComboBox *space_{};
  QComboBox *white_{};
  QComboBox *blur_{};
  QLineEdit *gain_{};
  QComboBox *source_mode_{};
  QComboBox *source_zoom_{};
  QComboBox *wave_kind_{};
  QComboBox *wave_mode_{};
  QComboBox *hist_mode_{};
  QComboBox *vector_mode_{};
  QCheckBox *colorize_{};
  std::array<QCheckBox *, 3> visibility_{};
  std::array<QToolButton *, 4> tools_{};
  std::array<QLabel *, 3> zoom_labels_{};
  std::array<QToolButton *, 2> export_buttons_{};
  std::array<ReferenceEditor, 2> ref_editors_{};
  QTimer *request_timer_{};
  QTimer *present_timer_{};
  QTimer *refine_timer_{};
  qint64 operation_overlay_key_{};
  std::shared_ptr<const analysis::UiImage> operation_overlay_;
  QStyle *analyzer_style_{};
};
} // namespace hdrshot
