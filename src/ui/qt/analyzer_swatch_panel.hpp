#pragma once

#include "domain/analysis/swatch_pairs.hpp"

#include <QString>
#include <QIcon>
#include <QElapsedTimer>
#include <QPointer>
#include <QWidget>

#include <functional>

class QHideEvent;
class QPainter;

class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace hdrshot {

class AnalyzerSwatchPanel final : public QWidget {
public:
  explicit AnalyzerSwatchPanel(QWidget *parent = nullptr);

  void set_samples_and_result(const analysis::Request &request,
                              analysis::ResultRef result);
  void remove_swatch(std::uint64_t id);
  void set_transient_hidden(bool hidden);
  bool cancel_pair_gesture();
  void set_reading_formatter(
      std::function<QString(const analysis::Readout &, bool)> formatter);
  void set_copy_icon(QIcon icon) { copy_icon_ = std::move(icon); }

  [[nodiscard]] const analysis::SwatchPairModel &pair_model() const noexcept {
    return pairs_;
  }

  std::function<void(std::uint64_t)> removeSwatch;

protected:
  void paintEvent(QPaintEvent *) override;
  bool eventFilter(QObject *, QEvent *) override;
  void resizeEvent(QResizeEvent *) override;
  void hideEvent(QHideEvent *) override;

private:
  void rebuild();
  void update_existing();
  QWidget *card_at(const QPoint &global) const;
  QWidget *handle_for(std::uint64_t id) const;
  void try_pair(std::uint64_t source, std::uint64_t target);
  void set_transient_target(QWidget *target);
  void update_pair_row_directions();
  void update_transient_overlay();
  void draw_transient(QPainter &painter) const;
  void update_pair_success();
  void apply_pair_success_opacity();
  void prune_pair_success();
  static QString pair_key(const analysis::SwatchPair &pair);

  analysis::Request request_;
  analysis::ResultRef result_;
  analysis::SwatchPairModel pairs_;
  QScrollArea *scroll_{};
  QWidget *transient_overlay_{};
  QWidget *content_{};
  QVBoxLayout *layout_{};
  bool transient_hidden_{};
  std::optional<std::uint64_t> pending_source_;
  QPoint transient_cursor_;
  bool transient_cursor_valid_{};
  std::vector<analysis::SampleRequest> rendered_fixed_;
  std::vector<analysis::SwatchPair> rendered_pairs_;
  std::function<QString(const analysis::Readout &, bool)> reading_formatter_;
  QIcon copy_icon_;
  QPointer<QWidget> transient_target_;
  QPointer<QWidget> drag_handle_;
  std::optional<analysis::SwatchPair> transient_pair_;
  struct PairSuccess {
    analysis::SwatchPair pair;
    qint64 started_ms{};
  };
  std::vector<PairSuccess> pair_successes_;
  QElapsedTimer pair_success_clock_;
  QTimer *pair_success_timer_{};
  int scroll_restore_target_{};
  std::uint64_t scroll_restore_generation_{};
  bool scroll_restore_pending_{};
};

} // namespace hdrshot
