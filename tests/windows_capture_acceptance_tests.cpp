#include "application/capture_progress_watchdog.hpp"
#include "application/capture_inflight_diagnostics.hpp"
#include "platform/windows/windows_capture.hpp"
#include "platform/windows/windows_capture_native_calls.hpp"
#include "platform/windows/windows_capture_operations.hpp"
#include "platform/windows/windows_capture_preparation.hpp"
#include "platform/qt/qt_capture_runtime.hpp"
#include "platform/qt/qt_session_diagnostics_port.hpp"
#include "ui/qt/capture_error_message.hpp"
#include "test_support.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <future>
#include <thread>
#include <string_view>
using namespace hdrshot;
using namespace std::chrono_literals;
namespace {
struct Observer final : CaptureProgressObserver {
  std::atomic_int milliseconds{};
  CaptureOperationKey key{{71},{72},73};
  CaptureProgressWatchdog watchdog{key,[this]{return CaptureSteadyTime{}+std::chrono::milliseconds(milliseconds.load());}};
  void native_event(CaptureNativeEvent event) noexcept override {watchdog.receive(event);}
  bool stop_requested() const noexcept override {return watchdog.stop_requested();}
  void image(DisplayId display) {native_event({key,display,CaptureNativeEventKind::image_received,{}});}
};
auto displays(int count=1) {
  auto values=std::make_shared<std::vector<WindowsDisplayInfo>>();
  for(int i=1;i<=count;++i){WindowsDisplayInfo d;d.snapshot.id={static_cast<std::uint64_t>(i)};d.snapshot.capture_size_px={1,1};d.advanced_color_mode=1;values->push_back(d);}
  return values;
}
CaptureBatchRequest request(std::shared_ptr<Observer> observer={},int count=1){
  CaptureBatchRequest r{{71},{72},73,{},PixelFormat::rgba16_float,true,observer};
  for(int i=1;i<=count;++i)r.targets.push_back({static_cast<std::uint64_t>(i)});return r;
}
QByteArray read(const std::string& path){QFile f(QString::fromStdString(path));HDRSHOT_CHECK(f.open(QIODevice::ReadOnly));return f.readAll();}
std::shared_ptr<QtSessionDiagnosticsPort> sink(const char* label){
  auto directory=QDir::tempPath()+"/independent-"+label+"-"+QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz");
  auto result=std::make_shared<QtSessionDiagnosticsPort>(directory.toStdString());
  HDRSHOT_CHECK(result->start("T62-independent","Windows").has_value());
  std::cout<<"sink="<<result->exact_path()<<'\n';return result;
}
void sliding_windows_and_native_copy_boundary(){
  for(int count:{3,4}){
    auto observer=std::make_shared<Observer>();auto r=request(observer,count);observer->watchdog.start(r.targets);
    for(int i=1;i<=count;++i){
      observer->milliseconds=i*1500;
      WindowsCaptureFrameState<int> state;state.accepted=[&]{observer->image({static_cast<std::uint64_t>(i)});};
      WindowsCaptureCallHooks calls;calls.failed=[&](const Error& e){state.report_failure(e);};
      windows_receive_capture_frame({},state,calls,[]{return 1;},[](int f){windows_validate_capture_extent(10,10,1,1,1,1,1,1);return f;});
      HDRSHOT_CHECK(state.wait(nullptr,std::nullopt).has_value());
    }
    HDRSHOT_CHECK(observer->watchdog.snapshot().phase==CaptureProgressWatchdog::Phase::processing);
    observer->milliseconds=20000;HDRSHOT_CHECK(!observer->watchdog.check_deadline(1).snapshot.terminal());
    int rows=0;windows_copy_capture_rows(3,[&]{return observer->stop_requested();},[&]{HDRSHOT_CHECK(!observer->watchdog.snapshot().deadline);},[&](auto){++rows;},[]{});
    HDRSHOT_CHECK(rows==3);HDRSHOT_CHECK(observer->watchdog.finish({}).snapshot.phase==CaptureProgressWatchdog::Phase::succeeded);
  }
  Observer held;held.watchdog.start({{1},{2},{3}});held.milliseconds=1500;held.image({1});held.milliseconds=3000;held.image({2});
  auto epoch=held.watchdog.snapshot().epoch;held.milliseconds=4999;HDRSHOT_CHECK(!held.watchdog.check_deadline(epoch).snapshot.terminal());held.milliseconds=5000;
  HDRSHOT_CHECK(held.watchdog.check_deadline(epoch).snapshot.phase==CaptureProgressWatchdog::Phase::timed_out);
}
void callback_arbitration_cancel_and_tickets(){
  auto observer=std::make_shared<Observer>();observer->watchdog.start({{1}});int images=0,drained=0,fetches=0,rows=0,maps=0,unmaps=0;
  auto work=std::make_shared<WindowsCaptureWork>([&]{++drained;});work->registration_open();
  WindowsCaptureFrameState<int> state;state.accepted=[&]{++images;observer->image({1});};WindowsCaptureCallHooks calls;
  observer->milliseconds=1999;
  windows_receive_capture_frame(work,state,calls,[&]{++fetches;return 4;},[](int f){return f;});
  observer->milliseconds=6000;HDRSHOT_CHECK(!observer->watchdog.check_deadline(1).snapshot.terminal());
  windows_receive_capture_frame(work,state,calls,[&]{++fetches;return 8;},[](int f){return f;});HDRSHOT_CHECK(images==1 && fetches==1);
  HDRSHOT_CHECK(state.wait(nullptr,std::nullopt).value()==4);
  bool cancelled=false;
  try{windows_copy_capture_rows(5,[&]{return cancelled;},[&]{++maps;},[&](auto row){++rows;if(row==0)cancelled=true;},[&]{++unmaps;});}catch(const WindowsCaptureCallFailure& e){HDRSHOT_CHECK(e.error.code==ErrorCode::operation_cancelled);}
  HDRSHOT_CHECK(rows==1 && maps==1 && unmaps==1);
  try{windows_copy_capture_rows(5,[&]{return cancelled;},[&]{++maps;},[&](auto){++rows;},[&]{++unmaps;});}catch(const WindowsCaptureCallFailure&){}
  HDRSHOT_CHECK(rows==1 && maps==1 && unmaps==1);
  HDRSHOT_CHECK(work->enter_callback());work->resources_released();work->result_finished();work->registration_closed();work->outer_returned();HDRSHOT_CHECK(drained==0);
  work->leave_callback();HDRSHOT_CHECK(drained==1);
  windows_receive_capture_frame(work,state,calls,[&]{++fetches;return 9;},[](int f){return f;});HDRSHOT_CHECK(fetches==1 && drained==1);
}
void cancelled_frame_cannot_outlive_physical_drain(){
  bool native_alive=false;int drained=0;
  struct Resource {bool* alive;explicit Resource(bool* value):alive(value){*alive=true;}~Resource(){*alive=false;}};
  auto work=std::make_shared<WindowsCaptureWork>([&]{++drained;});work->registration_open();
  auto state=std::make_shared<WindowsCaptureFrameState<std::shared_ptr<Resource>>>();
  WindowsCaptureCallHooks callback_calls;callback_calls.failed=[state](const Error& error){state->report_failure(error);};
  windows_receive_capture_frame(work,*state,callback_calls,[&]{return std::make_shared<Resource>(&native_alive);},[](auto f){return f;});
  std::atomic_bool cancelled=true;HDRSHOT_CHECK(!state->wait(&cancelled,std::nullopt));state->close();
  // Match worker teardown while the outer callback still owns callback_calls.
  state.reset();work->registration_closed();work->resources_released();work->result_finished();work->outer_returned();
  std::cout<<"cancelled_frame_tail drained="<<drained<<" native_alive="<<native_alive<<'\n';
  const bool released_before_drain=!native_alive || drained==0;
  callback_calls.failed={};HDRSHOT_CHECK(!native_alive);
  HDRSHOT_CHECK(released_before_drain);
}
void production_adapter_error_before_other_screen_and_com_tail(){
  auto log=sink("native");auto observer=std::make_shared<Observer>();observer->watchdog.start({{1},{2}});
  WindowsCapturePort port(displays(2),1.0,log);std::vector<std::function<void()>> tasks;int delivered=0,rejected=0,tail=0,entered=0,error_tail=0;bool tail_ok=true;std::optional<Error> result;
  auto native=std::make_shared<WindowsCaptureNativeCalls>();
  native->boundary=[&](const char* api,bool enter){
    if(enter && std::string_view(api)=="GraphicsCaptureItem.GetActivationFactory"){++entered;throw winrt::hresult_error(E_ACCESSDENIED);}
    if(enter && std::string_view(api)=="CoUninitialize"){
      ++tail;tail_ok=tail_ok && port.busy();
      if(entered>0){++error_tail;tail_ok=tail_ok && observer->stop_requested() && delivered>=1;}
      for(int i=0;i<100;++i)port.capture(request(observer,2),[&](auto value){HDRSHOT_CHECK(!value && value.error().safe_context.at("reason")=="capture_draining");++rejected;});
    }
  };
  port.set_test_seams([&](auto task){tasks.push_back(std::move(task));},{},native);
  port.capture(request(observer,2),[&](auto value){++delivered;HDRSHOT_CHECK(!value);result=value.error();record_diagnostic_failure(log.get(),"capture",value.error());});
  HDRSHOT_CHECK(tasks.size()==2);tasks[1]();
  std::cout<<"native_tail delivered="<<delivered<<" entered="<<entered<<" tail="<<tail<<" rejected="<<rejected<<" busy="<<port.busy()<<" tail_ok="<<tail_ok<<'\n';
  if(result)for(const auto& [key,value]:result->safe_context)std::cout<<"native_error "<<key<<'='<<value<<'\n';
  HDRSHOT_CHECK(delivered==1 && entered==1 && tail>=1 && error_tail>=1 && rejected>=100 && port.busy() && tail_ok);
  HDRSHOT_CHECK(result->safe_context.at("api")=="GraphicsCaptureItem.GetActivationFactory");HDRSHOT_CHECK(result->safe_context.at("hresult")=="2147942405");
  auto simple=read(log->exact_path());HDRSHOT_CHECK(simple.count("failure")==1 && simple.count("hresult=0x80070005")==1);HDRSHOT_CHECK(!simple.contains("call_enter"));
  std::cout<<"native_checkpoint before_hot_switch\n";
  log->set_detailed_logging(true);port.diagnostic_snapshot();HDRSHOT_CHECK(read(log->exact_path()).contains("inflight_snapshot"));
  std::cout<<"native_checkpoint before_old_drain\n";
  tasks[0]();HDRSHOT_CHECK(!port.busy() && delivered==1 && entered==1);
  std::cout<<"native_checkpoint before_manual_retry\n";
  tasks.clear();port.capture(request(),[&](auto value){HDRSHOT_CHECK(!value);++delivered;});HDRSHOT_CHECK(tasks.size()==1);tasks[0]();HDRSHOT_CHECK(!port.busy() && delivered==2);
  std::cout<<"native_checkpoint after_manual_retry\n";
}
void permission_wait_is_preflight_and_cancel_suppresses_delivery(){
  std::vector<std::function<void()>> tasks;int permission=0,delivered=0;WindowsCapturePreparationOwner* observed{};
  WindowsCapturePreparationOwner owner({},[&](auto task){tasks.push_back(std::move(task));},[]{return Result<std::vector<WindowsDisplayInfo>,Error>::success(*displays());},[&]{
    ++permission;HDRSHOT_CHECK(observed->busy());for(int i=0;i<100;++i)HDRSHOT_CHECK(!observed->prepare({99},[](auto){}));
    return WindowsBorderlessAccess{false,5,"bordered_fallback",windows_capture_error("optional_denied",E_ACCESSDENIED)};
  });observed=&owner;
  HDRSHOT_CHECK(owner.prepare({1},[&](auto result){HDRSHOT_CHECK(result && !result.value().borderless.allowed && result.value().borderless.native_code==5);++delivered;}));
  tasks[0]();HDRSHOT_CHECK(permission==1 && delivered==1 && !owner.busy());tasks.clear();
  HDRSHOT_CHECK(owner.prepare({2},[&](auto){++delivered;}));owner.cancel({2});tasks[0]();HDRSHOT_CHECK(permission==1 && delivered==1 && !owner.busy());
}
void default_threadpool_manual_batches_and_new_owner(){
  auto port=std::make_unique<WindowsCapturePort>(displays());std::atomic_int native_entries{};
  auto native=std::make_shared<WindowsCaptureNativeCalls>();
  native->boundary=[&](const char* api,bool enter){if(enter && std::string_view(api)=="GraphicsCaptureItem.GetActivationFactory"){
    ++native_entries;throw winrt::hresult_error(E_ACCESSDENIED);
  }};
  port->set_test_seams({}, {}, native);
  for(int run=1;run<=11;++run){
    if(run==11){port.reset();port=std::make_unique<WindowsCapturePort>(displays());port->set_test_seams({}, {}, native);}
    std::promise<Error> completed;auto future=completed.get_future();
    port->capture(request(),[&](auto result){if(result)completed.set_exception(std::make_exception_ptr(std::runtime_error("unexpected success")));else completed.set_value(result.error());});
    HDRSHOT_CHECK(future.wait_for(2s)==std::future_status::ready);
    HDRSHOT_CHECK(future.get().safe_context.at("hresult")=="2147942405");
    const auto until=std::chrono::steady_clock::now()+2s;
    while(port->busy() && std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(1ms);
    HDRSHOT_CHECK(!port->busy() && native_entries==run);
  }
  std::cout<<"default_threadpool completed_batches="<<native_entries.load()<<" recreated_owner_batches=1\n";
}
void gui_cli_wait_policy_and_shutdown_reference(){
  for(bool gui:{false,true}){std::vector<std::function<void()>> tasks;int calls=0;auto observer=gui?std::make_shared<Observer>():nullptr;
    WindowsCapturePort port(displays());port.set_test_seams([&](auto task){tasks.push_back(std::move(task));},[&](const auto&,const auto*,const WindowsCaptureHooks& hooks){
      ++calls;HDRSHOT_CHECK(gui?!hooks.frame_timeout:hooks.frame_timeout==5000);HDRSHOT_CHECK(bool(hooks.borderless)==gui);
      return Result<WindowsRawCapture,Error>::failure(windows_capture_error("controlled"));});
    port.capture(request(observer),[](auto){});tasks[0]();HDRSHOT_CHECK(calls==1 && !port.busy());
  }
  QtCaptureRuntime owner;auto runtime=owner.handle();const auto gui=std::this_thread::get_id();std::thread::id destroyed;int callback=0;
  struct UiRef{std::thread::id* destroyed;~UiRef(){*destroyed=std::this_thread::get_id();}};
  auto ui=std::make_shared<UiRef>(&destroyed);
  std::thread worker([runtime,ui,&callback]{HDRSHOT_CHECK(runtime->post([ui,&callback]{++callback;}));});worker.join();ui.reset();owner.shutdown();QCoreApplication::processEvents();
  HDRSHOT_CHECK(callback==0 && destroyed==gui);
  std::thread late([runtime]{HDRSHOT_CHECK(!runtime->post([]{}));});late.join();
}
void real_sink_hot_switch_native_errors_timeout_cleanup_and_privacy(){
  auto log=sink("diagnostics");CaptureInFlightDiagnostics facts({{1},{2},3},log,{{1},{2}});
  facts.stage({1},"CreateForMonitor","call_enter");HDRSHOT_CHECK(!read(log->exact_path()).contains("call_enter"));
  log->set_detailed_logging(true);facts.snapshot();
  for(auto api:{"CreateForMonitor","ID3D11DeviceContext.Map"}){
    WindowsCaptureCallHooks calls;calls.failed=[&](const Error& error){facts.failure({1},error,"native_failure");};WindowsCaptureCallScope scope(&calls);
    try{windows_capture_call(api,[]{throw winrt::hresult_error(E_ACCESSDENIED);});}catch(const WindowsCaptureCallFailure&){}
  }
  auto primary=windows_capture_error("original",E_ACCESSDENIED);WindowsCaptureCleanup cleanup;cleanup.attempt("GraphicsCaptureSession.Close",[]{throw winrt::hresult_error(E_FAIL);});
  auto final=cleanup.complete(Result<int,Error>::failure(primary));HDRSHOT_CHECK(final.error().safe_context.at("hresult")=="2147942405");facts.failure({1},cleanup.error(),"cleanup_failure");
  Observer timeout;timeout.watchdog.start({{1}});timeout.milliseconds=2000;auto error=timeout.watchdog.check_deadline(1).snapshot.error.value();HDRSHOT_CHECK(!error.safe_context.contains("hresult"));
  record_diagnostic_failure(log.get(),"capture",error);auto unsafe=primary;unsafe.safe_context["api"]="C:/private/secret";unsafe.safe_context["path"]="SECRET_PATH";unsafe.safe_context["windowTitle"]="SECRET_WINDOW";facts.failure({2},unsafe,"late_failure");
  auto text=read(log->exact_path());HDRSHOT_CHECK(text.contains("api=CreateForMonitor") && text.contains("api=ID3D11DeviceContext.Map") && text.contains("inflight_snapshot") && text.contains("elapsedMs="));
  HDRSHOT_CHECK(text.contains("hresult=0x80070005") && text.contains("hresult=0x80004005"));HDRSHOT_CHECK(!text.contains("SECRET") && !text.contains("private"));
  for(auto line:text.split('\n'))if(line.contains("capture_progress_timeout"))HDRSHOT_CHECK(!line.contains("hresult=") && !line.contains("nativeCode="));HDRSHOT_CHECK(!facts.write_failed());
  auto message=capture_error_message(error,QStringLiteral("允许屏幕截图"),"Ctrl+F9");HDRSHOT_CHECK(message.contains("Ctrl+F9") && message.contains(QStringLiteral("截图")));
}
}
int main(int argc,char** argv){std::cout<<std::unitbuf;std::cerr<<std::unitbuf;QCoreApplication app(argc,argv);return test::run({
 {"A02 A03 A05 A06 production receive sliding and post-copy",sliding_windows_and_native_copy_boundary},
 {"A04 A08 A09 callback cancellation and physical tickets",callback_arbitration_cancel_and_tickets},
 {"A09 cancelled frame resource cannot outlive physical drain",cancelled_frame_cannot_outlive_physical_drain},
 {"A07 A09 A11 actual adapter early error COM tail single final",production_adapter_error_before_other_screen_and_com_tail},
 {"A12 A15 preflight optional permission semantics",permission_wait_is_preflight_and_cancel_suppresses_delivery},
 {"A09 A15 default threadpool ten batches and recreated owner",default_threadpool_manual_batches_and_new_owner},
 {"A15 GUI no 5000 CLI finite mailbox UI final reference",gui_cli_wait_policy_and_shutdown_reference},
 {"A10 A11 A12 actual sink hot switch provenance and privacy",real_sink_hot_switch_native_errors_timeout_cleanup_and_privacy}});}
