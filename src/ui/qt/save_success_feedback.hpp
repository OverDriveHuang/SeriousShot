#pragma once
#include "application/export_workflow.hpp"
#include <QFont>
#include <QPalette>
#include <QPointer>
#include <QRect>
#include <QScreen>
#include <QTimer>
#include <deque>
#include <functional>

class QLabel;
class QWidget;
namespace hdrshot {
// Value snapshot: neither the originating widget nor capture display is needed
// after submit. A moved pin uses its current screen, not its frozen capture ID.
struct SaveFeedbackAnchor {
  QRect source_geometry;
  QPointer<QScreen> screen;
  QPalette palette;
  QFont font;
};
class SaveSuccessFeedback final : public QObject {
public:
  using ConfigureWindow = std::function<void(QWidget &)>;
  explicit SaveSuccessFeedback(QObject *parent = nullptr, ConfigureWindow configure = {});
  ~SaveSuccessFeedback() override;
  static SaveFeedbackAnchor capture(QWidget *source);
  void completed(const Result<ExportReceipt, Error> &, SaveFeedbackAnchor);
  void set_capture_active(bool);
private:
  struct Message { QString path; SaveFeedbackAnchor anchor; };
  void show_next();
  void place_current();
  bool eventFilter(QObject *, QEvent *) override;
  std::deque<Message> queue_;
  Message current_;
  QPointer<QLabel> label_;
  QTimer timer_;
  ConfigureWindow configure_window_;
  bool scheduled_{};
  bool capture_active_{};
};
} // namespace hdrshot
