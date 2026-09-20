#include "application/capture_session.hpp"

#include "domain/frame/frame_pipeline.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace hdrshot {

struct CaptureSession::State : std::enable_shared_from_this<State> {
  State(std::shared_ptr<DisplayCatalogPort> catalog, std::shared_ptr<CapturePort> capture,
        std::shared_ptr<PreviewPresenterPort> presenter, std::shared_ptr<WindowCatalogPort> windows,
        std::shared_ptr<DiagnosticsPort> diagnostics)
      : catalog_owner(std::move(catalog)), capture_owner(std::move(capture)),
        presenter_owner(std::move(presenter)), window_owner(std::move(windows)), display_catalog_(*catalog_owner),
        capture_port_(*capture_owner), presenter_(*presenter_owner), diagnostics_(std::move(diagnostics)) {}
  struct RunState;
  void begin(BeginCaptureRequest, Completion);
  void capture(const std::shared_ptr<RunState>&);
  void windows_ready(const std::shared_ptr<RunState>&, Result<WindowSnapshot, Error>);
  void cancel(SessionId, OperationId);
  void abandon();
  bool is_current(const std::shared_ptr<RunState>&) const;
  void complete(const std::shared_ptr<RunState>&, Result<CaptureReadyPayload, Error>);
  std::shared_ptr<DisplayCatalogPort> catalog_owner;
  std::shared_ptr<CapturePort> capture_owner;
  std::shared_ptr<PreviewPresenterPort> presenter_owner;
  std::shared_ptr<WindowCatalogPort> window_owner;
  DisplayCatalogPort& display_catalog_;
  CapturePort& capture_port_;
  PreviewPresenterPort& presenter_;
  std::shared_ptr<DiagnosticsPort> diagnostics_;
  mutable std::mutex mutex_;
  std::shared_ptr<RunState> active_;
};

struct CaptureSession::State::RunState {
  BeginCaptureRequest request;
  Completion completion;
  std::optional<DisplaySnapshotSet> displays;
  WindowSnapshotRef windows;
  bool window_received{}, capture_started{};
};

namespace {

Error session_error(
    const ErrorCode code,
    const Retryability retryability,
    std::map<std::string, std::string> context = {},
    std::source_location origin = std::source_location::current()) {
  return Error{code, "CaptureSession", retryability, std::move(context), origin};
}

const DisplaySnapshot* find_display(
    const DisplaySnapshotSet& displays,
    const DisplayId id) {
  const auto found = std::find_if(
      displays.displays.begin(), displays.displays.end(),
      [id](const DisplaySnapshot& display) { return display.id == id; });
  return found == displays.displays.end() ? nullptr : &*found;
}

}  // namespace

bool CaptureSession::State::is_current(const std::shared_ptr<RunState>& run) const {
  const std::scoped_lock lock(mutex_);
  return active_ == run;
}

void CaptureSession::State::complete(
    const std::shared_ptr<RunState>& run,
    Result<CaptureReadyPayload, Error> result) {
  Completion completion;
  {
    const std::scoped_lock lock(mutex_);
    if (active_ != run) {
      return;
    }
    active_.reset();
    completion = std::move(run->completion);
  }
  record_diagnostic_stage(diagnostics_.get(), run->request.session_id, run->request.operation_id,
      "capture", "ready", result ? "success" : "failure", {}, result ? nullptr : &result.error());
  completion(std::move(result));
}

void CaptureSession::State::begin(BeginCaptureRequest request, Completion completion) {
  auto run = std::make_shared<RunState>(RunState{std::move(request), std::move(completion), {}, {}, false, false});
  Completion rejected;
  {
    const std::scoped_lock lock(mutex_);
    if (active_ != nullptr) {
      rejected = std::move(run->completion);
    } else {
      active_ = run;
    }
  }
  if (rejected) {
    rejected(Result<CaptureReadyPayload, Error>::failure(session_error(
        ErrorCode::precondition_failed,
        Retryability::never,
        {{"reason", "capture_already_active"}})));
    return;
  }

  record_diagnostic_stage(diagnostics_.get(), run->request.session_id, run->request.operation_id,
      "capture", "enumerate_begin", "accepted");
  display_catalog_.snapshot_displays(
      SnapshotDisplaysRequest{run->request.session_id, run->request.operation_id},
      [this, weak = weak_from_this(), run](Result<DisplaySnapshotSet, Error> snapshot_result) {
    const auto lifetime = weak.lock();
    if (!lifetime) return;
    if (!is_current(run)) {
      return;
    }
    if (!snapshot_result) {
      complete(run, Result<CaptureReadyPayload, Error>::failure(snapshot_result.error()));
      return;
    }
    auto displays = std::move(snapshot_result.value());
    record_diagnostic_stage(diagnostics_.get(), run->request.session_id, run->request.operation_id,
        "capture", "enumerate_complete", "success", {{"displayCount", std::to_string(displays.displays.size())}});
    if (displays.generation == 0 || displays.displays.empty()) {
      complete(run, Result<CaptureReadyPayload, Error>::failure(session_error(
          ErrorCode::display_configuration_changed, Retryability::same_input)));
      return;
    }
    if (run->request.targets.empty()) {
      run->request.targets.reserve(displays.displays.size());
      for (const auto& display : displays.displays) {
        run->request.targets.push_back(display.id);
      }
    }
    for (const auto target : run->request.targets) {
      if (find_display(displays, target) == nullptr) {
        complete(run, Result<CaptureReadyPayload, Error>::failure(session_error(
            ErrorCode::display_configuration_changed,
            Retryability::same_input,
            {{"missingDisplayId", std::to_string(target.value)}})));
        return;
      }
    }
    run->displays = std::move(displays);
    if (window_owner) {
      window_owner->snapshot_windows(
          {run->request.session_id, run->request.operation_id, *run->displays},
          [weak = weak_from_this(), run](Result<WindowSnapshot, Error> windows) {
            if (auto state = weak.lock()) state->windows_ready(run, std::move(windows));
          });
    } else {
      capture(run); // Unimplemented platforms retain their existing manual selector.
    }
  });
}

void CaptureSession::State::windows_ready(
    const std::shared_ptr<RunState>& run, Result<WindowSnapshot, Error> result) {
  if (!result && is_current(run))
    record_diagnostic_stage(diagnostics_.get(), run->request.session_id, run->request.operation_id,
        "window", "manual_fallback", "failure", {}, &result.error());
  {
    const std::scoped_lock lock(mutex_);
    if (active_ != run || run->window_received) return;
    run->window_received = true;
    WindowSnapshot snapshot{run->request.session_id, run->request.operation_id,
                            run->displays->generation, {}};
    if (result && result.value().session_id == snapshot.session_id &&
        result.value().operation_id == snapshot.operation_id &&
        result.value().display_generation == snapshot.display_generation) {
      for (const auto& candidate : result.value().candidates) {
        const auto* display = find_display(*run->displays, candidate.display_id);
        const auto r = candidate.bounds_px;
        if (!display || candidate.token == 0 || r.empty() || r.x < 0 || r.y < 0 ||
            static_cast<std::int64_t>(r.x) + r.width > display->capture_size_px.width ||
            static_cast<std::int64_t>(r.y) + r.height > display->capture_size_px.height) continue;
        snapshot.candidates.push_back(candidate);
      }
    }
    // Even errors carry an empty, correctly associated snapshot: hover is empty
    // but the exact same shared manual gesture path remains usable.
    run->windows = std::make_shared<const WindowSnapshot>(std::move(snapshot));
  }
  capture(run);
}

void CaptureSession::State::capture(const std::shared_ptr<RunState>& run) {
  {
    const std::scoped_lock lock(mutex_);
    if (active_ != run || run->capture_started) return;
    run->capture_started = true;
  }
    record_diagnostic_stage(diagnostics_.get(), run->request.session_id, run->request.operation_id,
        "capture", "batch_begin", "started");
    capture_port_.capture(
        CaptureBatchRequest{
            run->request.session_id,
            run->request.operation_id,
            run->displays->generation,
            run->request.targets,
            PixelFormat::rgba16_float,
            true,
        },
        [this, weak = weak_from_this(), run](Result<NativeFrameBatch, Error> capture_result) {
      const auto lifetime = weak.lock();
      if (!lifetime) return;
      if (!is_current(run)) {
        return;
      }
      if (!capture_result) {
        complete(run, Result<CaptureReadyPayload, Error>::failure(capture_result.error()));
        return;
      }
      auto batch = std::move(capture_result.value());
      record_diagnostic_stage(diagnostics_.get(), run->request.session_id, run->request.operation_id,
          "capture", "batch_complete", "success", {{"displayCount", std::to_string(batch.frames.size())}});
      if (!run->displays.has_value() ||
          batch.session_id != run->request.session_id ||
          batch.operation_id != run->request.operation_id ||
          batch.display_generation != run->displays->generation ||
          batch.frames.size() != run->request.targets.size()) {
        complete(run, Result<CaptureReadyPayload, Error>::failure(session_error(
            ErrorCode::display_configuration_changed, Retryability::same_input)));
        return;
      }

      std::vector<CanonicalFrameSegment> segments;
      segments.reserve(batch.frames.size());
      for (auto& native_frame : batch.frames) {
        const auto* display = find_display(*run->displays, native_frame.display_id);
        if (display == nullptr) {
          complete(run, Result<CaptureReadyPayload, Error>::failure(session_error(
              ErrorCode::display_configuration_changed, Retryability::same_input)));
          return;
        }
        auto interpreted = SourceColorInterpreter::interpret(
            std::move(native_frame), *display);
        if (!interpreted) {
          complete(run, Result<CaptureReadyPayload, Error>::failure(interpreted.error()));
          return;
        }
        record_diagnostic_stage(diagnostics_.get(), run->request.session_id, run->request.operation_id,
            "capture", interpreted.value().linear_source ? "source_normalization_submitted" : "source_frozen", "success", {
              {"displayId", std::to_string(interpreted.value().display_id.value)},
              {"transfer", interpreted.value().encoding.transfer == TransferFunction::linear ? "linear" : "other"},
              {"format", interpreted.value().pixel_format == PixelFormat::rgba32_float ? "RGBA32Float" : "RGBA16Float"},
              {"sourceStorage", interpreted.value().linear_source ? "native_texture" : "cpu"},
              {"linearizationPasses", std::to_string(interpreted.value().software_linearization_passes)},
              {"sourceBytes", std::to_string(interpreted.value().linear_source
                  ? interpreted.value().linear_source->byte_count()
                  : interpreted.value().rgba_float.size() * sizeof(float) +
                      interpreted.value().rgba_half.size() * sizeof(std::uint16_t))}});
        segments.push_back(std::move(interpreted.value()));
      }

      auto assembled = DisplayFrameAssembler::assemble(
          run->request.frame_id,
          run->displays->generation,
          *run->displays,
          std::move(segments));
      if (!assembled) {
        complete(run, Result<CaptureReadyPayload, Error>::failure(assembled.error()));
        return;
      }
      auto frozen = std::make_shared<const FrozenDesktop>(std::move(assembled.value()));
      auto preview = PresentPreviewRequest{
          run->request.operation_id,
          PreviewModel{
              run->request.session_id,
              frozen,
              run->request.initial_selection,
              AnnotationDocument::empty().snapshot(),
              OverlayStyle{},
              frozen->display_generation,
          },
      };
      record_diagnostic_stage(diagnostics_.get(), run->request.session_id, run->request.operation_id,
          "overlay", "first_render", "started",
          {{"dimFactor", std::to_string(preview.model.overlay_style.outside_linear_dim_factor)},
           {"uiWhite", std::to_string(preview.model.overlay_style.ui_white_edr)},
           {"borderWidth", std::to_string(preview.model.overlay_style.selection_border_width_px)}});
      presenter_.present(
          preview,
          [this, weak = weak_from_this(), run, frozen](Result<PresentReceipt, Error> present_result) {
        const auto lifetime = weak.lock();
        if (!lifetime) return;
        if (!is_current(run)) {
          return;
        }
        if (!present_result) {
          complete(run, Result<CaptureReadyPayload, Error>::failure(present_result.error()));
          return;
        }
        const auto receipt = present_result.value();
        record_diagnostic_stage(diagnostics_.get(), run->request.session_id, run->request.operation_id,
            "overlay", "first_render", "success");
        if (receipt.session_id != run->request.session_id ||
            receipt.operation_id != run->request.operation_id ||
            receipt.frame_id != frozen->frame_id ||
            receipt.display_generation != frozen->display_generation ||
            receipt.selection_revision != run->request.initial_selection.revision) {
          complete(run, Result<CaptureReadyPayload, Error>::failure(session_error(
              ErrorCode::state_inconsistent, Retryability::after_recreate)));
          return;
        }
        for (const auto& segment : frozen->canonical_segments) if (segment.linear_source)
          record_diagnostic_stage(diagnostics_.get(), run->request.session_id, run->request.operation_id,
              "capture", "source_frozen", "success", {
                {"displayId", std::to_string(segment.display_id.value)},
                {"transfer", "linear"}, {"format", "RGBA32Float"}, {"sourceStorage", "native_texture"},
                {"linearizationPasses", std::to_string(segment.software_linearization_passes)},
                {"sourceBytes", std::to_string(segment.linear_source->byte_count())}});
        complete(run, Result<CaptureReadyPayload, Error>::success(
            CaptureReadyPayload{frozen, receipt, run->windows}));
      });
    });
}

void CaptureSession::State::cancel(const SessionId session_id, const OperationId operation_id) {
  std::shared_ptr<RunState> run;
  Completion completion;
  {
    const std::scoped_lock lock(mutex_);
    if (active_ == nullptr || active_->request.session_id != session_id ||
        active_->request.operation_id != operation_id) {
      return;
    }
    run = std::move(active_);
    completion = std::move(run->completion);
  }
  capture_port_.cancel(session_id, operation_id);
  if (window_owner) window_owner->cancel(session_id, operation_id);
  presenter_.cancel(session_id, operation_id);
  completion(Result<CaptureReadyPayload, Error>::failure(session_error(
      ErrorCode::operation_cancelled, Retryability::never)));
}


void CaptureSession::State::abandon() {
  std::shared_ptr<RunState> run;
  {
    const std::scoped_lock lock(mutex_);
    run = std::move(active_);
  }
  if (run) {
    if (window_owner) window_owner->cancel(run->request.session_id, run->request.operation_id);
    capture_port_.cancel(run->request.session_id, run->request.operation_id);
    presenter_.cancel(run->request.session_id, run->request.operation_id);
  }
}
CaptureSession::CaptureSession(
    DisplayCatalogPort& catalog, CapturePort& capture, PreviewPresenterPort& presenter,
    WindowCatalogPort* windows)
    : CaptureSession(std::shared_ptr<DisplayCatalogPort>(&catalog, [](auto*) {}),
                     std::shared_ptr<CapturePort>(&capture, [](auto*) {}),
                     std::shared_ptr<PreviewPresenterPort>(&presenter, [](auto*) {}),
                     windows ? std::shared_ptr<WindowCatalogPort>(windows, [](auto*) {}) : nullptr) {}
CaptureSession::CaptureSession(
    std::shared_ptr<DisplayCatalogPort> catalog, std::shared_ptr<CapturePort> capture,
    std::shared_ptr<PreviewPresenterPort> presenter, std::shared_ptr<WindowCatalogPort> windows,
    std::shared_ptr<DiagnosticsPort> diagnostics)
    : state_(std::make_shared<State>(std::move(catalog), std::move(capture), std::move(presenter),
                                   std::move(windows), std::move(diagnostics))) {}
CaptureSession::~CaptureSession() { state_->abandon(); }
void CaptureSession::begin(BeginCaptureRequest request, Completion completion) {
  state_->begin(std::move(request), std::move(completion));
}
void CaptureSession::cancel(SessionId session, OperationId operation) {
  state_->cancel(session, operation);
}

}  // namespace hdrshot
