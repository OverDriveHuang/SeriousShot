#include "application/multi_display_preview_presenter.hpp"
#include "test_support.hpp"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

using namespace hdrshot;

FrozenDesktopRef two_display_frame() {
  const auto segment = [](const DisplayId id, const double x) {
    return CanonicalFrameSegment{
        id,
        LogicalRect{x, 0.0, 2.0, 2.0},
        1.0,
        PixelSize{2, 2},
        PixelFormat::rgba16_float,
        ColorEncoding{ColorPrimaries::bt2020, TransferFunction::pq,
                      AlphaMode::straight, 100.0},
        DisplayDynamicRange::sdr,
        std::vector<std::uint16_t>(16U, 0x3800),
    };
  };
  return std::make_shared<const FrozenDesktop>(FrozenDesktop{
      FrameId{9},
      4,
      LogicalRect{0.0, 0.0, 4.0, 2.0},
      {segment(DisplayId{1}, 0.0), segment(DisplayId{2}, 2.0)},
  });
}

PresentPreviewRequest request(const DisplayId target = {}) {
  const auto frame = two_display_frame();
  return PresentPreviewRequest{
      OperationId{3},
      PreviewModel{
          SessionId{2},
          frame,
          SelectionSnapshot{5, PixelRect{0, 0, 2, 2}},
          AnnotationDocumentSnapshot{},
          OverlayStyle{},
          frame->display_generation,
      },
      target,
  };
}

class FakeDisplayPresenter final : public PreviewPresenterPort {
 public:
  void present(const PresentPreviewRequest& value, Completion completion) override {
    requests.push_back(value);
    if (fail) {
      completion(Result<PresentReceipt, Error>::failure(Error{
          ErrorCode::presenter_failed,
          "FakeDisplayPresenter",
          Retryability::after_recreate,
          {},
      }));
      return;
    }
    completion(Result<PresentReceipt, Error>::success(PresentReceipt{
        value.model.session_id,
        value.operation_id,
        value.model.frozen_desktop->frame_id,
        value.model.display_generation,
        value.model.selection.revision,
        value.model.annotation_document.revision,
        wrong_preview_revision ? 0 : value.model.initial_highlight.revision,
    }));
  }

  void cancel(const SessionId session_id, const OperationId operation_id) override {
    cancellations.emplace_back(session_id, operation_id);
  }

  std::vector<PresentPreviewRequest> requests;
  std::vector<std::pair<SessionId, OperationId>> cancellations;
  bool fail{};
  bool wrong_preview_revision{};
};

void fanout_targets_every_display_and_completes_once() {
  auto first = std::make_shared<FakeDisplayPresenter>();
  auto second = std::make_shared<FakeDisplayPresenter>();
  MultiDisplayPreviewPresenterPort presenter({
      {DisplayId{1}, first},
      {DisplayId{2}, second},
  });
  std::optional<Result<PresentReceipt, Error>> completion;
  std::size_t completion_count = 0;
  presenter.present(request(), [&](auto result) {
    ++completion_count;
    completion = std::move(result);
  });
  HDRSHOT_CHECK(completion_count == 1U);
  HDRSHOT_CHECK(completion.has_value() && completion->has_value());
  HDRSHOT_CHECK(first->requests.size() == 1U);
  HDRSHOT_CHECK(second->requests.size() == 1U);
  HDRSHOT_CHECK(first->requests.front().target_display_id == DisplayId{1});
  HDRSHOT_CHECK(second->requests.front().target_display_id == DisplayId{2});
}

void exact_target_routes_only_one_surface_and_cancel_fans_out() {
  auto first = std::make_shared<FakeDisplayPresenter>();
  auto second = std::make_shared<FakeDisplayPresenter>();
  MultiDisplayPreviewPresenterPort presenter({
      {DisplayId{1}, first},
      {DisplayId{2}, second},
  });
  std::optional<Result<PresentReceipt, Error>> completion;
  presenter.present(request(DisplayId{2}), [&](auto result) {
    completion = std::move(result);
  });
  HDRSHOT_CHECK(completion.has_value() && completion->has_value());
  HDRSHOT_CHECK(first->requests.empty());
  HDRSHOT_CHECK(second->requests.size() == 1U);
  presenter.cancel(SessionId{2}, OperationId{3});
  HDRSHOT_CHECK(first->cancellations.size() == 1U);
  HDRSHOT_CHECK(second->cancellations.size() == 1U);
}

void fanout_failure_completes_once_cancels_all_and_stops_new_work() {
  auto first = std::make_shared<FakeDisplayPresenter>();
  auto second = std::make_shared<FakeDisplayPresenter>();
  first->fail = true;
  MultiDisplayPreviewPresenterPort presenter({
      {DisplayId{1}, first},
      {DisplayId{2}, second},
  });
  std::size_t completion_count = 0;
  std::optional<Result<PresentReceipt, Error>> completion;
  presenter.present(request(), [&](auto result) {
    ++completion_count;
    completion = std::move(result);
  });
  HDRSHOT_CHECK(completion_count == 1U);
  HDRSHOT_CHECK(completion.has_value() && !completion->has_value());
  HDRSHOT_CHECK(first->requests.size() == 1U);
  HDRSHOT_CHECK(second->requests.empty());
  HDRSHOT_CHECK(first->cancellations.size() == 1U);
  HDRSHOT_CHECK(second->cancellations.size() == 1U);
}

void candidate_revision_is_part_of_receipt_identity() {
  for (bool targeted : {false, true}) for (bool wrong : {false, true}) {
    auto endpoint = std::make_shared<FakeDisplayPresenter>();
    endpoint->wrong_preview_revision = wrong;
    MultiDisplayPreviewPresenterPort presenter({{{1}, endpoint}});
    auto value = request();
    if (targeted) value.target_display_id = {1};
    value.model.selection.desktop_rect = {};
    value.model.initial_highlight = {PreviewHighlightKind::window_candidate, {0, 0, 2, 2}, 42};
    std::optional<Result<PresentReceipt, Error>> result;
    presenter.present(value, [&](auto completion) { result = std::move(completion); });
    HDRSHOT_CHECK(result && result->has_value() == !wrong);
    if (!wrong) HDRSHOT_CHECK(result->value().preview_revision == 42);
  }
}
}  // namespace

int main() {
  return hdrshot::test::run({
      {"multi-display presenter fans out", fanout_targets_every_display_and_completes_once},
      {"candidate-only revision is verified", candidate_revision_is_part_of_receipt_identity},
      {"multi-display presenter routes exact target", exact_target_routes_only_one_surface_and_cancel_fans_out},
      {"multi-display presenter cancels aggregate failure", fanout_failure_completes_once_cancels_all_and_stops_new_work},
  });
}
