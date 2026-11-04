#include "ui/qt/update_window.hpp"
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QUrl>
#include <QVBoxLayout>
namespace hdrshot::update {
namespace {
QString localized_detail(const View &view) {
  auto detail = QString::fromStdString(view.detail);
  for (std::size_t i = 0; i < view.manual_open_count; ++i)
    detail.remove(0, QStringLiteral("安装包已打开。\n").size());
  switch (view.detail_kind) {
  case DetailKind::up_to_date:
    detail = /*% "Up to date" */ qtTrId("update.detail.up_to_date");
    break;
  case DetailKind::incompatible:
    if (view.candidate)
      detail = /*% "Version %1 requires a newer OS" */ qtTrId(
                   "update.detail.incompatible")
                   .arg(QString::fromStdString(view.candidate->version.text()));
    break;
  case DetailKind::download_canceled:
    detail =
        /*% "Download canceled" */ qtTrId("update.detail.download_canceled");
    break;
  case DetailKind::cancel_cleanup_failed:
    detail = /*% "Could not remove the installer" */ qtTrId(
        "update.detail.cancel_cleanup_failed");
    break;
  case DetailKind::literal:
    break;
  }
  if (view.detail_error) {
    const auto &context = view.detail_error->safe_context;
    const auto message = context.find("update_ui_id");
    if (message != context.end()) {
      detail = qtTrId(message->second.c_str());
      const auto rollback_version = context.find("update_rollback_version");
      const auto rollback_reason = context.find("update_rollback_reason_id");
      if (rollback_version != context.end() &&
          rollback_reason != context.end()) {
        auto reason = qtTrId(rollback_reason->second.c_str());
        const auto native_error = context.find("native_error");
        if (native_error != context.end())
          reason += /*% " (Windows error %1)" */ qtTrId(
                        "update.detail.rollback.native_error")
                        .arg(QString::fromStdString(native_error->second));
        detail = detail.arg(QString::fromStdString(rollback_version->second),
                            reason);
      }
    }
  }
  for (std::size_t i = 0; i < view.manual_open_count; ++i)
    detail =
        /*% "Installer opened.\n%1" */ qtTrId("update.detail.package_opened")
            .arg(detail);
  return detail;
}
} // namespace
UpdateWindow::UpdateWindow(QWidget *parent) : QWidget(parent, Qt::Window) {
  setObjectName("softwareUpdateWindow");
  setWindowTitle(/*% "Software Update" */ qtTrId("update.title"));
  setMinimumSize(400, 300);
  resize(440, 320);
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(18, 22, 18, 18);
  root->setSpacing(14);
  heading_ = new QLabel(this);
  heading_->setObjectName("updateWindowHeading");
  auto bold = heading_->font();
  bold.setBold(true);
  heading_->setFont(bold);
  heading_->setWordWrap(true);
  root->addWidget(heading_);
  status_ = new QLabel(this);
  status_->setObjectName("updateWindowStatus");
  status_->setWordWrap(true);
  root->addWidget(status_);
  progress_ = new QProgressBar(this);
  progress_->setObjectName("updateDownloadProgress");
  progress_->setRange(0, 100);
  progress_->setTextVisible(false);
  root->addWidget(progress_);
  detail_ = new QLabel(this);
  detail_->setObjectName("updateWindowDetail");
  detail_->setWordWrap(true);
  detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  root->addWidget(detail_);
  time_ = new QLabel(this);
  time_->setObjectName("updateWindowCheckTime");
  time_->setWordWrap(true);
  root->addWidget(time_);
  root->addStretch();
  auto *release_row = new QHBoxLayout;
  release_row->addStretch();
  releases_ = new QPushButton(
      /*% "Release Notes ↗" */ qtTrId("update.release_notes"), this);
  releases_->setObjectName("openReleasesButton");
  releases_->setFlat(true);
  release_row->addWidget(releases_);
  root->addLayout(release_row);
  connect(releases_, &QPushButton::clicked, this, [this] {
    const QUrl url(!view_.error && view_.phase != Phase::check_failed &&
                           view_.phase != Phase::download_failed && view_.candidate &&
                           view_.candidate->release_tag == "v" + view_.candidate->version.text()
                       ? QString("https://github.com/OverDriveHuang/SeriousShot/releases/"
                                 "tag/") +
                             QString::fromStdString(view_.candidate->release_tag)
                       : QString("https://github.com/OverDriveHuang/SeriousShot/releases"));
    if (!(release_opener_ ? release_opener_(url) : QDesktopServices::openUrl(url))) {
      detail_->setText(
          /*% "Could not open the browser. Try again in a moment." */ qtTrId(
              "update.browser_error"));
      detail_->show();
    }
  });
  auto *actions = new QHBoxLayout;
  actions->addStretch();
  secondary_ = new QPushButton(this);
  secondary_->setObjectName("updateWindowSecondary");
  primary_ = new QPushButton(this);
  primary_->setObjectName("updateWindowPrimary");
  actions->addWidget(secondary_);
  actions->addWidget(primary_);
  root->addLayout(actions);
  connect(secondary_, &QPushButton::clicked, this, &QWidget::close);
  connect(primary_, &QPushButton::clicked, this, [this] {
    if (view_.phase == Phase::ready || view_.phase == Phase::deferred ||
        view_.phase == Phase::manual_pending) {
      const auto flag=view_.error?view_.error->safe_context.find("manual_install_required"):std::map<std::string,std::string>::const_iterator{};
      if(view_.error && flag!=view_.error->safe_context.end() && flag->second=="true") {
        if(open_manual)open_manual();
      } else if (install)
        install();
    } else if (view_.phase == Phase::downloading) {
      if (cancel)
        cancel();
    } else if (retry)
      retry();
  });
  set_view({});
}
void UpdateWindow::set_release_page_opener(std::function<bool(const QUrl &)> opener) {
  release_opener_ = std::move(opener);
}
void UpdateWindow::present(QtWindowActivationPort *activation) {
  if (activation)
    activation->activate(*this);
  else {
    QtDefaultWindowActivation default_activation;
    default_activation.activate(*this);
  }
}
void UpdateWindow::closeEvent(QCloseEvent *event) {
  if ((view_.phase == Phase::ready || view_.phase == Phase::deferred ||
       view_.phase == Phase::manual_pending) &&
      later)
    later();
  event->accept(); // QWidget hides; the service and this single instance remain
                   // alive.
}
void UpdateWindow::set_view(const View &view) {
  view_ = view;
  const bool failed = view.phase == Phase::check_failed || view.phase == Phase::download_failed ||
                      view.phase == Phase::cleanup_blocked || view.error.has_value();
  releases_->setText(
      failed ? /*% "Download Manually ↗" */ qtTrId("update.download_manually")
             : qtTrId("update.release_notes"));
  const auto version =
      view.candidate ? QString::fromStdString(view.candidate->version.text()) : QString();
  heading_->setText(version.isEmpty() ? QStringLiteral("SeriousShot")
                                      : QStringLiteral("SeriousShot %1").arg(version));
  QString status, detail, action;
  bool enabled = true;
  progress_->hide();
  secondary_->setText(/*% "Close" */ qtTrId("update.close"));
  switch (view.phase) {
  case Phase::idle:
    status = view.detail.empty()
                 ? /*% "Not checked yet" */ qtTrId("update.not_checked")
                 : localized_detail(view);
    action = /*% "Check for Updates" */ qtTrId("update.check_now");
    break;
  case Phase::checking:
    status = /*% "Checking for updates… (%1/3)" */ qtTrId("update.checking")
                 .arg(view.attempts);
    enabled = false;
    break;
  case Phase::retry_wait:
    status = /*% "Waiting to retry…" */ qtTrId("update.waiting_retry");
    enabled = false;
    break;
  case Phase::cleaning:
    status = /*% "Preparing download…" */ qtTrId("update.preparing_download");
    enabled = false;
    break;
  case Phase::downloading: {
    const auto percent = view.total ? static_cast<int>(view.received * 100 / view.total) : 0;
    status =
        /*% "Downloading update · %1%" */ qtTrId("update.download_progress")
            .arg(percent);
    progress_->setValue(percent);
    progress_->show();
    detail = /*% "The download continues after this window closes." */ qtTrId(
        "update.download_continues");
    action = /*% "Cancel Download" */ qtTrId("update.cancel_download");
    break;
  }
  case Phase::verifying:
    status = /*% "Verifying update…" */ qtTrId("update.verifying");
    enabled = false;
    break;
  case Phase::ready:
  case Phase::deferred:
  case Phase::manual_pending:
    status = /*% "Install the downloaded update?" */ qtTrId(
        "update.install_question");
    secondary_->setText(/*% "Later" */ qtTrId("update.later"));
    action = /*% "Install" */ qtTrId("update.install");
    enabled = !view.busy;
    if (view.error) {
      status =
          view.error->code == ErrorCode::operation_cancelled
              ? /*% "Installation canceled" */ qtTrId("update.install_canceled")
              : /*% "Installation incomplete" */ qtTrId(
                    "update.install_incomplete");
      detail = localized_detail(view);
      const auto manual=view.error->safe_context.find("manual_install_required");
      if(manual!=view.error->safe_context.end() && manual->second=="true") {
        action = /*% "Open Package" */ qtTrId("update.open_package");
        detail +=
            //% "\nOpen the package, quit SeriousShot from the tray, then extract all ZIP files into the current folder below, replacing existing files. Run SeriousShot.exe when finished:"
            qtTrId("update.windows.manual_instructions");
        const auto directory=view.error->safe_context.find("install_directory");
        if(directory!=view.error->safe_context.end())detail+="\n"+QString::fromStdString(directory->second);
      }
    } else if (view.busy) {
      status = /*% "Update downloaded" */ qtTrId("update.downloaded");
      //% "SeriousShot will ask to install once your current task is complete."
      detail = qtTrId("update.wait_current_task");
    }
    break;
  case Phase::preparing:
    status = /*% "Checking install requirements…" */ qtTrId(
        "update.preparing_install");
    enabled = false;
    break;
  case Phase::installing:
    status = /*% "Installing and restarting…" */ qtTrId("update.installing");
    enabled = false;
    break;
  case Phase::check_failed:
    status = /*% "Update check failed: %1" */ qtTrId("update.check_failed")
                 .arg(localized_detail(view));
    action = /*% "Retry" */ qtTrId("update.retry");
    break;
  case Phase::download_failed:
    status = /*% "Download failed: %1" */ qtTrId("update.download_failed")
                 .arg(localized_detail(view));
    action = qtTrId("update.retry");
    break;
  case Phase::cleanup_blocked:
    status = /*% "Update deferred: %1" */ qtTrId("update.deferred")
                 .arg(localized_detail(view));
    action = qtTrId("update.retry");
    break;
  case Phase::recovery_required:
    status =
        /*% "Update recovery required: %1" */ qtTrId("update.recovery_required")
            .arg(localized_detail(view));
    enabled = false;
    break;
  }
  if (view.phase == Phase::check_failed || view.phase == Phase::download_failed ||
      view.phase == Phase::cleanup_blocked) {
    //% "You can continue using SeriousShot. Retry or choose “Download Manually”."
    detail = qtTrId("update.can_continue");
  }
  status_->setText(status);
  detail_->setText(detail);
  detail_->setVisible(!detail.isEmpty());
  QString times;
  if (view.last_check > 0)
    times = /*% "Last checked: %1" */ qtTrId("update.last_check")
                .arg(QDateTime::fromSecsSinceEpoch(view.last_check)
                         .toLocalTime()
                         .toString(QStringLiteral("MM-dd HH:mm")));
  if (view.last_success > 0)
    times += (times.isEmpty() ? QString() : QStringLiteral("\n")) +
             /*% "Last successful check: %1" */ qtTrId("update.last_success")
                 .arg(QDateTime::fromSecsSinceEpoch(view.last_success)
                          .toLocalTime()
                          .toString(QStringLiteral("MM-dd HH:mm")));
  time_->setText(times);
  time_->setVisible(!times.isEmpty());
  primary_->setText(action);
  primary_->setVisible(!action.isEmpty());
  primary_->setEnabled(enabled);
  adjust_text_heights();
}
void UpdateWindow::adjust_text_heights() {
  const int text_width = std::max(1, width() - 36);
  for (auto *label : {heading_, status_, detail_, time_})
    label->setMinimumHeight(label->isHidden() ? 0
                                              : std::max(label->fontMetrics().height(),
                                                         label->heightForWidth(text_width)));
  layout()->invalidate();
  layout()->activate();
  setMinimumHeight(std::max(300,layout()->minimumSize().height()));
}
void UpdateWindow::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  adjust_text_heights();
}
} // namespace hdrshot::update
