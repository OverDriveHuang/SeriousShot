#pragma once
#include "ports/export_task_executor_port.hpp"
#include <QCoreApplication>
#include <QEvent>
#include <QMetaObject>
#include <QObject>
#include <QThreadPool>

namespace hdrshot {
class QtExportTaskExecutor final : public ExportTaskExecutorPort {
 public:
  explicit QtExportTaskExecutor(QObject& context) : context_(context) {
    pool_.setMaxThreadCount(1);
  }
  void background(std::function<void()> task) override { pool_.start(std::move(task)); }
  void main_thread(std::function<void()> task) override {
    QMetaObject::invokeMethod(&context_, std::move(task), Qt::QueuedConnection);
  }
  void drain() {
    pool_.waitForDone();
    QCoreApplication::sendPostedEvents(&context_, QEvent::MetaCall);
  }
 private:
  QObject& context_;
  QThreadPool pool_;
};
}  // namespace hdrshot
