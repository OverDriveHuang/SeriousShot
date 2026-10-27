#include "platform/qt/qt_capture_runtime.hpp"
#include <QObject>
#include <QMetaObject>
#include <QTimer>
#include <QThread>
#include <map>
#include <mutex>
#include <algorithm>
namespace hdrshot {
struct QtCaptureRuntime::Impl {
  struct Mailbox final : CaptureRuntimePort {
    std::mutex mutex; QObject* context{};
    std::map<TimerId, QTimer*> timers; // accessed only by GUI tasks
    CaptureSteadyTime now() const noexcept override { return std::chrono::steady_clock::now(); }
    bool post(Task task) override {
      std::lock_guard lock(mutex);
      if (!context) return false;
      return QMetaObject::invokeMethod(context, [task = std::move(task)] { task(); }, Qt::QueuedConnection);
    }
    void arm(TimerId id, CaptureSteadyTime due, Task task) override {
      // arm/disarm are owner-thread operations; no QObject escapes to native clients.
      if (!context) return;
      Q_ASSERT(QThread::currentThread() == context->thread());
      disarm(id);
      auto* timer = new QTimer(context); timers[id] = timer;
      timer->setSingleShot(true); timer->setTimerType(Qt::PreciseTimer);
      QObject::connect(timer, &QTimer::timeout, context, [this, id, task = std::move(task)] {
        disarm(id); task();
      });
      auto remaining = std::chrono::ceil<std::chrono::milliseconds>(due - now()).count();
      timer->start(static_cast<int>(std::clamp<std::int64_t>(remaining, 0, 2147483647)));
    }
    void disarm(TimerId id) override {
      auto it = timers.find(id); if (it == timers.end()) return;
      it->second->stop(); it->second->deleteLater(); timers.erase(it);
    }
  };
  std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
  std::unique_ptr<QObject> context = std::make_unique<QObject>();
  Impl() { mailbox->context = context.get(); }
  void shutdown() {
    if (!context) return;
    Q_ASSERT(QThread::currentThread() == context->thread());
    { std::lock_guard lock(mailbox->mutex); mailbox->context = nullptr; }
    mailbox->timers.clear();
    context.reset(); // removes queued callbacks and destroys every GUI timer synchronously
  }
};
QtCaptureRuntime::QtCaptureRuntime() : impl_(std::make_unique<Impl>()) {}
QtCaptureRuntime::~QtCaptureRuntime() { shutdown(); }
std::shared_ptr<CaptureRuntimePort> QtCaptureRuntime::handle() const { return impl_->mailbox; }
void QtCaptureRuntime::shutdown() { impl_->shutdown(); }
}
