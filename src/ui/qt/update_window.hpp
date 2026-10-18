#pragma once
#include "domain/update/update_policy.hpp"
#include "ui/qt/qt_window_activation_port.hpp"
#include <QWidget>
#include <functional>
class QLabel;
class QPushButton;
class QProgressBar;
class QCloseEvent;
namespace hdrshot::update {
// One application-owned native window. Closing presentation never owns/cancels work.
class UpdateWindow final : public QWidget {
public:
  explicit UpdateWindow(QWidget *parent = nullptr);
  void set_view(const View &);
  void present(QtWindowActivationPort *activation = nullptr);
  void set_release_page_opener(std::function<bool(const QUrl &)> opener);
  std::function<void()> install, later, cancel, retry, open_manual;

protected:
  void closeEvent(QCloseEvent *) override;
  void resizeEvent(QResizeEvent *) override;

private:
  void adjust_text_heights();
  View view_;
  QLabel *heading_{};
  QLabel *status_{};
  QLabel *detail_{};
  QLabel *time_{};
  QProgressBar *progress_{};
  QPushButton *releases_{};
  QPushButton *secondary_{};
  QPushButton *primary_{};
  std::function<bool(const QUrl &)> release_opener_;
};
} // namespace hdrshot::update
