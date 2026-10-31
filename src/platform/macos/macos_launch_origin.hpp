#pragma once
#include <functional>
#include <memory>
namespace hdrshot {
enum class MacLaunchOrigin { manual, login };
// Install before QApplication, and synchronously freeze the Apple event while
// didFinishLaunching is being delivered. Does not replace Qt's delegate.
class MacLaunchOriginObserver final {
 public:
  MacLaunchOriginObserver();
  ~MacLaunchOriginObserver();
  MacLaunchOriginObserver(const MacLaunchOriginObserver&) = delete;
  MacLaunchOriginObserver& operator=(const MacLaunchOriginObserver&) = delete;
  void when_ready(std::function<void(MacLaunchOrigin)> consumer);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace hdrshot
