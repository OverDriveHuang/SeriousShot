#pragma once

#include <QLayout>
#include <QWidget>

#include <functional>
#include <vector>

namespace hdrshot {

// A bounded two-child split. Hidden children release their space without
// changing the remembered ratio; there is deliberately no docking or global
// scroll area.
class AnalyzerSplitPane final : public QWidget {
public:
  AnalyzerSplitPane(Qt::Orientation orientation, QString name,
                    QWidget *parent = nullptr);
  void set_panes(QWidget *first, QWidget *second);
  void set_ratio(double ratio);
  [[nodiscard]] double ratio() const noexcept { return ratio_; }
  void set_minimum_spans(int first, int second);
  void relayout();
  bool cancel_drag();
  [[nodiscard]] QWidget *handle() const noexcept { return handle_; }
  std::function<void(double)> changed;

protected:
  void resizeEvent(QResizeEvent *) override;
  bool eventFilter(QObject *, QEvent *) override;

private:
  void place_from_span(double span);
  Qt::Orientation orientation_;
  QWidget *first_{};
  QWidget *second_{};
  QWidget *handle_{};
  double ratio_{.5};
  int min_first_{80};
  int min_second_{80};
  bool dragging_{};
  double start_ratio_{};
  int start_span_{};
  QPoint start_point_;
};

// Compact headers wrap at control boundaries; no control is scaled or clipped.
class AnalyzerFlowLayout final : public QLayout {
public:
  explicit AnalyzerFlowLayout(QWidget *parent = nullptr, int margin = 6,
                              int spacing = 5);
  ~AnalyzerFlowLayout() override;
  void addItem(QLayoutItem *) override;
  int count() const override;
  QLayoutItem *itemAt(int) const override;
  QLayoutItem *takeAt(int) override;
  QSize sizeHint() const override;
  QSize minimumSize() const override;
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int) const override;
  Qt::Orientations expandingDirections() const override { return {}; }
  void setGeometry(const QRect &) override;

private:
  int arrange(const QRect &, bool apply) const;
  std::vector<QLayoutItem *> items_;
};

} // namespace hdrshot
