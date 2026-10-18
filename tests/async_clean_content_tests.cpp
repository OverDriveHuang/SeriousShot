#include "application/async_clean_content_cache.hpp"
#include "test_support.hpp"
#include <future>
#include <algorithm>
#include <condition_variable>
#include <thread>

using namespace hdrshot;
namespace {
struct Tasks final : ExportTaskExecutorPort {
  std::vector<std::function<void()>> background_jobs, callbacks;
  std::mutex mutex;
  void background(std::function<void()> task) override {
    std::scoped_lock lock(mutex);background_jobs.push_back(std::move(task));
  }
  void main_thread(std::function<void()> task) override {
    std::scoped_lock lock(mutex);callbacks.push_back(std::move(task));
  }
  void build() {auto task=std::move(background_jobs.front());background_jobs.erase(background_jobs.begin());task();}
  void publish(bool reverse=false) {
    std::vector<std::function<void()>> pending;
    {std::scoped_lock lock(mutex);pending.swap(callbacks);}
    if(reverse) std::reverse(pending.begin(),pending.end());
    for(auto& task:pending)task();
  }
};
FrozenDesktopRef source() {
  auto f=std::make_shared<FrozenDesktop>();f->frame_id={1};f->display_generation=3;
  CanonicalFrameSegment s;s.display_id={7};s.size_px={8,4};s.pixel_format=PixelFormat::rgba32_float;
  s.encoding={ColorPrimaries::display_p3,TransferFunction::linear,AlphaMode::opaque,0};
  s.rgba_float.assign(8*4*4,.5F);f->canonical_segments.push_back(std::move(s));return f;
}
std::shared_ptr<const AnnotationRenderPlan> plan(int revision,unsigned color) {
  return std::make_shared<AnnotationRenderPlan>(AnnotationRenderPlan{
      std::uint64_t(revision),{8,4},{{{1},AnnotationKind::rectangle,{1,1,2,1},color,
      {{1,1,{128,255}}},"fixture"}},{0,0,8,4}});
}
void preview_coalesces_but_exact_requests_survive_and_match() {
  Tasks tasks;const auto f=source();AsyncCleanContentCache cache(tasks);
  const auto red=plan(1,0xff0000),green=plan(2,0x00ff00),blue=plan(3,0x0000ff);
  int cancelled=0;std::vector<std::pair<int,CleanContentRef>> completed;
  cache.preview(f,{7},red,[&](auto r){HDRSHOT_CHECK(!r && r.error().code==ErrorCode::operation_cancelled);++cancelled;});
  for(const auto& p:{red,green}) cache.exact(f,{7},p,[&,p](auto r){HDRSHOT_CHECK(bool(r));completed.emplace_back(int(p->source_document_revision),r.value());});
  cache.preview(f,{7},blue,[&](auto r){HDRSHOT_CHECK(bool(r));completed.emplace_back(3,r.value());});
  HDRSHOT_CHECK(tasks.background_jobs.size()==1);
  tasks.build();tasks.publish(true); // Deliberately reverse callback order.
  HDRSHOT_CHECK(cancelled==1 && completed.size()==3);
  CleanContentCache reference;
  for(const auto& [revision,result]:completed) {
    const auto p=revision==1?red:revision==2?green:blue;
    const auto expected=reference.get(f,{7},p);HDRSHOT_CHECK(bool(expected));
    HDRSHOT_CHECK(result->owned==expected.value()->owned && result->source==f);
    const auto roi=result->roi_plan({0,0,8,4},p->source_document_revision);
    HDRSHOT_CHECK(roi && roi.value().source_document_revision==p->source_document_revision);
  }
}
struct Composition final : CleanCompositionPort {
  bool throws{},fails{},blocked{};
  std::mutex mutex;std::condition_variable entered,release;
  bool inside{},released{};
  Result<std::vector<std::array<float,4>>,Error> compose(std::span<const CleanCompositionSample> s) override {
    if(blocked){std::unique_lock lock(mutex);inside=true;entered.notify_all();release.wait(lock,[&]{return released;});}
    if(throws)throw std::runtime_error("fixture");
    if(fails)return Result<std::vector<std::array<float,4>>,Error>::failure(
        {ErrorCode::presenter_failed,"FixtureComposition",Retryability::same_input,{{"reason","controlled_error"}}});
    return CpuCleanComposition{}.compose(s);
  }
};
void errors_retry_and_close_keep_exact_requests() {
  Tasks tasks;auto engine=std::make_shared<Composition>();AsyncCleanContentCache cache(tasks,engine);
  const auto f=source();const auto p=plan(1,0xff0000);int errors=0,success=0,cancelled=0;
  engine->fails=true;
  cache.exact(f,{7},p,[&](auto r){HDRSHOT_CHECK(!r && r.error().module=="FixtureComposition");++errors;});
  tasks.build();tasks.publish();engine->fails=false;engine->throws=true;
  cache.exact(f,{7},p,[&](auto r){HDRSHOT_CHECK(!r && r.error().safe_context.at("reason")=="composition_exception");++errors;});
  tasks.build();tasks.publish();engine->throws=false;
  cache.exact(f,{7},p,[&](auto r){HDRSHOT_CHECK(bool(r));++success;});
  cache.preview(f,{7},p,[&](auto r){HDRSHOT_CHECK(!r);++cancelled;});
  cache.close();tasks.build();tasks.publish();
  cache.exact(f,{7},p,[&](auto r){HDRSHOT_CHECK(!r && r.error().code==ErrorCode::operation_cancelled);++cancelled;});
  tasks.publish();HDRSHOT_CHECK(errors==2 && success==1 && cancelled==2);
}
void enqueue_and_close_never_wait_for_composition() {
  Tasks tasks;auto engine=std::make_shared<Composition>();engine->blocked=true;
  auto cache=std::make_unique<AsyncCleanContentCache>(tasks,engine);const auto f=source();
  int cancelled=0,exact=0;
  cache->preview(f,{7},plan(1,0xff0000),[&](auto r){HDRSHOT_CHECK(!r);++cancelled;});
  auto worker=std::async(std::launch::async,[&]{tasks.build();});
  {std::unique_lock lock(engine->mutex);HDRSHOT_CHECK(engine->entered.wait_for(lock,std::chrono::seconds(2),[&]{return engine->inside;}));}
  // The worker holds the cache's long composition lock. Both GUI operations
  // must still finish before releasing it, rather than only meeting a timeout.
  cache->exact(f,{7},plan(2,0x0000ff),[&](auto r){HDRSHOT_CHECK(bool(r));++exact;});
  cache->preview(f,{7},plan(3,0x00ff00),[&](auto r){HDRSHOT_CHECK(!r);++cancelled;});
  cache.reset();
  {std::scoped_lock lock(engine->mutex);engine->released=true;}engine->release.notify_all();
  HDRSHOT_CHECK(worker.wait_for(std::chrono::seconds(2))==std::future_status::ready);worker.get();tasks.publish();
  HDRSHOT_CHECK(cancelled==2 && exact==1);
}
}
int main(){return test::run({
  {"latest preview and durable exact document versions",preview_coalesces_but_exact_requests_survive_and_match},
  {"composition error, exception, retry and close",errors_retry_and_close_keep_exact_requests},
  {"GUI queue/close never acquires worker composition lock",enqueue_and_close_never_wait_for_composition}
});}
