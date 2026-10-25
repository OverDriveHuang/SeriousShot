#pragma once
#include "application/input_mapper.hpp"
#include "domain/analysis/types.hpp"
#include "domain/pinned_screenshot_defaults.hpp"
#include <QTimer>
#include <QWidget>
#include <functional>

class QLabel;
class QToolButton;
namespace hdrshot {
class AnalyzerSourceView;
class SourcePresentationController;
class ToolbarSymbolButton;
class PinnedScreenshotPreview;
// OS window levels and ordering belong to the platform adapter, not the widget.
class QtPinnedWindowPort {
public:
  virtual ~QtPinnedWindowPort() = default;
  virtual void set_always_on_top(QWidget &, bool) = 0;
  virtual void bring_forward(QWidget &) = 0;
  virtual void configure_preview(QWidget &, QWidget &) {}
};

class PinnedScreenshotWindow final : public QWidget {
public:
  explicit PinnedScreenshotWindow(analysis::Input input,
                                  PinnedScreenshotDefaults defaults,
                                  QWidget *parent = nullptr);
  ~PinnedScreenshotWindow() override;
  static QRect initial_geometry(QSize image, int short_edge, QRect available);
  PinnedScreenshotPreview *preview() const { return preview_; }
  AnalyzerSourceView *source_view() const { return source_; }
  analysis::SourceView presentation_view() const;
  void set_busy(bool);
  void show_error(QString);
  void show_display_error(QString);
  void clear_display_error();
  bool always_on_top() const;
  bool is_closed() const { return closed_; }
  // A native target/display change needs a new frame, not a new source.
  void refresh_presentation();
  std::function<void(analysis::SourceView, std::uintptr_t)> present_requested;
  std::function<void(UiCommand)> action_requested;
  std::function<void(bool)> topmost_changed;
  std::function<void()> front_requested;
protected:
  bool eventFilter(QObject *, QEvent *) override;
  void resizeEvent(QResizeEvent *) override;
  void showEvent(QShowEvent *) override;
  void closeEvent(QCloseEvent *) override;
  void changeEvent(QEvent *) override;
private:
  void schedule_present();
  void reveal_toolbar();
  void set_collapsed(bool);
  void layout_status(bool ensure_height = false);
  void update_theme();
  void restore_cursors();
  bool image_fully_visible() const;
  void cancel_interaction();
  analysis::Input input_;
  AnalyzerSourceView *source_{};
  QWidget *toolbar_{}, *grip_{};
  QLabel *status_{};
  QToolButton *topmost_{};
  ToolbarSymbolButton *collapse_{};
  QTimer hide_timer_;
  SourcePresentationController *presentation_{};
  PinnedScreenshotPreview *preview_{};
  bool menu_open_{}, moving_{}, resizing_{}, closed_{}, busy_{};
  bool collapsed_{}, expanded_status_visible_{};
  QSize expanded_size_, expanded_minimum_size_, expanded_maximum_size_;
  QRect expanded_source_geometry_;
  Qt::Edges resize_edges_;
  QPoint drag_origin_;
  QRect original_geometry_;
};
} // namespace hdrshot
