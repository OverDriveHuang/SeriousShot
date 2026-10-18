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
UpdateWindow::UpdateWindow(QWidget *parent) : QWidget(parent, Qt::Window) {
  setObjectName("softwareUpdateWindow");
  setWindowTitle(QStringLiteral("软件更新"));
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
  releases_ = new QPushButton(QStringLiteral("查看发布说明 ↗"), this);
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
      detail_->setText(QStringLiteral("无法打开浏览器，请稍后重试。"));
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
  releases_->setText(failed ? QStringLiteral("前往官网下载 ↗") : QStringLiteral("查看发布说明 ↗"));
  const auto version =
      view.candidate ? QString::fromStdString(view.candidate->version.text()) : QString();
  heading_->setText(version.isEmpty() ? QStringLiteral("SeriousShot")
                                      : QStringLiteral("SeriousShot %1").arg(version));
  QString status, detail, action;
  bool enabled = true;
  progress_->hide();
  secondary_->setText(QStringLiteral("关闭"));
  switch (view.phase) {
  case Phase::idle:
    status =
        view.detail.empty() ? QStringLiteral("尚未检查更新") : QString::fromStdString(view.detail);
    action = QStringLiteral("检查更新");
    break;
  case Phase::checking:
    status = QStringLiteral("正在检查更新…（%1/3）").arg(view.attempts);
    enabled = false;
    break;
  case Phase::retry_wait:
    status = QStringLiteral("正在等待重试…");
    enabled = false;
    break;
  case Phase::cleaning:
    status = QStringLiteral("正在准备更新下载…");
    enabled = false;
    break;
  case Phase::downloading: {
    const auto percent = view.total ? static_cast<int>(view.received * 100 / view.total) : 0;
    status = QStringLiteral("正在下载更新 · %1%").arg(percent);
    progress_->setValue(percent);
    progress_->show();
    detail = QStringLiteral("关闭此窗口后下载不会停止。");
    action = QStringLiteral("取消下载");
    break;
  }
  case Phase::verifying:
    status = QStringLiteral("正在校验更新…");
    enabled = false;
    break;
  case Phase::ready:
  case Phase::deferred:
  case Phase::manual_pending:
    status = QStringLiteral("新版本已下载，是否安装？");
    secondary_->setText(QStringLiteral("稍后"));
    action = QStringLiteral("安装");
    enabled = !view.busy;
    if (view.error) {
      status = view.error->code == ErrorCode::operation_cancelled ? QStringLiteral("安装已取消")
                                                                  : QStringLiteral("安装未完成");
      detail = QString::fromStdString(view.detail);
      const auto manual=view.error->safe_context.find("manual_install_required");
      if(manual!=view.error->safe_context.end() && manual->second=="true") {
        action=QStringLiteral("打开安装包");
        detail+=QStringLiteral("\n打开安装包后，从托盘退出 SeriousShot，将 ZIP 内全部文件解压覆盖到下面的当前文件夹，再运行 SeriousShot.exe：");
        const auto directory=view.error->safe_context.find("install_directory");
        if(directory!=view.error->safe_context.end())detail+="\n"+QString::fromStdString(directory->second);
      }
    } else if (view.busy) {
      status = QStringLiteral("新版本已下载");
      detail = QStringLiteral("当前工作结束后询问是否安装。");
    }
    break;
  case Phase::preparing:
    status = QStringLiteral("正在检查安装条件…");
    enabled = false;
    break;
  case Phase::installing:
    status = QStringLiteral("正在安装并重启…");
    enabled = false;
    break;
  case Phase::check_failed:
    status = QStringLiteral("检查更新失败：%1").arg(QString::fromStdString(view.detail));
    action = QStringLiteral("重试");
    break;
  case Phase::download_failed:
    status = QStringLiteral("下载失败：%1").arg(QString::fromStdString(view.detail));
    action = QStringLiteral("重试");
    break;
  case Phase::cleanup_blocked:
    status = QStringLiteral("更新暂缓：%1").arg(QString::fromStdString(view.detail));
    action = QStringLiteral("重试");
    break;
  case Phase::recovery_required:
    status = QStringLiteral("更新需要恢复：%1").arg(QString::fromStdString(view.detail));
    enabled = false;
    break;
  }
  if (view.phase == Phase::check_failed || view.phase == Phase::download_failed ||
      view.phase == Phase::cleanup_blocked)
    detail = QStringLiteral("当前程序可以继续使用。请重试，或点击“前往官网下载”手动下载。");
  status_->setText(status);
  detail_->setText(detail);
  detail_->setVisible(!detail.isEmpty());
  QString times;
  if (view.last_check > 0)
    times = QStringLiteral("最近检查：%1")
                .arg(QDateTime::fromSecsSinceEpoch(view.last_check)
                         .toLocalTime()
                         .toString(QStringLiteral("MM-dd HH:mm")));
  if (view.last_success > 0)
    times += (times.isEmpty() ? QString() : QStringLiteral("\n")) +
             QStringLiteral("最近成功：%1")
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
