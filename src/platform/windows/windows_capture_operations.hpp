#pragma once

#include "core/error.hpp"
#include "core/result.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winrt/base.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

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
    return std::forward<Call>(call)();
  } catch (const winrt::hresult_error& exception) {
    auto error = windows_capture_error(reason, exception.code(), origin);
    error.safe_context["api"] = api;
    throw WindowsCaptureCallFailure{std::move(error), static_cast<HRESULT>(exception.code())};
  }
}

// Shared by the real free-threaded callback and deterministic state tests.
// Once a frame, an error or wait's terminal decision wins, late callbacks can
// neither replace the accepted value nor reopen a completed operation.
template<class Frame>
class WindowsCaptureFrameState {
 public:
  template<class Fetch>
  void receive(Fetch&& fetch,
      std::source_location origin = std::source_location::current()) noexcept {
    try {
      auto frame = windows_capture_call("TryGetNextFrame", std::forward<Fetch>(fetch), origin,
          "frame_acquisition_failed");
      {
        const std::scoped_lock lock(mutex_);
        if (!closed_ && !frame_ && !error_ && !callback_failed_ && frame) frame_ = std::move(frame);
      }
    } catch (WindowsCaptureCallFailure& failure) {
      fail(std::move(failure.error));
    } catch (...) {
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
      std::uint32_t timeout_ms,
      std::source_location origin = std::source_location::current()) {
    std::unique_lock lock(mutex_);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!frame_ && !error_ && !callback_failed_ && !closed_ && !(cancelled && cancelled->load()) &&
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
    if (frame_) return Result<Frame, Error>::success(std::move(*frame_));
    if (error_) return Result<Frame, Error>::failure(*error_);
    if (callback_failed_) return Result<Frame, Error>::failure(
        windows_capture_error("frame_callback_failure", {}, callback_origin_));
    if (externally_closed) return Result<Frame, Error>::failure(windows_capture_error("frame_closed", {}, origin));
    return Result<Frame, Error>::failure(windows_capture_error("frame_timeout", {}, origin));
  }

  void close() noexcept {
    const std::scoped_lock lock(mutex_);
    closed_ = true;
    ready_.notify_one();
  }

 private:
  void fail(Error error) {
    const std::scoped_lock lock(mutex_);
    if (!closed_ && !frame_ && !error_ && !callback_failed_) error_ = std::move(error);
  }
  std::mutex mutex_;
  std::condition_variable ready_;
  std::optional<Frame> frame_;
  std::optional<Error> error_;
  bool callback_failed_{};
  std::source_location callback_origin_{};
  bool closed_{};
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
