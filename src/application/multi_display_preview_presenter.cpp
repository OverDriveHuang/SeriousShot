#include "application/multi_display_preview_presenter.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace hdrshot {
namespace {

Error presenter_error(
    const ErrorCode code,
    std::map<std::string, std::string> context = {}) {
  return Error{
      code,
      "MultiDisplayPreviewPresenterPort",
      Retryability::after_recreate,
      std::move(context)};
}

bool receipt_matches(
    const PresentPreviewRequest& request,
    const PresentReceipt& receipt) {
  return receipt.session_id == request.model.session_id &&
      receipt.operation_id == request.operation_id &&
      request.model.frozen_desktop != nullptr &&
      receipt.frame_id == request.model.frozen_desktop->frame_id &&
      receipt.display_generation == request.model.display_generation &&
      receipt.selection_revision == request.model.selection.revision &&
      receipt.document_revision == request.model.annotation_document.revision &&
      receipt.preview_revision == request.model.initial_highlight.revision;
}

struct FanOutState {
  std::mutex mutex;
  std::size_t remaining{};
  bool completed{};
  std::optional<PresentReceipt> representative_receipt;
  PreviewPresenterPort::Completion completion;
  std::vector<std::shared_ptr<PreviewPresenterPort>> presenters;
  SessionId session_id{};
  OperationId operation_id{};
};

}  // namespace

MultiDisplayPreviewPresenterPort::MultiDisplayPreviewPresenterPort(
    std::vector<DisplayPreviewEndpoint> endpoints)
    : endpoints_(std::move(endpoints)) {}

void MultiDisplayPreviewPresenterPort::present(
    const PresentPreviewRequest& request,
    Completion completion) {
  if (request.target_display_id.value != 0U) {
    const auto endpoint = std::find_if(
        endpoints_.begin(), endpoints_.end(),
        [&request](const DisplayPreviewEndpoint& candidate) {
          return candidate.display_id == request.target_display_id;
        });
    if (endpoint == endpoints_.end() || endpoint->presenter == nullptr) {
      completion(Result<PresentReceipt, Error>::failure(presenter_error(
          ErrorCode::object_not_found,
          {{"displayId", std::to_string(request.target_display_id.value)}})));
      return;
    }
    endpoint->presenter->present(request,
        [request, completion = std::move(completion)](auto result) mutable {
          if (result && !receipt_matches(request, result.value())) {
            completion(Result<PresentReceipt, Error>::failure(presenter_error(
                ErrorCode::invalid_input, {{"reason", "present_receipt_mismatch"}})));
          } else {
            completion(std::move(result));
          }
        });
    return;
  }

  if (endpoints_.empty()) {
    completion(Result<PresentReceipt, Error>::failure(presenter_error(
        ErrorCode::precondition_failed,
        {{"reason", "no_display_presenters"}})));
    return;
  }
  for (const auto& endpoint : endpoints_) {
    if (endpoint.display_id.value == 0U || endpoint.presenter == nullptr) {
      completion(Result<PresentReceipt, Error>::failure(presenter_error(
          ErrorCode::precondition_failed,
          {{"reason", "invalid_display_presenter"}})));
      return;
    }
  }

  auto state = std::make_shared<FanOutState>();
  state->remaining = endpoints_.size();
  state->completion = std::move(completion);
  state->session_id = request.model.session_id;
  state->operation_id = request.operation_id;
  state->presenters.reserve(endpoints_.size());
  for (const auto& endpoint : endpoints_) {
    state->presenters.push_back(endpoint.presenter);
  }
  for (const auto& endpoint : endpoints_) {
    {
      const std::scoped_lock lock(state->mutex);
      if (state->completed) {
        break;
      }
    }
    auto targeted = request;
    targeted.target_display_id = endpoint.display_id;
    endpoint.presenter->present(
        targeted,
        [state, targeted](Result<PresentReceipt, Error> result) mutable {
      PreviewPresenterPort::Completion done;
      std::optional<Result<PresentReceipt, Error>> outcome;
      bool cancel_all = false;
      {
        const std::scoped_lock lock(state->mutex);
        if (state->completed) {
          return;
        }
        if (!result) {
          state->completed = true;
          cancel_all = true;
          done = std::move(state->completion);
          outcome = Result<PresentReceipt, Error>::failure(result.error());
        } else if (!receipt_matches(targeted, result.value())) {
          state->completed = true;
          cancel_all = true;
          done = std::move(state->completion);
          outcome = Result<PresentReceipt, Error>::failure(presenter_error(
              ErrorCode::state_inconsistent,
              {{"reason", "display_receipt_mismatch"}}));
        } else {
          if (!state->representative_receipt.has_value()) {
            state->representative_receipt = result.value();
          }
          --state->remaining;
          if (state->remaining == 0U) {
            state->completed = true;
            done = std::move(state->completion);
            outcome = Result<PresentReceipt, Error>::success(
                *state->representative_receipt);
          }
        }
      }
      if (cancel_all) {
        for (const auto& presenter : state->presenters) {
          presenter->cancel(state->session_id, state->operation_id);
        }
      }
      if (done && outcome.has_value()) {
        done(std::move(*outcome));
      }
    });
  }
}

void MultiDisplayPreviewPresenterPort::cancel(
    const SessionId session_id,
    const OperationId operation_id) {
  for (const auto& endpoint : endpoints_) {
    if (endpoint.presenter != nullptr) {
      endpoint.presenter->cancel(session_id, operation_id);
    }
  }
}

}  // namespace hdrshot
