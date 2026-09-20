#include "platform/macos/macos_capture_ports.hpp"
#include "platform/macos/macos_gpu_source.hpp"

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

using OperationKey = std::pair<std::uint64_t, std::uint64_t>;

OperationKey operation_key(const SessionId session, const OperationId operation) {
  return {session.value, operation.value};
}

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

struct MacCapturePort::CancellationRegistry {
  std::mutex mutex;
  std::set<OperationKey> cancelled;

  bool is_cancelled(const OperationKey key) {
    const std::scoped_lock lock(mutex);
    return cancelled.contains(key);
  }
};

void MacDisplayCatalogPort::snapshot_displays(
    const SnapshotDisplaysRequest& request,
    Completion completion) {
  MacScreenCaptureKitAdapter::enumerate_displays(
      [request, log = diagnostics_, completion = std::move(completion)](
          Result<std::vector<MacDisplayInfo>, Error> result) mutable {
    record_diagnostic_stage(log.get(), request.session_id, request.operation_id,
        "display", "enumerate", result ? "success" : "failure",
        result ? std::map<std::string, std::string>{{"displayCount", std::to_string(result.value().size())}}
               : std::map<std::string, std::string>{}, result ? nullptr : &result.error());
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
  });
}

MacCapturePort::MacCapturePort(std::shared_ptr<DiagnosticsPort> diagnostics)
    : cancellations_(std::make_shared<CancellationRegistry>()), diagnostics_(std::move(diagnostics)) {}
MacCapturePort::~MacCapturePort() = default;

void MacCapturePort::capture(const CaptureBatchRequest& request, Completion completion) {
  if (request.desired_format != PixelFormat::rgba16_float || request.targets.empty()) {
    completion(Result<NativeFrameBatch, Error>::failure(adapter_error(
        ErrorCode::unsupported_pixel_format, Retryability::never)));
    return;
  }
  std::set<DisplayId> unique_targets(request.targets.begin(), request.targets.end());
  if (unique_targets.size() != request.targets.size()) {
    completion(Result<NativeFrameBatch, Error>::failure(adapter_error(
        ErrorCode::invalid_input,
        Retryability::never,
        {{"reason", "duplicate_display_target"}})));
    return;
  }

  struct BatchState {
    std::mutex mutex;
    CaptureBatchRequest request;
    Completion completion;
    std::vector<NativeCaptureFrame> frames;
    std::size_t remaining{};
    bool completed{};
  };
  auto state = std::make_shared<BatchState>();
  state->request = request;
  state->completion = std::move(completion);
  state->remaining = request.targets.size();
  const auto key = operation_key(request.session_id, request.operation_id);
  const auto registry = cancellations_;

  for (const auto target : request.targets) {
    const auto started = std::chrono::steady_clock::now();
    record_diagnostic_stage(diagnostics_.get(), request.session_id, request.operation_id,
        "capture", "native_begin", "started", {{"displayId", std::to_string(target.value)},
        {"api", "captureImageWithFilter"}, {"requestedSpace", "ExtendedDisplayP3"},
        {"requestedFormat", "RGhA"}, {"preset", "HDRCanonical"}, {"opaque", "yes"}, {"cursor", "no"}});
    MacScreenCaptureKitAdapter::capture_display(
        static_cast<std::uint32_t>(target.value),
        [state, registry, key, target, started, log = diagnostics_](Result<MacCapturedFrame, Error> result) mutable {
      std::map<std::string, std::string> context{
          {"displayId", std::to_string(target.value)}, {"api", "captureImageWithFilter"},
          {"elapsedMs", std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - started).count())}};
      if (result) {
        const auto& d = result.value().descriptor;
        context.insert({{"width", std::to_string(d.width_px)}, {"height", std::to_string(d.height_px)},
            {"stride", std::to_string(d.source_bytes_per_row)}, {"bitmapInfo", std::to_string(d.bitmap_info)},
            {"bitsPerComponent", "16"}, {"bitsPerPixel", "64"}, {"transfer", "extended_srgb"},
            {"returnedSpace", d.returned_space}});
      }
      record_diagnostic_stage(log.get(), state->request.session_id, state->request.operation_id,
          "capture", "native_return", result ? "success" : "failure", std::move(context),
          result ? nullptr : &result.error());
      Completion finished;
      std::optional<Result<NativeFrameBatch, Error>> outcome;
      {
        const std::scoped_lock lock(state->mutex);
        if (state->completed) {
          return;
        }
        if (registry->is_cancelled(key)) {
          state->completed = true;
          finished = std::move(state->completion);
          outcome = Result<NativeFrameBatch, Error>::failure(adapter_error(
              ErrorCode::operation_cancelled, Retryability::never));
        } else if (!result) {
          state->completed = true;
          finished = std::move(state->completion);
          outcome = Result<NativeFrameBatch, Error>::failure(result.error());
        } else {
          state->frames.push_back(map_frame(std::move(result.value())));
          --state->remaining;
          if (state->remaining == 0) {
            std::sort(
                state->frames.begin(), state->frames.end(),
                [&state](const NativeCaptureFrame& left, const NativeCaptureFrame& right) {
                  const auto left_index = std::find(
                      state->request.targets.begin(), state->request.targets.end(), left.display_id);
                  const auto right_index = std::find(
                      state->request.targets.begin(), state->request.targets.end(), right.display_id);
                  return left_index < right_index;
                });
            state->completed = true;
            finished = std::move(state->completion);
            outcome = Result<NativeFrameBatch, Error>::success(NativeFrameBatch{
                state->request.session_id,
                state->request.operation_id,
                state->request.display_generation,
                std::move(state->frames),
            });
          }
        }
      }
      if (outcome.has_value()) {
        finished(std::move(*outcome));
      }
    });
  }
}

void MacCapturePort::cancel(const SessionId session_id, const OperationId operation_id) {
  const std::scoped_lock lock(cancellations_->mutex);
  cancellations_->cancelled.insert(operation_key(session_id, operation_id));
}

}  // namespace hdrshot
