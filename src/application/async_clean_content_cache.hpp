#pragma once
#include "application/clean_content_cache.hpp"
#include "ports/export_task_executor_port.hpp"
#include <deque>
#include <optional>

namespace hdrshot {
// Only the serialized background task touches CleanContentCache. The queue
// mutex protects small immutable requests, never composition or GPU readback.
// The injected executor must outlive this object and its accepted tasks.
class AsyncCleanContentCache {
 public:
  using Completion = std::function<void(Result<CleanContentRef, Error>)>;
  AsyncCleanContentCache(ExportTaskExecutorPort& executor,
                        std::shared_ptr<CleanCompositionPort> composition = {});
  ~AsyncCleanContentCache();
  void preview(FrozenDesktopRef source, DisplayId display,
               std::shared_ptr<const AnnotationRenderPlan> plan, Completion done);
  // Exact requests are durable: later preview requests never replace them.
  void exact(FrozenDesktopRef source, DisplayId display,
             std::shared_ptr<const AnnotationRenderPlan> plan, Completion done);
  void close();
 private:
  struct State;
  std::shared_ptr<State> state_;
  void submit(bool exact, FrozenDesktopRef, DisplayId,
              std::shared_ptr<const AnnotationRenderPlan>, Completion);
};
} // namespace hdrshot
