#pragma once

#include "core/error.hpp"
#include "core/result.hpp"
#include "platform/windows/windows_capture_work.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <winrt/base.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>
#include <functional>
#include <type_traits>
#include <string_view>

namespace hdrshot {

inline Error windows_capture_error(const char* reason,
    std::optional<HRESULT> hr = {},
    std::source_location origin = std::source_location::current()) {
  Error error{ErrorCode::capture_failed, "WindowsCapture", Retryability::after_recreate,
      {{"reason", reason}}, origin};
  if (hr) error.safe_context["hresult"] = std::to_string(static_cast<std::uint32_t>(*hr));
  return error;
}

struct WindowsCaptureCallFailure { Error error; HRESULT native_code; };

// Thread-local scope is installed on both worker and WGC callback. Hooks never
// escape their owning task; the optional operation seam wraps the real helper.
struct WindowsCaptureCallHooks {
  std::function<void(const char*, bool)> boundary;
  std::function<void(const Error&)> failed;
};
inline thread_local const WindowsCaptureCallHooks* windows_active_capture_calls{};
struct WindowsCaptureCallScope {
  const WindowsCaptureCallHooks* previous;
  explicit WindowsCaptureCallScope(const WindowsCaptureCallHooks* hooks)
      : previous(windows_active_capture_calls) { windows_active_capture_calls=hooks; }
  ~WindowsCaptureCallScope() { windows_active_capture_calls=previous; }
};
inline void windows_capture_failed(const Error& error) noexcept {
  try { if(windows_active_capture_calls && windows_active_capture_calls->failed)
    windows_active_capture_calls->failed(error); } catch (...) {}
}

template<class Value>
struct WindowsCaptureStatus {
  Value value;
  const char* api;
  std::source_location origin;
  Error error(const char* reason, std::optional<HRESULT> hr = {}) const {
    auto error = windows_capture_error(reason, hr, origin);
    error.safe_context["api"] = api;
    return error;
  }
};
// Returned statuses need the same call-site provenance as thrown errors;
// callers still decide which status is expected (e.g. enumeration exhausted).
template<class Call>
auto windows_capture_status(const char* api, Call&& call,
    std::source_location origin = std::source_location::current()) {
  return WindowsCaptureStatus<decltype(call())>{std::forward<Call>(call)(), api, origin};
}

// Every potentially throwing native operation calls this at its own source
// line. The lambda executes immediately; Error owns all diagnostic values.
template<class Call>
decltype(auto) windows_capture_call(const char* api, Call&& call,
    std::source_location origin = std::source_location::current(),
    const char* reason = "wgc_capture") {
  try {
    if(windows_active_capture_calls && windows_active_capture_calls->boundary)
      windows_active_capture_calls->boundary(api, true);
    if constexpr(std::is_void_v<decltype(call())>) {
      std::forward<Call>(call)();
      if(windows_active_capture_calls && windows_active_capture_calls->boundary)
        windows_active_capture_calls->boundary(api, false);
      return;
    } else {
      auto value=std::forward<Call>(call)();
      if(windows_active_capture_calls && windows_active_capture_calls->boundary)
        windows_active_capture_calls->boundary(api, false);
      return value;
    }
  } catch (const winrt::hresult_error& exception) {
    auto error = windows_capture_error(reason, exception.code(), origin);
    error.safe_context["api"] = api;
    windows_capture_failed(error);
    throw WindowsCaptureCallFailure{std::move(error), static_cast<HRESULT>(exception.code())};
  } catch (const WindowsCaptureCallFailure&) {
    throw;
  } catch (...) {
    auto error=windows_capture_error(std::string_view(reason)=="frame_acquisition_failed" ?
        "frame_callback_failure" : "capture_resource_failure", {}, origin);
    error.safe_context["api"]=api;
    windows_capture_failed(error);
    throw WindowsCaptureCallFailure{std::move(error),S_OK};
  }
}

// Shared by the real free-threaded callback and deterministic state tests.
// Once a frame, an error or wait's terminal decision wins, late callbacks can
// neither replace the accepted value nor reopen a completed operation.
template<class Frame>
class WindowsCaptureFrameState {
 public:
  std::function<void()> accepted;
  std::function<void(const Error&)> failed;
  template<class Fetch>
  void receive(Fetch&& fetch,
      std::source_location origin = std::source_location::current()) noexcept {
    try {
      { const std::scoped_lock lock(mutex_); if(closed_ || frame_ || error_) return; }
      auto frame = windows_capture_call("TryGetNextFrame", std::forward<Fetch>(fetch), origin,
          "frame_acquisition_failed");
      bool won=false;
      {
        const std::scoped_lock lock(mutex_);
        if (!closed_ && !frame_ && !error_ && !callback_failed_ && frame) { frame_ = std::move(frame); won=true; }
      }
      if(won && accepted) {
        try { accepted(); }
        catch (...) {
          std::optional<Frame> discarded;
          { const std::scoped_lock lock(mutex_); discarded.swap(frame_); callback_failed_=true; callback_origin_=origin; }
          try { if(failed) failed(windows_capture_error("frame_callback_failure",{},origin)); } catch (...) {}
          ready_.notify_one();
          return;
        }
      }
      if(won) { const std::scoped_lock lock(mutex_); published_=true; }
    } catch (WindowsCaptureCallFailure& failure) {
      fail(std::move(failure.error));
    } catch (...) {
      try { fail(windows_capture_error("frame_callback_failure", {}, origin)); } catch (...) {}
      // No diagnostic allocation on a failing callback/ABI boundary. Build
      // the generic internal Error later on the waiting worker.
      const std::scoped_lock lock(mutex_);
      if (!closed_ && !frame_ && !error_ && !callback_failed_) {
        callback_failed_ = true;
        callback_origin_ = origin;
      }
    }
    ready_.notify_one();
  }

  Result<Frame, Error> wait(const std::atomic_bool* cancelled,
      std::optional<std::uint32_t> timeout_ms,
      std::source_location origin = std::source_location::current()) {
    std::unique_lock lock(mutex_);
    const auto deadline = timeout_ms ? std::chrono::steady_clock::now() + std::chrono::milliseconds(*timeout_ms)
                                    : std::chrono::steady_clock::time_point::max();
    while ((!frame_ || !published_) && !error_ && !callback_failed_ && !closed_ && !(cancelled && cancelled->load()) &&
        std::chrono::steady_clock::now() < deadline) {
      ready_.wait_until(lock, std::min(deadline,
          std::chrono::steady_clock::now() + std::chrono::milliseconds(50)));
    }
    const bool externally_closed = closed_;
    closed_ = true;
    // Keep the adapter's existing explicit cancellation precedence.
    if (cancelled && cancelled->load()) {
      auto error = windows_capture_error("cancelled", {}, origin);
      error.code = ErrorCode::operation_cancelled;
      error.retryability = Retryability::never;
      return Result<Frame, Error>::failure(std::move(error));
    }
    if (frame_ && published_) return Result<Frame, Error>::success(std::move(*frame_));
    if (error_) return Result<Frame, Error>::failure(*error_);
    if (callback_failed_) return Result<Frame, Error>::failure(
        windows_capture_error("frame_callback_failure", {}, callback_origin_));
    if (externally_closed) return Result<Frame, Error>::failure(windows_capture_error("frame_closed", {}, origin));
    return Result<Frame, Error>::failure(windows_capture_error("frame_timeout", {}, origin));
  }

  void report_failure(Error error) { fail(std::move(error)); }
  void close() noexcept {
    // Callback hook closures may retain this state after worker teardown.
    // They must not thereby retain an unconsumed native frame past drain.
    std::optional<Frame> discarded;
    {
      const std::scoped_lock lock(mutex_);
      closed_ = true;
      published_ = false;
      discarded.swap(frame_);
    }
    discarded.reset(); // Native release must not run under the state mutex.
    ready_.notify_one();
  }

 private:
  void fail(Error error) {
    bool won=false;
    { const std::scoped_lock lock(mutex_);
      if (!closed_ && !frame_ && !error_ && !callback_failed_) { error_ = error; won=true; }
    }
    if(won && failed) failed(error);
  }
  std::mutex mutex_;
  std::condition_variable ready_;
  std::optional<Frame> frame_;
  std::optional<Error> error_;
  bool callback_failed_{};
  std::source_location callback_origin_{};
  bool closed_{};
  bool published_{};
};

// Production callback boundary, also usable without an interactive desktop.
// Native values are injected, while ticket ordering, first-result arbitration,
// validation-before-progress and failure provenance remain production code.
template<class Frame, class Fetch, class Validate>
void windows_receive_capture_frame(const WindowsCallbackTicket& ticket,
    WindowsCaptureFrameState<Frame>& state, const WindowsCaptureCallHooks& calls,
    Fetch&& fetch, Validate&& validate,
    std::source_location origin=std::source_location::current()) noexcept {
  if(!ticket.admitted) return;
  WindowsCaptureCallScope scope(&calls);
  state.receive([&]() -> Frame {
    auto value=fetch();
    if(!value) return {};
    return validate(std::move(value));
  },origin);
}
// Standalone helper scope for callers with no additional callback-local native
// resources. A native delegate owns its ticket outside its hooks and locals and
// uses the overload above, so the ticket ends at the actual application tail.
template<class Frame, class Fetch, class Validate>
void windows_receive_capture_frame(const std::shared_ptr<WindowsCaptureWork>& work,
    WindowsCaptureFrameState<Frame>& state, const WindowsCaptureCallHooks& calls,
    Fetch&& fetch, Validate&& validate,
    std::source_location origin=std::source_location::current()) noexcept {
  WindowsCallbackTicket ticket(work);
  windows_receive_capture_frame(ticket,state,calls,std::forward<Fetch>(fetch),
      std::forward<Validate>(validate),origin);
}
inline void windows_validate_capture_extent(unsigned format, unsigned expected_format,
    int width, int height, int expected_width, int expected_height,
    unsigned texture_width, unsigned texture_height,
    std::source_location origin=std::source_location::current()) {
  if(format==expected_format && width>0 && height>0 && width==expected_width && height==expected_height &&
      static_cast<unsigned>(width)<=texture_width && static_cast<unsigned>(height)<=texture_height) return;
  auto error=windows_capture_error("capture_size_or_format_changed",{},origin);
  windows_capture_failed(error);
  throw WindowsCaptureCallFailure{std::move(error),S_OK};
}
template<class Stop, class Map, class Copy, class Unmap>
void windows_copy_capture_rows(std::size_t rows,Stop stop,Map map,Copy copy,Unmap unmap,
    std::source_location origin=std::source_location::current()) {
  const auto check=[&]{if(stop())throw WindowsCaptureCallFailure{
      Error{ErrorCode::operation_cancelled,"WindowsCapture",Retryability::never,{}},S_OK};};
  check();
  bool mapped=false;
  struct Guard {Unmap& unmap;bool& mapped;~Guard(){if(mapped)unmap();}} guard{unmap,mapped};
  windows_capture_call("ID3D11DeviceContext.Map",[&]{map();mapped=true;},origin);
  windows_capture_call("CPU.copy",[&]{
    for(std::size_t row=0;row<rows;++row){check();copy(row);}
  },origin);
}

struct WindowsCaptureApartment {
  HRESULT hr;
  std::source_location origin;
  explicit WindowsCaptureApartment(std::source_location at=std::source_location::current())
      :hr(windows_capture_call("CoInitializeEx",[]{return CoInitializeEx(nullptr,COINIT_MULTITHREADED);},at)),origin(at) {
    if(FAILED(hr) && hr!=RPC_E_CHANGED_MODE){auto error=windows_capture_error("wgc_apartment_unavailable",hr,origin);error.safe_context["api"]="CoInitializeEx";windows_capture_failed(error);}
  }
  ~WindowsCaptureApartment(){
    if(SUCCEEDED(hr)){
      try{if(windows_active_capture_calls && windows_active_capture_calls->boundary)windows_active_capture_calls->boundary("CoUninitialize",true);}catch(...){}
      CoUninitialize();
      try{if(windows_active_capture_calls && windows_active_capture_calls->boundary)windows_active_capture_calls->boundary("CoUninitialize",false);}catch(...){}
    }
  }
};

// Cleanup is attempted independently. Its first error is retained separately
// so it can never replace an earlier capture error, nor disappear on success.
class WindowsCaptureCleanup {
 public:
  template<class Close>
  void attempt(const char* api, Close&& close,
      std::source_location origin = std::source_location::current()) noexcept {
    try {
      windows_capture_call(api, std::forward<Close>(close), origin, "capture_cleanup_failed");
    } catch (WindowsCaptureCallFailure& failure) {
      if (!failed()) error_ = std::move(failure.error);
    } catch (...) {
      if (!failed()) {
        internal_failure_ = true;
        failure_origin_ = origin;
        failure_api_ = api;
      }
    }
  }
  [[nodiscard]] bool failed() const noexcept { return error_.has_value() || internal_failure_; }
  [[nodiscard]] Error error() const {
    if (error_) return *error_;
    auto error = windows_capture_error("capture_cleanup_failed", {}, failure_origin_);
    error.safe_context["api"] = failure_api_;
    return error;
  }
  template<class Value>
  Result<Value, Error> complete(Result<Value, Error> result) const {
    if (result && failed()) return Result<Value, Error>::failure(error());
    return result;
  }

 private:
  std::optional<Error> error_;
  bool internal_failure_{};
  std::source_location failure_origin_{};
  const char* failure_api_{"unknown"};
};

}  // namespace hdrshot
