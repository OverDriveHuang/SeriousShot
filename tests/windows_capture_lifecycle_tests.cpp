#include "platform/windows/windows_capture.hpp"
#include "platform/windows/windows_capture_native_calls.hpp"
#include "platform/windows/windows_capture_operations.hpp"
#include "platform/windows/windows_capture_preparation.hpp"
#include "test_support.hpp"
#include <iostream>
#include <thread>
#include <future>
#include <chrono>
using namespace hdrshot;
namespace {
struct Observer final:CaptureProgressObserver {
  int images{},failures{};bool stop{};
  void native_event(CaptureNativeEvent event)noexcept override{if(event.kind==CaptureNativeEventKind::image_received)++images;else ++failures;}
  bool stop_requested()const noexcept override{return stop;}
};
auto displays(){auto value=std::make_shared<std::vector<WindowsDisplayInfo>>();WindowsDisplayInfo display;display.snapshot.id=DisplayId{1};display.snapshot.capture_size_px={1,1};display.advanced_color_mode=1;value->push_back(display);return value;}
CaptureBatchRequest request(std::shared_ptr<Observer> observer={}){return {SessionId{1},OperationId{1},1,{DisplayId{1}},PixelFormat::rgba16_float,true,observer};}
void queued_cancel_and_bounded_owner(){
  std::vector<std::function<void()>> tasks;int calls=0,results=0;
  auto port=std::make_unique<WindowsCapturePort>(displays());
  port->set_test_seams([&](auto task){tasks.push_back(std::move(task));},[&](const auto&,const auto*,const auto&){++calls;return Result<WindowsRawCapture,Error>::failure(windows_capture_error("synthetic"));});
  port->capture(request(),[&](auto){++results;});HDRSHOT_CHECK(port->busy() && tasks.size()==1);
  port->cancel(SessionId{1},OperationId{1});
  for(int i=0;i<100;++i)port->capture(request(),[](auto result){HDRSHOT_CHECK(!result);});
  HDRSHOT_CHECK(tasks.size()==1);tasks.front()();tasks.clear();HDRSHOT_CHECK(!port->busy() && calls==0 && results==0);
  port->capture(request(),[&](auto){++results;});HDRSHOT_CHECK(tasks.size()==1);tasks.front()();tasks.clear();HDRSHOT_CHECK(calls==1 && results==1 && !port->busy());
  port->capture(request(),[](auto){});port.reset();tasks.front()();HDRSHOT_CHECK(calls==1);
}
void work_requires_every_boundary(){
  int drained=0;auto work=std::make_shared<WindowsCaptureWork>([&]{++drained;});
  work->registration_open();HDRSHOT_CHECK(work->enter_callback());work->result_finished();work->resources_released();work->outer_returned();HDRSHOT_CHECK(drained==0);
  work->registration_closed();HDRSHOT_CHECK(drained==0 && !work->enter_callback());work->leave_callback();HDRSHOT_CHECK(drained==1);
  work->resources_released();HDRSHOT_CHECK(drained==1);
}
void production_receive_validate_copy(){
  int drained=0,images=0,errors=0,copies=0,unmaps=0;bool stopped=false;
  auto work=std::make_shared<WindowsCaptureWork>([&]{++drained;});work->registration_open();
  WindowsCaptureFrameState<int> state;state.accepted=[&]{++images;};state.failed=[&](const Error&){++errors;};
  WindowsCaptureCallHooks calls;calls.failed=[&](const Error& error){state.report_failure(error);};
  auto validate=[](int value){windows_validate_capture_extent(10,10,1,1,1,1,1,1);return value;};
  windows_receive_capture_frame(work,state,calls,[]{return 0;},validate);HDRSHOT_CHECK(images==0);
  windows_receive_capture_frame(work,state,calls,[]{return 7;},validate);HDRSHOT_CHECK(images==1);
  windows_receive_capture_frame(work,state,calls,[]()->int{throw winrt::hresult_error(E_ACCESSDENIED);},validate);HDRSHOT_CHECK(errors==0);
  auto value=state.wait(nullptr,std::nullopt);HDRSHOT_CHECK(value && value.value()==7);
  windows_copy_capture_rows(3,[&]{return stopped;},[&]{HDRSHOT_CHECK(images==1);},[&](auto){++copies;},[&]{++unmaps;});
  HDRSHOT_CHECK(copies==3 && unmaps==1);
  try{windows_copy_capture_rows(3,[&]{return stopped;},[&]{stopped=true;},[&](auto){++copies;},[&]{++unmaps;});HDRSHOT_CHECK(false);}catch(const WindowsCaptureCallFailure& failure){HDRSHOT_CHECK(failure.error.code==ErrorCode::operation_cancelled);}
  HDRSHOT_CHECK(copies==3 && unmaps==2);
  stopped=false;
  try{windows_copy_capture_rows(3,[&]{return stopped;},[]{},[](auto){throw winrt::hresult_error(E_ACCESSDENIED);},[&]{++unmaps;});HDRSHOT_CHECK(false);}catch(const WindowsCaptureCallFailure&){}
  HDRSHOT_CHECK(unmaps==3);
  work->registration_closed();work->resources_released();work->result_finished();HDRSHOT_CHECK(drained==0);work->outer_returned();HDRSHOT_CHECK(drained==1);
}
void validation_failure_and_native_provenance(){
  WindowsCaptureFrameState<int> state;int errors=0;Error recorded{};state.failed=[&](const Error& error){++errors;recorded=error;};
  WindowsCaptureCallHooks calls;calls.failed=[&](const Error& error){state.report_failure(error);};
  windows_receive_capture_frame({},state,calls,[]{return 1;},[](int value){windows_validate_capture_extent(10,10,0,1,1,1,1,1);return value;});
  HDRSHOT_CHECK(errors==1 && !state.wait(nullptr,1));
  for(const auto* api:{"CreateForMonitor","ID3D11DeviceContext.Map"}){
    int immediate=0;WindowsCaptureCallHooks operation;operation.failed=[&](const Error& error){++immediate;HDRSHOT_CHECK(error.safe_context.at("api")==api);HDRSHOT_CHECK(error.safe_context.at("hresult")=="2147942405");};
    WindowsCaptureCallScope scope(&operation);
    try{windows_capture_call(api,[]{throw winrt::hresult_error(E_ACCESSDENIED);});HDRSHOT_CHECK(false);}catch(const WindowsCaptureCallFailure&){}
    HDRSHOT_CHECK(immediate==1);
  }
}
void preparation_cancel_and_com_tail(){
  std::vector<std::function<void()>> tasks;int enumeration=0,permission=0,results=0;
  auto native=std::make_shared<WindowsCaptureNativeCalls>();WindowsCapturePreparationOwner* observed=nullptr;int cleanups=0;
  native->boundary=[&](const char* api,bool enter){if(std::string_view(api)=="CoUninitialize" && enter){++cleanups;HDRSHOT_CHECK(observed->busy());HDRSHOT_CHECK(results==0);HDRSHOT_CHECK(!observed->prepare(SessionId{2},[](auto){}));}};
  WindowsCapturePreparationOwner owner({},[&](auto task){tasks.push_back(std::move(task));},[&]{++enumeration;return Result<std::vector<WindowsDisplayInfo>,Error>::success(*displays());},[&]{++permission;return WindowsBorderlessAccess{};},native);observed=&owner;
  HDRSHOT_CHECK(owner.prepare(SessionId{1},[&](auto result){HDRSHOT_CHECK(result.has_value());++results;}));tasks.front()();tasks.clear();HDRSHOT_CHECK(cleanups==1 && results==1 && !owner.busy());
  HDRSHOT_CHECK(owner.prepare(SessionId{2},[&](auto){++results;}));owner.cancel(SessionId{2});tasks.front()();tasks.clear();HDRSHOT_CHECK(enumeration==1 && permission==1 && results==1 && !owner.busy());
  native->boundary=[](const char* api,bool enter){if(std::string_view(api)=="CoInitializeEx" && enter)throw winrt::hresult_error(static_cast<HRESULT>(0x80080005));};
  HDRSHOT_CHECK(owner.prepare(SessionId{3},[&](auto result){HDRSHOT_CHECK(!result && result.error().safe_context.at("hresult")=="2148007941");++results;}));tasks.front()();tasks.clear();HDRSHOT_CHECK(results==2 && enumeration==1 && !owner.busy());
}
void submit_failure_has_no_phantom_work(){
  WindowsCapturePort port(displays());int done=0;
  port.set_test_seams([](auto){throw std::runtime_error("synthetic submit rejection");});
  port.capture(request(),[&](auto result){HDRSHOT_CHECK(!result);++done;});HDRSHOT_CHECK(done==1 && !port.busy());
}
void progress_is_published_before_waiter_can_copy(){
  WindowsCaptureFrameState<int> state;std::promise<void> entered,release;
  auto released=release.get_future().share();state.accepted=[&]{entered.set_value();released.wait();};
  auto callback=std::async(std::launch::async,[&]{state.receive([]{return 1;});});
  entered.get_future().wait();
  auto worker=std::async(std::launch::async,[&]{return state.wait(nullptr,std::nullopt);});
  const bool blocked=worker.wait_for(std::chrono::milliseconds(100))==std::future_status::timeout;
  release.set_value();callback.get();HDRSHOT_CHECK(worker.get().has_value() && blocked);
}
void cancelled_frame_release_and_outer_callback_tail(){
  int alive=0,drained=0;bool tail_destroyed=false,drain_saw_tail=false;
  struct Resource {int& alive;std::function<void()> releasing;explicit Resource(int& count):alive(count){++alive;}~Resource(){--alive;if(releasing)releasing();}};
  auto state=std::make_shared<WindowsCaptureFrameState<std::shared_ptr<Resource>>>();
  auto work=std::make_shared<WindowsCaptureWork>([&]{++drained;drain_saw_tail=tail_destroyed && alive==0;});work->registration_open();
  {
    WindowsCallbackTicket ticket(work); // Matches the native delegate's outer scope.
    struct Tail {bool& destroyed;~Tail(){destroyed=true;}} tail{tail_destroyed};
    WindowsCaptureCallHooks calls;calls.failed=[state](const Error& error){state->report_failure(error);};
    windows_receive_capture_frame(ticket,*state,calls,[&]{
      auto frame=std::make_shared<Resource>(alive);
      // Reentrant close proves destruction occurs outside the FrameState lock.
      frame->releasing=[weak=std::weak_ptr{state}]{if(auto retained=weak.lock())retained->close();};
      return frame;
    },[](auto frame){return frame;});
    HDRSHOT_CHECK(alive==1);
    std::atomic_bool cancelled=true;HDRSHOT_CHECK(!state->wait(&cancelled,std::nullopt));state->close();
    HDRSHOT_CHECK(alive==0);state.reset();
    work->registration_closed();work->resources_released();work->result_finished();work->outer_returned();
    HDRSHOT_CHECK(drained==0 && !tail_destroyed);
  }
  HDRSHOT_CHECK(drained==1 && drain_saw_tail);
}
}
int main(){try{queued_cancel_and_bounded_owner();work_requires_every_boundary();production_receive_validate_copy();validation_failure_and_native_provenance();preparation_cancel_and_com_tail();submit_failure_has_no_phantom_work();progress_is_published_before_waiter_can_copy();cancelled_frame_release_and_outer_callback_tail();std::cout<<"Windows capture lifecycle / production adapter boundaries: 8 passed\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
