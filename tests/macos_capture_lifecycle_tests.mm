#include "platform/macos/macos_capture_ports.hpp"
#include "platform/macos/screen_capture_work.hpp"
#include "test_support.hpp"
#include <array>
#include <stdexcept>
#include "application/capture_session.hpp"
#include "application/capture_progress_watchdog.hpp"
#include "application/multi_display_preview_presenter.hpp"
#include "platform/macos/macos_preview_presenter_port.hpp"
#include "platform/qt/qt_capture_runtime.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <condition_variable>
#include <thread>
#include <dispatch/dispatch.h>
using namespace hdrshot;
namespace {
struct Image {
  CGImageRef image{};
  Image() {
    std::array<std::uint16_t,4> pixels{0x3800,0x4000,0x3c00,0x3c00};
    auto data = CFDataCreate(nullptr,reinterpret_cast<const UInt8*>(pixels.data()),sizeof(pixels));
    auto provider = CGDataProviderCreateWithCFData(data); auto space = CGColorSpaceCreateWithName(kCGColorSpaceExtendedDisplayP3);
    image = CGImageCreate(1,1,16,64,8,space,static_cast<CGBitmapInfo>(kCGBitmapFloatComponents|kCGBitmapByteOrder16Little|kCGImageAlphaLast),provider,nullptr,false,kCGRenderingIntentDefault);
    CGColorSpaceRelease(space); CGDataProviderRelease(provider); CFRelease(data);
  }
  ~Image() { CGImageRelease(image); }
};
CaptureBatchRequest request() { return {{1},{1},1,{{1},{2}},PixelFormat::rgba16_float,true}; }
void synchronous_nested_tickets() {
  Image image; int results{}, drains{}, progresses{}; bool held_image_submit{}, held_content_submit{};
  MacCaptureHooks hooks; hooks.image_received=[&] { ++progresses; };
  hooks.drained=[&] { ++drains; };
  MacNativeCaptureFunctions native{[&](std::uint32_t, MacContentCallback callback) {
    callback(Result<MacImageSubmit,Error>::success([&](MacImageCallback image_callback) {
      image_callback(image.image,{}); held_image_submit = drains == 0 && results == 1;
      image_callback(image.image,{}); HDRSHOT_CHECK(results == 1);
    }));
    held_content_submit = drains == 0; // outer native submission has not returned
  }};
  mac_capture_with_native(1,[&](auto result) { HDRSHOT_CHECK(result.has_value()); ++results; }, hooks, native);
  HDRSHOT_CHECK(results == 1 && progresses == 1 && drains == 1 && held_image_submit && held_content_submit);
}
void cancelled_content_and_throws() {
  MacContentCallback held; bool cancelled{}; int submits{}, results{}, drains{};
  MacCaptureHooks hooks; hooks.cancelled=[&] { return cancelled; }; hooks.drained=[&] { ++drains; };
  mac_capture_with_native(1,[&](auto result) { HDRSHOT_CHECK(!result); ++results; }, hooks,
      {[&](auto, auto callback) { held=std::move(callback); }});
  cancelled=true;
  held(Result<MacImageSubmit,Error>::success([&](auto) { ++submits; }));
  HDRSHOT_CHECK(submits==0 && results==1 && drains==1);
  cancelled=false; hooks.stage=[](auto,auto) { throw std::runtime_error("diagnostics"); };
  mac_capture_with_native(1,[&](auto result) { HDRSHOT_CHECK(!result); ++results; }, hooks,
      {[](auto,auto) { throw std::runtime_error("submit"); }});
  HDRSHOT_CHECK(results==2 && drains==2);
}
void progress_before_copy_failure() {
  auto space=CGColorSpaceCreateDeviceRGB(); std::array<unsigned char,4> bytes{1,2,3,255};
  auto data=CFDataCreate(nullptr,bytes.data(),4); auto provider=CGDataProviderCreateWithCFData(data);
  auto image=CGImageCreate(1,1,8,32,4,space,kCGImageAlphaLast,provider,nullptr,false,kCGRenderingIntentDefault);
  bool progress{}, failed{}, result_failed{}; int drained{};
  MacCaptureHooks hooks; hooks.image_received=[&] { progress=true; };
  hooks.failed=[&](const auto&) { HDRSHOT_CHECK(progress); failed=true; }; hooks.drained=[&] { ++drained; };
  mac_capture_with_native(1,[&](auto result) { result_failed = !result && progress && failed; }, hooks,
      {[&](auto,auto callback) { callback(Result<MacImageSubmit,Error>::success([&](auto cb) { cb(image,{}); })); }});
  HDRSHOT_CHECK(drained==1 && progress && failed && result_failed); CGImageRelease(image); CGDataProviderRelease(provider); CFRelease(data); CGColorSpaceRelease(space);
}
void port_single_slot_and_queued_cancel() {
  std::vector<std::function<void()>> tasks; int submits{}, deliveries{};
  MacCapturePort port({},[&](auto,auto,auto) { ++submits; },[&](auto task) { tasks.push_back(std::move(task)); });
  port.capture(request(),[&](auto) { ++deliveries; }); HDRSHOT_CHECK(port.busy());
  for(int i=0;i<100;++i) port.capture(request(),[&](auto result) { HDRSHOT_CHECK(!result); HDRSHOT_CHECK(result.error().safe_context.at("reason")=="capture_draining"); });
  HDRSHOT_CHECK(tasks.size()==2); port.cancel({1},{1}); tasks[0](); HDRSHOT_CHECK(port.busy()); tasks[1]();
  HDRSHOT_CHECK(!port.busy() && submits==0 && deliveries==1);
}
void production_adapter_port_drain() {
  Image image; std::vector<MacContentCallback> contents; std::vector<MacImageCallback> images;
  int deliveries{}; MacCapturePort* owner{};
  MacCapturePort port({},[&](auto id, auto done, auto hooks) {
    mac_capture_with_native(id,std::move(done),std::move(hooks),{[&](auto,auto callback) { contents.push_back(callback); }});
  },[](auto task) { task(); }); owner=&port;
  port.capture(request(),[&](auto result) { HDRSHOT_CHECK(result.has_value()); HDRSHOT_CHECK(owner->busy()); ++deliveries; });
  for(auto& cb:contents) cb(Result<MacImageSubmit,Error>::success([&](auto image_cb) { images.push_back(image_cb); }));
  images[0](image.image,{}); HDRSHOT_CHECK(port.busy()); images[1](image.image,{});
  HDRSHOT_CHECK(!port.busy() && deliveries==1);
  port.capture(request(),[&](auto) { ++deliveries; }); HDRSHOT_CHECK(port.busy()); port.shutdown();
}
void enumeration_cancel_and_frozen_edr() {
  std::vector<std::function<void()>> tasks;
  MacScreenCaptureKitAdapter::DisplayListCompletion held;
  int snapshots{}, delivered{}; bool edr_frozen{}; const auto gui = std::this_thread::get_id();
  MacNativeDisplayFunctions native{
      [&] { HDRSHOT_CHECK(std::this_thread::get_id()==gui); ++snapshots; return MacEdrRanges{{1,{2.0,4.0}}}; },
      [&](auto task) { tasks.push_back(std::move(task)); },
      [&](auto completion) { held=std::move(completion); }};
  auto catalog=std::make_unique<MacDisplayCatalogPort>(nullptr,[&](auto completion, auto cancelled, auto stage) {
    mac_enumerate_with_native(std::move(completion),std::move(cancelled),std::move(stage),native);
  });
  catalog->snapshot_displays({{1},{2}},[&](auto) { ++delivered; });
  HDRSHOT_CHECK(snapshots==1 && tasks.size()==1); tasks[0](); catalog.reset();
  std::thread late([&] { held(Result<std::vector<MacDisplayInfo>,Error>::success({MacDisplayInfo{1,0,0,1,1,1,1,1}})); }); late.join();
  HDRSHOT_CHECK(delivered==0 && snapshots==1);
  catalog=std::make_unique<MacDisplayCatalogPort>(nullptr,[&](auto completion,auto cancelled,auto stage) {
    mac_enumerate_with_native(std::move(completion),std::move(cancelled),std::move(stage),native);
  });
  catalog->snapshot_displays({{3},{4}},[&](auto result) { ++delivered; edr_frozen=result && result.value().displays[0].dynamic_range==DisplayDynamicRange::hdr; });
  tasks.back()(); held(Result<std::vector<MacDisplayInfo>,Error>::success({MacDisplayInfo{1,0,0,1,1,1,1,1}}));
  HDRSHOT_CHECK(delivered==1 && snapshots==2 && edr_frozen);
}
void progress_registered_before_slow_diagnostics() {
  Image image; CaptureSteadyTime now{}; const CaptureOperationKey key{{1},{1},1};
  auto watchdog=std::make_shared<CaptureProgressWatchdog>(key,[&] { return now; }); watchdog->start({{1}});
  MacCaptureHooks hooks; int calls{};
  hooks.image_received=[&] { watchdog->receive({key,{1},CaptureNativeEventKind::image_received,{}}); };
  hooks.cancelled=[&] { return watchdog->stop_requested(); };
  hooks.stage=[&](const char* api,const char* stage) {
    if (std::string(api)=="captureImageWithFilter" && std::string(stage)=="callback_received") {
      now=CaptureSteadyTime{}+std::chrono::seconds(10); watchdog->check_deadline(1);
    }
  };
  mac_capture_with_native(1,[&](auto result) { if(result) ++calls; },hooks,
      {[&](auto,auto callback) { callback(Result<MacImageSubmit,Error>::success([&](auto cb) { cb(image.image,{}); })); }});
  HDRSHOT_CHECK(calls==1 && !watchdog->stop_requested());
}
class OneCatalog final : public DisplayCatalogPort {
 public: void snapshot_displays(const SnapshotDisplaysRequest&,Completion completion) override {
   completion(Result<DisplaySnapshotSet,Error>::success({1,{{{1},{0,0,1,1},1,{1,1}}}}));
 }
};
class NoPresenter final : public PreviewPresenterPort {
 public: int calls{}; void present(const PresentPreviewRequest&,Completion) override { ++calls; }
 void cancel(SessionId,OperationId) override {}
};
void blocked_native_submit_keeps_gui_alive() {
  QtCaptureRuntime owner; auto runtime=owner.handle(); std::mutex mutex; std::condition_variable cv;
  bool release{}; std::atomic_bool entered{}, returned{}; std::thread::id submit_thread;
  auto capture=std::make_shared<MacCapturePort>(nullptr,[&](auto id,auto done,auto hooks) {
    mac_capture_with_native(id,std::move(done),std::move(hooks),{[&](auto,auto callback) {
      { std::unique_lock lock(mutex); submit_thread=std::this_thread::get_id(); entered=true; cv.wait(lock,[&] { return release; }); }
      callback(Result<MacImageSubmit,Error>::failure(Error{ErrorCode::capture_failed,"held",Retryability::same_input,{}}));
      returned=true;
    }});
  });
  auto presenter=std::make_shared<NoPresenter>();
  auto session=std::make_unique<CaptureSession>(std::make_shared<OneCatalog>(),capture,presenter,nullptr,nullptr,runtime);
  int finished{}, heartbeat{}; bool timeout{}; QEventLoop loop;
  QTimer beats; QObject::connect(&beats,&QTimer::timeout,&loop,[&] { ++heartbeat; }); beats.start(10);
  QTimer::singleShot(4000,&loop,&QEventLoop::quit);
  session->begin({{1},{1},{1},{{1}},{}},[&](auto result) {
    ++finished; timeout=!result && result.error().safe_context.at("reason")=="capture_progress_timeout"; loop.quit();
  });
  loop.exec();
  const bool busy=capture->busy(); int rejections{};
  for(int i=0;i<100;++i) capture->capture({{2},{1},1,{{1}}},[&](auto result) { if(!result) ++rejections; });
  session.reset(); owner.shutdown();
  { std::lock_guard lock(mutex); release=true; } cv.notify_all();
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(capture->busy() && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
  HDRSHOT_CHECK(finished==1 && timeout && heartbeat>20 && entered && busy && rejections==100);
  HDRSHOT_CHECK(submit_thread!=std::this_thread::get_id()); HDRSHOT_CHECK(returned && !capture->busy() && presenter->calls==0);
}
void native_presenter_teardown_stays_on_gui() {
  QtCaptureRuntime runtime_owner; auto runtime=runtime_owner.handle();
  std::mutex mutex; std::condition_variable cv; bool entered{}, release{}; std::atomic_bool returned{};
  auto native=std::make_shared<MacPreviewPresenterPort>([&](void*, const MacMetalOverlayRequest&) {
    std::unique_lock lock(mutex); entered=true; cv.notify_all(); cv.wait(lock,[&] { return release; }); returned=true;
    return Result<MacMetalPresentReceipt,Error>::success({});
  },reinterpret_cast<void*>(1),1.0);
  auto fanout=std::make_unique<MultiDisplayPreviewPresenterPort>(std::vector<DisplayPreviewEndpoint>{{{1},native}},runtime);
  auto frozen=std::make_shared<FrozenDesktop>(); frozen->frame_id={1}; frozen->display_generation=1;
  CanonicalFrameSegment segment; segment.display_id={1}; segment.pixel_format=PixelFormat::rgba16_float;
  segment.encoding={ColorPrimaries::display_p3,TransferFunction::extended_srgb,AlphaMode::opaque,0};
  segment.rgba_half={0,0,0,0x3c00}; frozen->canonical_segments.push_back(std::move(segment));
  PresentPreviewRequest request; request.operation_id={1}; request.model.session_id={1}; request.model.frozen_desktop=frozen;
  request.model.display_generation=1; int completed{};
  fanout->present(request,[&](auto) { ++completed; });
  { std::unique_lock lock(mutex); HDRSHOT_CHECK(cv.wait_for(lock,std::chrono::seconds(2),[&] { return entered; })); }
  runtime_owner.shutdown(); fanout.reset();
  { std::lock_guard lock(mutex); release=true; } cv.notify_all();
  native.reset(); // production destructor joins worker on GUI; rejected post only drops pure fanout data
  HDRSHOT_CHECK(returned && completed==0);
}

void permission_and_enqueue_failure_settle_once() {
  int submits{}, results{}, drains{};
  MacCaptureHooks denied; denied.try_begin_step=[](auto) { return false; }; denied.drained=[&] { ++drains; };
  mac_capture_with_native(1,[&](auto r) { if(!r) ++results; },denied,{[&](auto,auto) { ++submits; }});
  HDRSHOT_CHECK(submits==0 && results==1 && drains==1);
  MacCapturePort enqueue_failure({},[&](auto,auto,auto) { ++submits; },[](auto) { throw std::runtime_error("enqueue"); });
  int failed{}; enqueue_failure.capture(request(),[&](auto r) { if(!r) ++failed; });
  HDRSHOT_CHECK(failed==1 && !enqueue_failure.busy() && submits==0);
}
void second_screen_error_while_first_held() {
  std::vector<MacContentCallback> callbacks; int deliveries{}; bool actual_error{};
  MacCapturePort port({},[&](auto id,auto done,auto hooks) {
    mac_capture_with_native(id,std::move(done),std::move(hooks),{[&](auto,auto callback) { callbacks.push_back(callback); }});
  },[](auto task) { task(); });
  port.capture(request(),[&](auto result) { ++deliveries; actual_error=!result && result.error().safe_context.at("nativeCode")=="-77"; });
  callbacks[1](Result<MacImageSubmit,Error>::failure({ErrorCode::capture_failed,"native",Retryability::same_input,{{"nativeCode","-77"}}}));
  HDRSHOT_CHECK(deliveries==1 && actual_error && port.busy());
  port.cancel({1},{1}); callbacks[0](Result<MacImageSubmit,Error>::failure({ErrorCode::operation_cancelled,"native",Retryability::never,{}}));
  HDRSHOT_CHECK(deliveries==1 && !port.busy());
}

void port_outer_call_ticket_survives_adapter_drain() {
  bool held{}; int deliveries{}; MacCapturePort* owner{};
  MacCapturePort port({},[&](auto id,auto completion,auto hooks) {
    mac_capture_with_native(id,std::move(completion),std::move(hooks),{[](auto,auto cb) {
      cb(Result<MacImageSubmit,Error>::failure({ErrorCode::capture_failed,"native",Retryability::same_input,{}}));
    }});
    held=owner->busy(); // adapter drained, but the queued outer native task has not returned
  },[](auto task) { task(); }); owner=&port;
  port.capture({{1},{1},1,{{1}}},[&](auto) { ++deliveries; });
  HDRSHOT_CHECK(held && deliveries==1 && !port.busy());
}

void enumeration_submit_cannot_block_gui_or_shutdown() {
  std::mutex mutex; std::condition_variable cv; bool release{};
  std::atomic_bool entered{}, returned{}; int delivered{}, snapshots{}, beats{};
  const auto gui=std::this_thread::get_id(); std::thread::id submitted;
  MacNativeDisplayFunctions native{
      [&] { ++snapshots; return MacEdrRanges{}; },
      [](std::function<void()> task) { dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{ task(); }); },
      [&](auto done) {
        { std::unique_lock lock(mutex); submitted=std::this_thread::get_id(); entered=true; cv.wait(lock,[&] { return release; }); }
        done(Result<std::vector<MacDisplayInfo>,Error>::success({})); returned=true;
      }};
  auto catalog=std::make_unique<MacDisplayCatalogPort>(nullptr,[&](auto completion,auto cancelled,auto stage) {
    mac_enumerate_with_native(std::move(completion),std::move(cancelled),std::move(stage),native);
  });
  catalog->snapshot_displays({{1},{1}},[&](auto) { ++delivered; });
  QEventLoop loop; QTimer pulse; QObject::connect(&pulse,&QTimer::timeout,&loop,[&] { ++beats; }); pulse.start(5);
  QTimer::singleShot(50,&loop,[&] { catalog.reset(); loop.quit(); }); loop.exec();
  { std::lock_guard lock(mutex); release=true; } cv.notify_all();
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(!returned && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
  HDRSHOT_CHECK(entered && returned && beats>0 && snapshots==1 && delivered==0 && submitted!=gui);
}

}
int main(int argc, char** argv) { QCoreApplication app(argc, argv); return test::run({
  {"enumeration submit GUI shutdown",enumeration_submit_cannot_block_gui_or_shutdown},
  {"port outer task ticket",port_outer_call_ticket_survives_adapter_drain},
  {"permission and enqueue failure",permission_and_enqueue_failure_settle_once},{"second screen early error",second_screen_error_while_first_held},
  {"enumeration cancellation",enumeration_cancel_and_frozen_edr},{"progress before log IO",progress_registered_before_slow_diagnostics},
  {"blocked submit GUI",blocked_native_submit_keeps_gui_alive},{"native presenter GUI teardown",native_presenter_teardown_stays_on_gui},{"nested tickets",synchronous_nested_tickets}, {"cancel and throws",cancelled_content_and_throws},
  {"progress before copy",progress_before_copy_failure},{"single slot",port_single_slot_and_queued_cancel},
  {"production adapter drain",production_adapter_port_drain}}); }
