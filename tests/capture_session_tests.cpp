#include "application/capture_session.hpp"
#include "test_support.hpp"

#include <optional>
#include <utility>
#include <vector>

namespace {

using namespace hdrshot;

Error fake_error(const ErrorCode code, const char* module) {
  return Error{code, module, Retryability::same_input, {}};
}

DisplaySnapshotSet display_set(const DisplayGeneration generation = 5) {
  return DisplaySnapshotSet{
      generation,
      {DisplaySnapshot{DisplayId{7}, LogicalRect{0.0, 0.0, 2.0, 2.0}, 1.0, {2, 2}}},
  };
}

NativeFrameBatch native_batch(
    const SessionId session,
    const OperationId operation,
    const DisplayGeneration generation = 5) {
  return NativeFrameBatch{
      session,
      operation,
      generation,
      {NativeCaptureFrame{
          DisplayId{7},
          {2, 2},
          PixelFormat::rgba16_float,
          ColorEncoding{
              ColorPrimaries::bt2020,
              TransferFunction::pq,
              AlphaMode::straight,
              100.0,
          },
          std::vector<std::uint16_t>(16, 0x3800),
      }},
  };
}

class FakeCatalog final : public DisplayCatalogPort {
 public:
  DisplaySnapshotSet snapshot{display_set()};
  std::optional<Error> failure;
  std::optional<SnapshotDisplaysRequest> last_request;

  void snapshot_displays(const SnapshotDisplaysRequest& request, Completion completion) override {
    last_request = request;
    if (failure.has_value()) {
      completion(Result<DisplaySnapshotSet, Error>::failure(*failure));
    } else {
      completion(Result<DisplaySnapshotSet, Error>::success(snapshot));
    }
  }
};

class FakeCapture final : public CapturePort {
 public:
  bool hold{};
  std::optional<Error> failure;
  std::optional<CaptureBatchRequest> last_request;
  Completion held_completion;
  bool cancel_called{};

  void capture(const CaptureBatchRequest& request, Completion completion) override {
    last_request = request;
    if (hold) {
      held_completion = std::move(completion);
      return;
    }
    if (failure.has_value()) {
      completion(Result<NativeFrameBatch, Error>::failure(*failure));
    } else {
      completion(Result<NativeFrameBatch, Error>::success(native_batch(
          request.session_id, request.operation_id, request.display_generation)));
    }
  }

  void cancel(SessionId, OperationId) override { cancel_called = true; }
};

class FakePresenter final : public PreviewPresenterPort {
 public:
  std::optional<PresentPreviewRequest> last_request;
  std::optional<Error> failure;
  bool cancel_called{};

  void present(const PresentPreviewRequest& request, Completion completion) override {
    last_request = request;
    if (failure.has_value()) {
      completion(Result<PresentReceipt, Error>::failure(*failure));
      return;
    }
    const auto& model = request.model;
    completion(Result<PresentReceipt, Error>::success(PresentReceipt{
        model.session_id,
        request.operation_id,
        model.frozen_desktop->frame_id,
        model.display_generation,
        model.selection.revision,
        model.annotation_document.revision,
        model.initial_highlight.revision,
    }));
  }

  void cancel(SessionId, OperationId) override { cancel_called = true; }
};

BeginCaptureRequest request() {
  return BeginCaptureRequest{
      SessionId{1},
      OperationId{2},
      FrameId{3},
      {DisplayId{7}},
      SelectionSnapshot{4, PixelRect{0, 0, 2, 2}},
  };
}

void happy_path_preserves_envelope_and_frame_truth() {
  FakeCatalog catalog;
  FakeCapture capture;
  FakePresenter presenter;
  CaptureSession session(catalog, capture, presenter);
  std::optional<Result<CaptureReadyPayload, Error>> completion;
  session.begin(request(), [&](auto result) { completion = std::move(result); });

  HDRSHOT_CHECK(completion.has_value());
  HDRSHOT_CHECK(completion->has_value());
  HDRSHOT_CHECK(catalog.last_request->session_id == SessionId{1});
  HDRSHOT_CHECK(capture.last_request->display_generation == 5);
  HDRSHOT_CHECK(capture.last_request->exclude_own_windows);
  HDRSHOT_CHECK(presenter.last_request->model.frozen_desktop->frame_id == FrameId{3});
  HDRSHOT_CHECK(presenter.last_request->model.frozen_desktop->canonical_segments.size() == 1);
  HDRSHOT_CHECK(completion->value().present_receipt.selection_revision == 4);
}

void capture_failure_does_not_present() {
  FakeCatalog catalog;
  FakeCapture capture;
  capture.failure = fake_error(ErrorCode::permission_denied, "FakeCapture");
  FakePresenter presenter;
  CaptureSession session(catalog, capture, presenter);
  std::optional<Result<CaptureReadyPayload, Error>> completion;
  session.begin(request(), [&](auto result) { completion = std::move(result); });
  HDRSHOT_CHECK(completion.has_value());
  HDRSHOT_CHECK(!completion->has_value());
  HDRSHOT_CHECK(completion->error().code == ErrorCode::permission_denied);
  HDRSHOT_CHECK(!presenter.last_request.has_value());
}

void generation_mismatch_is_rejected_before_present() {
  FakeCatalog catalog;
  FakeCapture capture;
  capture.hold = true;
  FakePresenter presenter;
  CaptureSession session(catalog, capture, presenter);
  std::optional<Result<CaptureReadyPayload, Error>> completion;
  session.begin(request(), [&](auto result) { completion = std::move(result); });
  capture.held_completion(Result<NativeFrameBatch, Error>::success(
      native_batch(SessionId{1}, OperationId{2}, 6)));
  HDRSHOT_CHECK(completion.has_value());
  HDRSHOT_CHECK(!completion->has_value());
  HDRSHOT_CHECK(completion->error().code == ErrorCode::display_configuration_changed);
  HDRSHOT_CHECK(!presenter.last_request.has_value());
}

void cancel_reaches_ports_and_ignores_late_capture() {
  FakeCatalog catalog;
  FakeCapture capture;
  capture.hold = true;
  FakePresenter presenter;
  CaptureSession session(catalog, capture, presenter);
  std::optional<Result<CaptureReadyPayload, Error>> completion;
  session.begin(request(), [&](auto result) { completion = std::move(result); });
  session.cancel(SessionId{1}, OperationId{2});
  HDRSHOT_CHECK(completion.has_value());
  HDRSHOT_CHECK(!completion->has_value());
  HDRSHOT_CHECK(completion->error().code == ErrorCode::operation_cancelled);
  HDRSHOT_CHECK(capture.cancel_called);
  HDRSHOT_CHECK(presenter.cancel_called);

  capture.held_completion(Result<NativeFrameBatch, Error>::success(
      native_batch(SessionId{1}, OperationId{2})));
  HDRSHOT_CHECK(!presenter.last_request.has_value());
}

void destroyed_owner_ignores_delayed_capture_and_catalog() {
  FakeCatalog catalog;
  FakeCapture capture;
  FakePresenter presenter;
  capture.hold = true;
  int callbacks = 0;
  {
    CaptureSession session(catalog, capture, presenter);
    session.begin(request(), [&](auto) { ++callbacks; });
  }
  HDRSHOT_CHECK(capture.cancel_called);
  capture.held_completion(Result<NativeFrameBatch, Error>::success(
      native_batch(SessionId{1}, OperationId{2})));
  HDRSHOT_CHECK(callbacks == 0);
  HDRSHOT_CHECK(!presenter.last_request);

  class DelayedCatalog final : public DisplayCatalogPort {
   public:
    Completion held;
    void snapshot_displays(const SnapshotDisplaysRequest&, Completion next) override {
      held = std::move(next);
    }
  } delayed;
  capture.last_request.reset();
  {
    CaptureSession session(delayed, capture, presenter);
    session.begin(request(), [&](auto) { ++callbacks; });
  }
  delayed.held(Result<DisplaySnapshotSet, Error>::success(display_set()));
  HDRSHOT_CHECK(callbacks == 0);
  HDRSHOT_CHECK(!capture.last_request);
}

class FakeWindows final : public WindowCatalogPort {
 public:
  Completion held;
  SnapshotWindowsRequest last;
  bool cancelled{};
  void snapshot_windows(const SnapshotWindowsRequest& value, Completion done) override {
    last = value; held = std::move(done);
  }
  void cancel(SessionId, OperationId) override { cancelled = true; }
};

void windows_are_frozen_before_capture_and_failure_is_manual_fallback() {
  for (int outcome = 0; outcome < 3; ++outcome) {
    FakeCatalog catalog; FakeCapture capture; FakePresenter presenter; FakeWindows windows;
    CaptureSession session(catalog, capture, presenter, &windows);
    std::optional<Result<CaptureReadyPayload, Error>> completed;
    session.begin(request(), [&](auto value) { completed = std::move(value); });
    HDRSHOT_CHECK(!capture.last_request && !completed);
    HDRSHOT_CHECK(windows.last.displays.generation == 5);
    WindowSnapshot snapshot{{1}, {2}, outcome == 2 ? 99U : 5U,
        {{7, {7}, {0, 0, 2, 2}, 0}, {8, {7}, {0, 0, 100, 100}, 1}}};
    if (outcome == 1) windows.held(Result<WindowSnapshot, Error>::failure(
        fake_error(ErrorCode::precondition_failed, "fake windows")));
    else windows.held(Result<WindowSnapshot, Error>::success(snapshot));
    HDRSHOT_CHECK(completed && completed->has_value());
    const auto& payload = completed->value();
    HDRSHOT_CHECK(payload.window_snapshot && payload.window_snapshot->display_generation == 5);
    HDRSHOT_CHECK(payload.window_snapshot->candidates.size() == (outcome == 0 ? 1U : 0U));
    HDRSHOT_CHECK(payload.frozen_desktop->canonical_segments.front().rgba_half ==
        native_batch({1}, {2}).frames.front().rgba_half);
    HDRSHOT_CHECK(presenter.last_request->model.initial_highlight.rect.empty());
  }
}

void cancelled_or_destroyed_window_query_cannot_start_capture() {
  for (bool destroy : {false, true}) {
    FakeCatalog catalog; FakeCapture capture; FakePresenter presenter; FakeWindows windows;
    int calls = 0;
    auto session = std::make_unique<CaptureSession>(catalog, capture, presenter, &windows);
    session->begin(request(), [&](auto result) {
      ++calls;
      HDRSHOT_CHECK(!result && result.error().code == ErrorCode::operation_cancelled);
    });
    if (destroy) session.reset(); else session->cancel({1}, {2});
    HDRSHOT_CHECK(windows.cancelled);
    windows.held(Result<WindowSnapshot, Error>::success({{1}, {2}, 5, {}}));
    HDRSHOT_CHECK(!capture.last_request && !presenter.last_request);
    HDRSHOT_CHECK(calls == (destroy ? 0 : 1));
  }
}

}  // namespace

int main() {
  using hdrshot::test::TestCase;
  return hdrshot::test::run(std::vector<TestCase>{
      {"capture session happy path", happy_path_preserves_envelope_and_frame_truth},
      {"window snapshot before capture and safe fallback", windows_are_frozen_before_capture_and_failure_is_manual_fallback},
      {"late window query cannot revive a capture", cancelled_or_destroyed_window_query_cannot_start_capture},
      {"destroyed capture owner rejects delayed callbacks", destroyed_owner_ignores_delayed_capture_and_catalog},
      {"capture failure does not present", capture_failure_does_not_present},
      {"generation mismatch rejected", generation_mismatch_is_rejected_before_present},
      {"cancel reaches ports and ignores late result", cancel_reaches_ports_and_ignores_late_capture},
  });
}
