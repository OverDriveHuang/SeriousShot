#include "platform/macos/macos_capture_ports.hpp"
#include "platform/macos/macos_gpu_source.hpp"
#include "ports/capture_progress_port.hpp"
#include "application/capture_inflight_diagnostics.hpp"
#include <cstdio>
#include <dispatch/dispatch.h>
#include <atomic>

#include <algorithm>
#include <chrono>
#include <bit>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace hdrshot {
namespace {

Error adapter_error(
    const ErrorCode code,
    const Retryability retryability,
    std::map<std::string, std::string> context = {},
    std::source_location origin = std::source_location::current()) {
  return Error{code, "MacCapturePort", retryability, std::move(context), origin};
}

void hash_mix(std::uint64_t& hash, const std::uint64_t value) {
  constexpr std::uint64_t prime = 1099511628211ULL;
  for (std::size_t shift = 0; shift < 64U; shift += 8U) {
    hash ^= (value >> shift) & 0xFFU;
    hash *= prime;
  }
}

DisplayGeneration generation_for(const std::vector<MacDisplayInfo>& displays) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto& display : displays) {
    hash_mix(hash, display.display_id);
    hash_mix(hash, std::bit_cast<std::uint64_t>(display.desktop_x_pt));
    hash_mix(hash, std::bit_cast<std::uint64_t>(display.desktop_y_pt));
    hash_mix(hash, std::bit_cast<std::uint64_t>(display.width_pt));
    hash_mix(hash, std::bit_cast<std::uint64_t>(display.height_pt));
    hash_mix(hash, std::bit_cast<std::uint64_t>(display.point_pixel_scale));
    hash_mix(hash, display.capture_width_px);
    hash_mix(hash, display.capture_height_px);
    hash_mix(hash, std::bit_cast<std::uint64_t>(display.current_maximum_edr));
    hash_mix(hash, std::bit_cast<std::uint64_t>(display.maximum_potential_edr));
  }
  return hash == 0 ? 1 : hash;
}

Result<DisplaySnapshot, Error> map_display(const MacDisplayInfo& display) {
  if (display.capture_width_px > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
      display.capture_height_px > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    return Result<DisplaySnapshot, Error>::failure(adapter_error(
        ErrorCode::invalid_input,
        Retryability::never,
        {{"displayId", std::to_string(display.display_id)}, {"reason", "pixel_size_overflow"}}));
  }
  return Result<DisplaySnapshot, Error>::success(DisplaySnapshot{
      DisplayId{display.display_id},
      LogicalRect{
          display.desktop_x_pt,
          display.desktop_y_pt,
          display.width_pt,
          display.height_pt,
      },
      display.point_pixel_scale,
      PixelSize{
          static_cast<std::int32_t>(display.capture_width_px),
          static_cast<std::int32_t>(display.capture_height_px),
      },
      display.current_maximum_edr > 1.0
          ? DisplayDynamicRange::hdr
          : DisplayDynamicRange::sdr,
  });
}

NativeCaptureFrame map_frame(MacCapturedFrame frame) {
  return NativeCaptureFrame{
      DisplayId{frame.descriptor.display_id},
      PixelSize{
          static_cast<std::int32_t>(frame.descriptor.width_px),
          static_cast<std::int32_t>(frame.descriptor.height_px),
      },
      PixelFormat::rgba16_float,
      ColorEncoding{
          ColorPrimaries::display_p3,
          TransferFunction::extended_srgb,
          AlphaMode::opaque,
          0.0,
      },
      std::move(frame.rgba_half_extended_p3),
      {},
      macos_source_normalizer(),
  };
}

}  // namespace

struct MacDisplayCatalogPort::State {
  std::mutex mutex; bool cancelled{}; bool completed{}; SnapshotDisplaysRequest request{}; Completion completion;
  std::shared_ptr<CaptureInFlightDiagnostics> diagnostics;
};
MacDisplayCatalogPort::MacDisplayCatalogPort(std::shared_ptr<DiagnosticsPort> diagnostics, Enumerate enumerate)
    : state_(std::make_shared<State>()), enumerate_(enumerate ? std::move(enumerate) : MacScreenCaptureKitAdapter::enumerate_displays),
      diagnostics_(std::move(diagnostics)) {}
MacDisplayCatalogPort::~MacDisplayCatalogPort() {
  std::lock_guard lock(state_->mutex); state_->cancelled = true; state_->completion = {};
}
void MacDisplayCatalogPort::cancel(SessionId session, OperationId operation) {
  std::lock_guard lock(state_->mutex);
  if (state_->request.session_id == session && state_->request.operation_id == operation) {
    state_->cancelled = true; state_->completion = {};
  }
}
void MacDisplayCatalogPort::snapshot_displays(
    const SnapshotDisplaysRequest& request,
    Completion completion) {
  { std::lock_guard lock(state_->mutex); state_->cancelled = true; state_->completion = {}; }
  state_ = std::make_shared<State>(); // each operation keeps its own cancellation and completion claim
  auto state = state_;
  auto facts = std::make_shared<CaptureInFlightDiagnostics>(CaptureOperationKey{request.session_id, request.operation_id, 0}, diagnostics_);
  facts->stage({}, "getShareableContent", "enumerate_queued");
  { std::lock_guard lock(state->mutex); state->request = request; state->completed = false;
    state->cancelled = false; state->completion = std::move(completion); state->diagnostics = facts; }
  auto deliver = [state](Result<DisplaySnapshotSet, Error> result) {
    Completion done;
    { std::lock_guard lock(state->mutex);
      if (state->cancelled || state->completed) return;
      state->completed = true; done = std::move(state->completion);
    }
    if (done) done(std::move(result));
  };
  enumerate_(
      [request, facts, log = diagnostics_, completion = std::move(deliver)](
          Result<std::vector<MacDisplayInfo>, Error> result) mutable {
    facts->stage({}, "getShareableContent", "enumerate_callback");
    record_diagnostic_stage(log.get(), request.session_id, request.operation_id,
        "display", "enumerate", result ? "success" : "failure",
        result ? std::map<std::string, std::string>{{"displayCount", std::to_string(result.value().size())}}
               : std::map<std::string, std::string>{}, result ? nullptr : &result.error(), true);
    if (!result) {
      completion(Result<DisplaySnapshotSet, Error>::failure(result.error()));
      return;
    }
    auto native_displays = std::move(result.value());
    std::sort(
        native_displays.begin(), native_displays.end(),
        [](const MacDisplayInfo& left, const MacDisplayInfo& right) {
          return left.display_id < right.display_id;
        });
    std::vector<DisplaySnapshot> displays;
    displays.reserve(native_displays.size());
    for (const auto& native : native_displays) {
      record_diagnostic_stage(log.get(), request.session_id, request.operation_id,
          "display", "snapshot", "success",
          {{"displayId", std::to_string(native.display_id)},
           {"x", std::to_string(native.desktop_x_pt)}, {"y", std::to_string(native.desktop_y_pt)},
           {"logicalWidth", std::to_string(native.width_pt)},
           {"logicalHeight", std::to_string(native.height_pt)},
           {"width", std::to_string(native.capture_width_px)},
           {"height", std::to_string(native.capture_height_px)},
           {"scale", std::to_string(native.point_pixel_scale)},
           {"currentEDR", std::to_string(native.current_maximum_edr)},
           {"potentialEDR", std::to_string(native.maximum_potential_edr)}});
      auto mapped = map_display(native);
      if (!mapped) {
        completion(Result<DisplaySnapshotSet, Error>::failure(mapped.error()));
        return;
      }
      displays.push_back(std::move(mapped.value()));
    }
    completion(Result<DisplaySnapshotSet, Error>::success(DisplaySnapshotSet{
        generation_for(native_displays),
        std::move(displays),
    }));
  }, [state] { std::lock_guard lock(state->mutex); return state->cancelled; },
      [facts](const char* api, const char* stage) { facts->stage({}, api, stage); });
}

void MacDisplayCatalogPort::diagnostic_snapshot() const {
  std::shared_ptr<CaptureInFlightDiagnostics> facts;
  { std::lock_guard lock(state_->mutex); if (!state_->completed && !state_->cancelled) facts = state_->diagnostics; }
  if (facts) facts->snapshot();
}

struct MacCapturePort::State {
  struct Batch {
    struct Screen { bool result{}, drained{}, task_returned{}; };
    std::mutex mutex; CaptureBatchRequest request; Completion completion;
    std::vector<NativeCaptureFrame> frames; std::vector<Screen> screens;
    std::shared_ptr<CaptureInFlightDiagnostics> diagnostics;
    bool cancelled{}, delivered{}; std::size_t results{};
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  };
  mutable std::mutex mutex; bool shutdown{}; std::shared_ptr<Batch> active;
  void release_if_drained(const std::shared_ptr<Batch>& batch) {
    bool drained{};
    { std::lock_guard lock(batch->mutex);
      drained = std::all_of(batch->screens.begin(), batch->screens.end(), [](const auto& screen) {
        return screen.drained && screen.task_returned;
      });
    }
    if (drained) { std::lock_guard lock(mutex); if (active == batch) active.reset(); }
  }
};
MacCapturePort::MacCapturePort(std::shared_ptr<DiagnosticsPort> diagnostics,
    CaptureDisplay capture_display, NativeSubmit submit)
    : state_(std::make_shared<State>()),
      capture_display_(capture_display ? std::move(capture_display) : MacScreenCaptureKitAdapter::capture_display),
      submit_(submit ? std::move(submit) : NativeSubmit{[](std::function<void()> task) {
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ task(); });
      }}), diagnostics_(std::move(diagnostics)) {}
MacCapturePort::~MacCapturePort() { shutdown(); }
bool MacCapturePort::busy() const { std::lock_guard lock(state_->mutex); return bool(state_->active); }
void MacCapturePort::shutdown() {
  std::shared_ptr<State::Batch> batch;
  { std::lock_guard lock(state_->mutex); state_->shutdown = true; batch = state_->active; }
  if (batch) { std::lock_guard lock(batch->mutex); batch->cancelled = true; batch->completion = {}; batch->frames.clear(); }
}
void MacCapturePort::cancel(SessionId session, OperationId operation) {
  std::shared_ptr<State::Batch> batch;
  { std::lock_guard lock(state_->mutex); batch = state_->active; }
  if (!batch) return;
  {
    std::lock_guard lock(batch->mutex);
    if (batch->request.session_id != session || batch->request.operation_id != operation) return;
    batch->cancelled = true; batch->frames.clear();
  }
  for (auto target : batch->request.targets) batch->diagnostics->stage(target, "cancel", "cancel_requested");
}
void MacCapturePort::diagnostic_snapshot() const {
  std::shared_ptr<State::Batch> batch;
  { std::lock_guard lock(state_->mutex); batch = state_->active; }
  if (!batch) return;
  batch->diagnostics->snapshot();
  if (batch->diagnostics->write_failed()) std::fputs("Capture diagnostic write failed\n", stderr);
}
void MacCapturePort::capture(const CaptureBatchRequest& request, Completion completion) {
  auto reject = [&](const char* reason, ErrorCode code = ErrorCode::precondition_failed) {
    completion(Result<NativeFrameBatch, Error>::failure(adapter_error(code,
        Retryability::after_user_action, {{"reason", reason}})));
  };
  if (request.desired_format != PixelFormat::rgba16_float || request.targets.empty()) {
    reject("invalid_capture_request", ErrorCode::unsupported_pixel_format); return;
  }
  if (std::set<DisplayId>(request.targets.begin(), request.targets.end()).size() != request.targets.size()) {
    reject("duplicate_display_target", ErrorCode::invalid_input); return;
  }
  auto batch = std::make_shared<State::Batch>(); batch->request = request;
  batch->diagnostics = std::make_shared<CaptureInFlightDiagnostics>(CaptureOperationKey{request.session_id, request.operation_id, request.display_generation}, diagnostics_, request.targets);
  batch->screens.resize(request.targets.size());
  bool rejected{};
  { std::lock_guard lock(state_->mutex); rejected = state_->shutdown || bool(state_->active);
    if (!rejected) { batch->completion = std::move(completion); state_->active = batch; }
  }
  if (rejected) { reject("capture_draining"); return; }
  const auto owner = state_;
  for (std::size_t index = 0; index < request.targets.size(); ++index) {
    const auto target = request.targets[index];
    const CaptureOperationKey key{request.session_id, request.operation_id, request.display_generation};
    auto stopped = [batch] {
      std::lock_guard lock(batch->mutex);
      return batch->cancelled || (batch->request.progress && batch->request.progress->stop_requested());
    };
    auto event = [batch, key, target](CaptureNativeEventKind kind, std::optional<Error> error = {}) {
      if (batch->request.progress) batch->request.progress->native_event({key, target, kind, std::move(error)});
    };
    auto result = [batch, index, event, log = diagnostics_](Result<MacCapturedFrame, Error> result) mutable {
      if (!result && result.error().code != ErrorCode::operation_cancelled) event(CaptureNativeEventKind::failed, result.error());
      std::map<std::string, std::string> context{
          {"displayId", std::to_string(batch->request.targets[index].value)}, {"api", "captureImageWithFilter"},
          {"elapsedMs", std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - batch->started).count())}};
      if (result) {
        const auto& descriptor = result.value().descriptor;
        context.insert({{"width", std::to_string(descriptor.width_px)}, {"height", std::to_string(descriptor.height_px)},
            {"stride", std::to_string(descriptor.source_bytes_per_row)}, {"bitmapInfo", std::to_string(descriptor.bitmap_info)},
            {"bitsPerComponent", "16"}, {"bitsPerPixel", "64"}, {"transfer", "extended_srgb"},
            {"returnedSpace", descriptor.returned_space}});
      }
      record_diagnostic_stage(log.get(), batch->request.session_id, batch->request.operation_id,
          "capture", "native_return", result ? "success" : "failure", std::move(context), result ? nullptr : &result.error(), true);
      bool map_success{};
      { std::lock_guard lock(batch->mutex);
        auto& screen = batch->screens[index]; if (screen.result) return;
        screen.result = true;
        if (batch->delivered) return;
        map_success = result && !batch->cancelled &&
            !(batch->request.progress && batch->request.progress->stop_requested());
      }
      // Attaching the source normalizer may initialize Metal; never hold the cancel lock across it.
      std::optional<NativeCaptureFrame> mapped;
      if (map_success) {
        try { mapped = map_frame(std::move(result.value())); }
        catch (...) {
          result = Result<MacCapturedFrame, Error>::failure(adapter_error(ErrorCode::capture_failed,
              Retryability::after_user_action, {{"reason", "source_prepare_exception"}}));
          event(CaptureNativeEventKind::failed, result.error());
        }
      }
      Completion done; std::optional<Result<NativeFrameBatch, Error>> outcome;
      { std::lock_guard lock(batch->mutex);
        ++batch->results;
        if (batch->delivered) return;
        if (batch->cancelled || (batch->request.progress && batch->request.progress->stop_requested())) {
          outcome = Result<NativeFrameBatch, Error>::failure(adapter_error(ErrorCode::operation_cancelled, Retryability::never));
        } else if (!result) outcome = Result<NativeFrameBatch, Error>::failure(result.error());
        else {
          batch->frames.push_back(std::move(*mapped));
          if (batch->results == batch->screens.size()) {
            std::sort(batch->frames.begin(), batch->frames.end(), [&batch](const auto& a, const auto& b) {
              return std::find(batch->request.targets.begin(), batch->request.targets.end(), a.display_id) <
                  std::find(batch->request.targets.begin(), batch->request.targets.end(), b.display_id);
            });
            outcome = Result<NativeFrameBatch, Error>::success({batch->request.session_id,
                batch->request.operation_id, batch->request.display_generation, std::move(batch->frames)});
          }
        }
        if (outcome) { batch->delivered = true; done = std::move(batch->completion); }
      }
      if (done) done(std::move(*outcome));
    };
    auto drained = [batch, index, owner] {
      { std::lock_guard lock(batch->mutex); batch->screens[index].drained = true; }
      batch->diagnostics->stage(batch->request.targets[index], "cleanup", "drained");
      owner->release_if_drained(batch);
    };
    MacCaptureHooks hooks;
    hooks.cancelled = stopped;
    hooks.try_begin_step = [batch](const char*) {
      std::lock_guard lock(batch->mutex);
      // The queued/outer-call ticket already holds this screen. The final atomic
      // stop read is the permission linearization point; no API runs under this lock.
      return !batch->cancelled && (!batch->request.progress || !batch->request.progress->stop_requested());
    };
    hooks.image_received = [event, batch, target] { event(CaptureNativeEventKind::image_received); batch->diagnostics->image_received(target); };
    hooks.failed = [event, batch, target](const Error& error) { event(CaptureNativeEventKind::failed, error); batch->diagnostics->failure(target, error, "native_error"); };
    hooks.drained = drained;
    hooks.stage = [batch, index](const char* api, const char* stage) {
      batch->diagnostics->stage(batch->request.targets[index], api, stage);
    };
    batch->diagnostics->stage(target, "capture", "native_queued");
    record_diagnostic_stage(diagnostics_.get(), request.session_id, request.operation_id,
        "capture", "native_begin", "queued", {{"displayId", std::to_string(target.value)},
        {"api", "captureImageWithFilter"}, {"requestedSpace", "ExtendedDisplayP3"},
        {"requestedFormat", "RGhA"}, {"preset", "HDRCanonical"}, {"opaque", "yes"}, {"cursor", "no"}}, nullptr, true);
    auto task_returned = [batch, index, owner] {
      { std::lock_guard lock(batch->mutex); batch->screens[index].task_returned = true; }
      owner->release_if_drained(batch);
    };
    try {
      submit_([target, stopped, result, drained, hooks, task_returned, capture = capture_display_]() mutable {
        try {
          if (stopped()) { result(Result<MacCapturedFrame, Error>::failure(adapter_error(ErrorCode::operation_cancelled, Retryability::never))); drained(); }
          else capture(static_cast<std::uint32_t>(target.value), result, std::move(hooks));
        } catch (...) {
          result(Result<MacCapturedFrame, Error>::failure(adapter_error(ErrorCode::capture_failed,
              Retryability::after_user_action, {{"reason", "native_submit_exception"}}))); drained();
        }
        // Includes the queued task and outer adapter call even for synchronous callbacks.
        task_returned();
      });
    } catch (...) {
      result(Result<MacCapturedFrame, Error>::failure(adapter_error(ErrorCode::capture_failed,
          Retryability::after_user_action, {{"reason", "native_enqueue_exception"}})));
      drained(); task_returned();
    }
  }
}
}  // namespace hdrshot
