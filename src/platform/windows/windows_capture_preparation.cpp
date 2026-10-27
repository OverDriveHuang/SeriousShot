#include "platform/windows/windows_capture_preparation.hpp"
#include "platform/windows/windows_capture_operations.hpp"
#include "application/capture_inflight_diagnostics.hpp"
#include "platform/windows/windows_capture_native_calls.hpp"
namespace hdrshot {
struct WindowsCapturePreparationOwner::State {
  mutable std::mutex mutex;bool active{},closed{};SessionId session{};Completion completion;
  std::shared_ptr<DiagnosticsPort> diagnostics;std::shared_ptr<CaptureInFlightDiagnostics> facts;
  WindowsNativeSubmit submit;Enumerate enumerate;Permission permission;
  std::shared_ptr<WindowsCaptureNativeCalls> native;
};
WindowsCapturePreparationOwner::WindowsCapturePreparationOwner(std::shared_ptr<DiagnosticsPort> diagnostics,WindowsNativeSubmit submit,Enumerate enumerate,Permission permission,std::shared_ptr<WindowsCaptureNativeCalls> native):state_(std::make_shared<State>()){
  state_->diagnostics=std::move(diagnostics);state_->submit=submit?std::move(submit):WindowsNativeSubmit{windows_submit_capture_work};state_->enumerate=std::move(enumerate);state_->permission=std::move(permission);
  state_->native=std::move(native);
}
WindowsCapturePreparationOwner::~WindowsCapturePreparationOwner(){shutdown();}
bool WindowsCapturePreparationOwner::busy()const{std::lock_guard lock(state_->mutex);return state_->active;}
void WindowsCapturePreparationOwner::cancel(SessionId session){Completion released;{std::lock_guard lock(state_->mutex);if(state_->session==session)released=std::move(state_->completion);}}
void WindowsCapturePreparationOwner::shutdown(){Completion released;{std::lock_guard lock(state_->mutex);state_->closed=true;released=std::move(state_->completion);}}
void WindowsCapturePreparationOwner::diagnostic_snapshot(){std::shared_ptr<CaptureInFlightDiagnostics> facts;{std::lock_guard lock(state_->mutex);if(state_->active)facts=state_->facts;}if(facts)facts->snapshot();}
bool WindowsCapturePreparationOwner::prepare(SessionId session,Completion completion){
  const auto state=state_;
  {std::lock_guard lock(state->mutex);if(state->closed||state->active)return false;state->active=true;state->session=session;state->completion=std::move(completion);state->facts=std::make_shared<CaptureInFlightDiagnostics>(CaptureOperationKey{session,OperationId{1},0},state->diagnostics);}
  const auto deliver=[state](Result<WindowsPreparedCapture,Error> result){Completion completion;{std::lock_guard lock(state->mutex);completion=std::move(state->completion);}if(completion){try{completion(std::move(result));}catch(...){}}};
  try{state->submit([state,deliver]{
    struct Tail{std::shared_ptr<State> state;~Tail(){std::lock_guard lock(state->mutex);state->active=false;}} tail{state};
    const auto alive=[&]{std::lock_guard lock(state->mutex);return !state->closed && bool(state->completion);};
    if(!alive())return;
    try{
      std::optional<WindowsPreparedCapture> prepared;
      {
      WindowsCaptureCallHooks calls;
      calls.failed=[&](const Error& error){deliver(Result<WindowsPreparedCapture,Error>::failure(error));};
      calls.boundary=[&](const char* api,bool enter){state->facts->stage({},api,enter?"call_enter":"call_return");if(state->native && state->native->boundary)state->native->boundary(api,enter);};
      WindowsCaptureCallScope scope(&calls);
      WindowsCaptureApartment apartment;
      if(FAILED(apartment.hr) && apartment.hr!=RPC_E_CHANGED_MODE)return;
      state->facts->stage({},"windows_enumerate_displays","call_enter");
      auto displays=state->enumerate?state->enumerate():windows_enumerate_displays(state->diagnostics.get());
      if(!displays){deliver(Result<WindowsPreparedCapture,Error>::failure(displays.error()));return;}
      if(!alive())return;
      state->facts->stage({},"GraphicsCaptureAccess.RequestAccessAsync","permission_pending");
      auto permission=state->permission?state->permission():windows_prepare_borderless_access();
      if(!alive())return;
      prepared=WindowsPreparedCapture{std::make_shared<const std::vector<WindowsDisplayInfo>>(std::move(displays.value())),std::move(permission)};
      } // Native locals and CoUninitialize finish before success can start capture.
      if(alive())deliver(Result<WindowsPreparedCapture,Error>::success(std::move(*prepared)));
    }catch(const WindowsCaptureCallFailure& failure){deliver(Result<WindowsPreparedCapture,Error>::failure(failure.error));}
    catch(...){deliver(Result<WindowsPreparedCapture,Error>::failure(windows_capture_error("capture_preparation_failed")));}
  });}catch(const WindowsCaptureCallFailure& failure){deliver(Result<WindowsPreparedCapture,Error>::failure(failure.error));std::lock_guard lock(state->mutex);state->active=false;}
  catch(...){deliver(Result<WindowsPreparedCapture,Error>::failure(windows_capture_error("capture_submit_failed")));std::lock_guard lock(state->mutex);state->active=false;}
  return true;
}
}
