#include "platform/macos/macos_preview_presenter_port.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <utility>

namespace hdrshot {
namespace {

Error presenter_error(
    const ErrorCode code,
    const Retryability retryability,
    std::map<std::string, std::string> context = {},
    std::source_location origin = std::source_location::current()) {
  return Error{code, "MacPreviewPresenterPort", retryability, std::move(context), origin};
}

}  // namespace

MacPreviewPresenterPort::MacPreviewPresenterPort(
    std::shared_ptr<MacMetalEdrPresenter> presenter,
    void* native_metal_layer,
    const double target_current_maximum_edr)
    : presenter_(std::move(presenter)),
      render_backend_([presenter = presenter_](
                          void* native_layer,
                          const MacMetalOverlayRequest& request) {
        if (presenter == nullptr) {
          return Result<MacMetalPresentReceipt, Error>::failure(presenter_error(
              ErrorCode::presenter_failed,
              Retryability::after_recreate,
              {{"reason", "missing_presenter"}}));
        }
        return presenter->present_to_layer(native_layer, request);
      }),
      native_metal_layer_(native_metal_layer),
      target_surface_range_(
          target_current_maximum_edr > 1.0
              ? MacMetalSurfaceRange::edr
              : MacMetalSurfaceRange::sdr),
      worker_([this] { worker_loop(); }) {}

MacPreviewPresenterPort::MacPreviewPresenterPort(
    RenderBackend render_backend,
    void* native_metal_layer,
    const double target_current_maximum_edr)
    : render_backend_(std::move(render_backend)),
      native_metal_layer_(native_metal_layer),
      target_surface_range_(
          target_current_maximum_edr > 1.0
              ? MacMetalSurfaceRange::edr
              : MacMetalSurfaceRange::sdr),
      worker_([this] { worker_loop(); }) {}

MacPreviewPresenterPort::~MacPreviewPresenterPort() {
  std::optional<WorkItem> abandoned;
  {
    const std::scoped_lock lock(mutex_);
    stopping_ = true;
    abandoned = std::move(latest_pending_);
    latest_pending_.reset();
  }
  work_available_.notify_one();
  if (abandoned.has_value()) {
    abandoned->completion(Result<PresentReceipt, Error>::failure(presenter_error(
        ErrorCode::operation_cancelled,
        Retryability::never,
        {{"reason", "presenter_shutdown"}})));
  }
  if (worker_.joinable()) {
    worker_.join();
  }
}

void MacPreviewPresenterPort::set_native_metal_layer(void* native_metal_layer) {
  const std::scoped_lock lock(mutex_);
  native_metal_layer_ = native_metal_layer;
}

void MacPreviewPresenterPort::present(
    const PresentPreviewRequest& request,
    Completion completion) {
  const auto key = std::pair{
      request.model.session_id.value,
      request.operation_id.value,
  };
  std::optional<WorkItem> displaced;
  bool rejected = false;
  {
    const std::scoped_lock lock(mutex_);
    if (stopping_ || cancelled_.contains(key)) {
      rejected = true;
    } else {
      displaced = std::move(latest_pending_);
      latest_pending_ = WorkItem{request, std::move(completion)};
    }
  }
  if (rejected) {
    completion(Result<PresentReceipt, Error>::failure(presenter_error(
        ErrorCode::operation_cancelled, Retryability::never)));
    return;
  }
  if (displaced.has_value()) {
    displaced->completion(Result<PresentReceipt, Error>::failure(presenter_error(
        ErrorCode::operation_cancelled,
        Retryability::never,
        {{"reason", "superseded_by_newer_revision"}})));
  }
  work_available_.notify_one();
}

void MacPreviewPresenterPort::worker_loop() {
  while (true) {
    std::optional<WorkItem> work;
    {
      std::unique_lock lock(mutex_);
      work_available_.wait(lock, [this] {
        return stopping_ || latest_pending_.has_value();
      });
      if (stopping_ && !latest_pending_.has_value()) {
        return;
      }
      work = std::move(latest_pending_);
      latest_pending_.reset();
    }
    render(std::move(*work));
  }
}

void MacPreviewPresenterPort::render(WorkItem work) {
  const auto& request = work.request;
  const auto key = std::pair{
      request.model.session_id.value,
      request.operation_id.value,
  };
  void* native_metal_layer = nullptr;
  bool cancelled = false;
  {
    const std::scoped_lock lock(mutex_);
    cancelled = cancelled_.contains(key);
    native_metal_layer = native_metal_layer_;
  }
  if (cancelled) {
    work.completion(Result<PresentReceipt, Error>::failure(presenter_error(
        ErrorCode::operation_cancelled, Retryability::never)));
    return;
  }
  if (!render_backend_ || native_metal_layer == nullptr ||
      request.model.frozen_desktop == nullptr ||
      request.model.frozen_desktop->canonical_segments.empty()) {
    work.completion(Result<PresentReceipt, Error>::failure(presenter_error(
        ErrorCode::presenter_failed,
        Retryability::after_recreate,
        {{"reason", "missing_surface_or_frame"}})));
    return;
  }
  const auto& frame = *request.model.frozen_desktop;
  const auto segment = request.target_display_id.value == 0U &&
          frame.canonical_segments.size() == 1U
      ? frame.canonical_segments.begin()
      : std::find_if(
            frame.canonical_segments.begin(), frame.canonical_segments.end(),
            [&request](const CanonicalFrameSegment& candidate) {
              return candidate.display_id == request.target_display_id;
            });
  if (segment == frame.canonical_segments.end()) {
    work.completion(Result<PresentReceipt, Error>::failure(presenter_error(
        ErrorCode::object_not_found,
        Retryability::after_recreate,
        {{"reason", "target_display_segment_missing"},
         {"displayId", std::to_string(request.target_display_id.value)}})));
    return;
  }
  if (segment->encoding.primaries != ColorPrimaries::display_p3 ||
      (segment->encoding.transfer != TransferFunction::extended_srgb &&
       segment->encoding.transfer != TransferFunction::linear) ||
      (segment->pixel_format != PixelFormat::rgba16_float &&
       segment->pixel_format != PixelFormat::rgba32_float)) {
    work.completion(Result<PresentReceipt, Error>::failure(presenter_error(
        ErrorCode::invalid_color_contract, Retryability::never)));
    return;
  }
  const auto rendered = render_backend_(
      native_metal_layer,
      MacMetalOverlayRequest{
          static_cast<std::size_t>(segment->size_px.width),
          static_cast<std::size_t>(segment->size_px.height),
          segment->rgba_half.empty() ? nullptr : std::shared_ptr<const std::vector<std::uint16_t>>(
              request.model.frozen_desktop, &segment->rgba_half),
          preview_selection_rect(request.model),
          request.model.overlay_style.outside_linear_dim_factor,
          target_surface_range_ == MacMetalSurfaceRange::edr
              ? request.model.overlay_style.ui_white_edr
              : 1.0F,
          request.model.overlay_style.selection_border_width_px,
          target_surface_range_,
          frame.frame_id.value,
          segment->encoding.transfer == TransferFunction::linear,
          segment->rgba_float.empty() ? nullptr : std::shared_ptr<const LinearFloatPixels>(
              request.model.frozen_desktop, &segment->rgba_float),
          request.model.clean_content,
          segment->linear_source,
      });
  if (!rendered) {
    work.completion(Result<PresentReceipt, Error>::failure(rendered.error()));
    return;
  }
  {
    const std::scoped_lock lock(mutex_);
    cancelled = cancelled_.contains(key);
  }
  if (cancelled) {
    work.completion(Result<PresentReceipt, Error>::failure(presenter_error(
        ErrorCode::operation_cancelled, Retryability::never)));
    return;
  }
  work.completion(Result<PresentReceipt, Error>::success(PresentReceipt{
      request.model.session_id,
      request.operation_id,
      frame.frame_id,
      frame.display_generation,
      request.model.selection.revision,
      request.model.annotation_document.revision,
      request.model.initial_highlight.revision,
  }));
}

void MacPreviewPresenterPort::cancel(
    const SessionId session_id,
    const OperationId operation_id) {
  std::optional<WorkItem> cancelled_pending;
  {
    const std::scoped_lock lock(mutex_);
    const auto key = std::pair{session_id.value, operation_id.value};
    cancelled_.insert(key);
    if (latest_pending_.has_value() &&
        latest_pending_->request.model.session_id == session_id &&
        latest_pending_->request.operation_id == operation_id) {
      cancelled_pending = std::move(latest_pending_);
      latest_pending_.reset();
    }
  }
  if (cancelled_pending.has_value()) {
    cancelled_pending->completion(Result<PresentReceipt, Error>::failure(presenter_error(
        ErrorCode::operation_cancelled, Retryability::never)));
  }
}

}  // namespace hdrshot
