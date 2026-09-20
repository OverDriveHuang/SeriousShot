#pragma once
#include <functional>
namespace hdrshot {
class ExportTaskExecutorPort {
 public:
  virtual ~ExportTaskExecutorPort() = default;
  // Background tasks run in submission order. Neither method runs inline.
  virtual void background(std::function<void()> task) = 0;
  virtual void main_thread(std::function<void()> task) = 0;
};
}  // namespace hdrshot
