#include "application/async_clean_content_cache.hpp"

namespace hdrshot {
namespace {
Error cache_cancelled() {
  return {ErrorCode::operation_cancelled,"AsyncCleanContentCache",Retryability::never,{}};
}
}
struct AsyncCleanContentCache::State {
  struct Work {
    FrozenDesktopRef source;
    DisplayId display;
    std::shared_ptr<const AnnotationRenderPlan> plan;
    Completion done;
    bool exact{};
  };
  ExportTaskExecutorPort& executor;
  CleanContentCache cache;
  std::mutex mutex;
  std::optional<Work> preview;
  std::deque<Work> exact;
  bool scheduled{}, closed{};
  State(ExportTaskExecutorPort& e,std::shared_ptr<CleanCompositionPort> c)
      :executor(e),cache(std::move(c)){}
  void deliver(Work work,Result<CleanContentRef,Error> result) {
    executor.main_thread([done=std::move(work.done),result=std::move(result)]() mutable {
      done(std::move(result));
    });
  }
  void run() {
    for(;;) {
      std::optional<Work> work;
      {
        std::scoped_lock lock(mutex);
        if(!exact.empty()) {work=std::move(exact.front());exact.pop_front();}
        else if(preview) {work=std::move(preview);preview.reset();}
        else {scheduled=false;return;}
      }
      auto result=[&] {
        try { return cache.get(work->source,work->display,work->plan); }
        catch(...) {return Result<CleanContentRef,Error>::failure(
            {ErrorCode::state_inconsistent,"AsyncCleanContentCache",Retryability::after_user_action,
             {{"reason","composition_exception"}}});}
      }();
      {
        std::scoped_lock lock(mutex);
        if(closed && !work->exact) result=Result<CleanContentRef,Error>::failure(cache_cancelled());
      }
      deliver(std::move(*work),std::move(result));
    }
  }
};
AsyncCleanContentCache::AsyncCleanContentCache(ExportTaskExecutorPort& e,
    std::shared_ptr<CleanCompositionPort> c):state_(std::make_shared<State>(e,std::move(c))){}
AsyncCleanContentCache::~AsyncCleanContentCache(){close();}
void AsyncCleanContentCache::close() {
  std::optional<State::Work> abandoned;
  {std::scoped_lock lock(state_->mutex);state_->closed=true;abandoned=std::move(state_->preview);state_->preview.reset();}
  if(abandoned) state_->deliver(std::move(*abandoned),Result<CleanContentRef,Error>::failure(cache_cancelled()));
}
void AsyncCleanContentCache::preview(FrozenDesktopRef s,DisplayId d,
    std::shared_ptr<const AnnotationRenderPlan> p,Completion c){submit(false,std::move(s),d,std::move(p),std::move(c));}
void AsyncCleanContentCache::exact(FrozenDesktopRef s,DisplayId d,
    std::shared_ptr<const AnnotationRenderPlan> p,Completion c){submit(true,std::move(s),d,std::move(p),std::move(c));}
void AsyncCleanContentCache::submit(bool exact,FrozenDesktopRef source,DisplayId display,
    std::shared_ptr<const AnnotationRenderPlan> plan,Completion done) {
  auto state=state_;
  State::Work work{std::move(source),display,std::move(plan),std::move(done),exact};
  std::optional<State::Work> abandoned;
  bool schedule=false,reject=false;
  {
    std::scoped_lock lock(state->mutex);
    reject=state->closed;
    if(!reject) {
      if(exact) state->exact.push_back(std::move(work));
      else {abandoned=std::move(state->preview);state->preview=std::move(work);}
      if(!state->scheduled){state->scheduled=true;schedule=true;}
    }
  }
  if(reject) state->deliver(std::move(work),Result<CleanContentRef,Error>::failure(cache_cancelled()));
  if(abandoned) state->deliver(std::move(*abandoned),Result<CleanContentRef,Error>::failure(cache_cancelled()));
  if(schedule) state->executor.background([state]{state->run();});
}
} // namespace hdrshot
