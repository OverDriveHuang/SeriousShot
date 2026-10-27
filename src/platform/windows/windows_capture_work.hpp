#pragma once
#include <functional>
#include <memory>
#include <mutex>

namespace hdrshot {
using WindowsNativeSubmit = std::function<void(std::function<void()>)>;
void windows_submit_capture_work(std::function<void()> task);

// Independent of results: a screen owns its slot through all native cleanup,
// callback tails and the outer task (including COM apartment teardown).
class WindowsCaptureWork {
 public:
  explicit WindowsCaptureWork(std::function<void()> drained) : drained_(std::move(drained)) {}
  bool enter_callback() { std::lock_guard lock(mutex_); if (!registration_) return false; ++callbacks_; return true; }
  void leave_callback() { change([&]{ --callbacks_; }); }
  void registration_open() { std::lock_guard lock(mutex_); registration_=true; }
  void registration_closed() { change([&]{ registration_=false; }); }
  void resources_released() { change([&]{ resources_=true; }); }
  void outer_returned() { change([&]{ outer_=false; }); }
  void result_finished() { change([&]{ result_=true; }); }
 private:
  template<class F> void change(F update) {
    std::function<void()> done;
    { std::lock_guard lock(mutex_); update();
      if (result_ && !outer_ && !registration_ && !callbacks_ && resources_ && !drained_once_) {
        drained_once_=true; done=std::move(drained_);
      }
    }
    if (done) done();
  }
  std::mutex mutex_; std::function<void()> drained_;
  unsigned callbacks_{}; bool registration_{}, resources_{}, result_{}, outer_{true}, drained_once_{};
};
struct WindowsCallbackTicket {
  std::shared_ptr<WindowsCaptureWork> work;
  bool admitted{};
  explicit WindowsCallbackTicket(std::shared_ptr<WindowsCaptureWork> value)
      : work(std::move(value)), admitted(!work || work->enter_callback()) {}
  ~WindowsCallbackTicket() { if (work && admitted) work->leave_callback(); }
};
}
