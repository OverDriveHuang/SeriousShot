#include "application/analysis_session.hpp"
#include "domain/analysis/engine.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace hdrshot {
namespace {
Error worker_error() {
  return {ErrorCode::state_inconsistent,
          "AnalysisSession",
          Retryability::after_recreate,
          {{"reason", "executor_exception"}}};
}
} // namespace
struct AnalysisSession::State {
  analysis::Input input;
  std::shared_ptr<AnalysisPort> executor;
  struct Pending {
    analysis::Request request;
    Completion completion;
  };
  struct Report {
    analysis::ReportPlan plan;
    ReportCompletion completion;
  };
  mutable std::mutex mutex;
  mutable std::condition_variable wake, ended;
  std::optional<Pending> pending;
  std::deque<Report> reports;
  std::uint64_t newest_revision{};
  std::optional<analysis::Request> newest_request;
  bool stopping{}, finished{};
};
AnalysisSession::AnalysisSession(analysis::Input input,
                                 std::shared_ptr<AnalysisPort> executor)
    : state_(std::make_shared<State>()) {
  state_->input = std::move(input);
  state_->executor = std::move(executor);
  // Only State is captured, never this or a QWidget. No unbounded history, and
  // no UI-thread join while a GPU command is still in flight.
  std::thread([state = state_] {
    for (;;) {
      std::optional<State::Pending> pending;
      std::optional<State::Report> report;
      {
        std::unique_lock lock(state->mutex);
        state->wake.wait(lock, [&] {
          return state->stopping || state->pending || !state->reports.empty();
        });
        if (state->stopping)
          break;
        // Accepted reports are not latest-wins; preserve their FIFO ordering.
        if (!state->reports.empty()) {
          report = std::move(state->reports.front());
          state->reports.pop_front();
        } else {
          pending = std::move(state->pending);
          state->pending.reset();
        }
      }
      if (pending) {
        auto result =
            Result<analysis::ResultRef, Error>::failure(worker_error());
        try {
          if (state->executor)
            result = state->executor->analyze(state->input, pending->request);
        } catch (
            ...) { /* stage-labelled failure; no pixel data in diagnostics */
        }
        bool deliver;
        {
          std::scoped_lock lock(state->mutex);
          deliver = !state->stopping &&
                    (pending->request.revision == state->newest_revision ||
                     (state->newest_request && analysis::same_statistics_request(
                         pending->request, *state->newest_request) &&
                      analysis::same_detail_request(pending->request, *state->newest_request)));
        }
        if (deliver && pending->completion) {
          try {
            pending->completion(std::move(result));
          } catch (...) {
          }
        }
      } else if (report) {
        auto result = Result<LinearSourceRef, Error>::failure(worker_error());
        try {
          if (state->executor)
            result =
                state->executor->compose_report(state->input, report->plan);
        } catch (...) {
        }
        bool deliver;
        {
          std::scoped_lock lock(state->mutex);
          deliver = !state->stopping;
        }
        if (deliver && report->completion) {
          try {
            report->completion(std::move(result));
          } catch (...) {
          }
        }
      }
    }
    // Release potentially large native resources before notifying
    // shutdown/tests.
    state->executor.reset();
    state->input.source.reset();
    {
      std::scoped_lock lock(state->mutex);
      state->pending.reset();
      state->reports.clear();
      state->finished = true;
    }
    state->ended.notify_all();
  }).detach();
}
AnalysisSession::~AnalysisSession() { stop(); }
bool AnalysisSession::request(analysis::Request request,
                              Completion completion) {
  {
    std::scoped_lock lock(state_->mutex);
    if (state_->stopping || request.revision < state_->newest_revision)
      return false;
    state_->newest_revision = request.revision;
    state_->newest_request = request;
    state_->pending = State::Pending{std::move(request), std::move(completion)};
  }
  state_->wake.notify_one();
  return true;
}
bool AnalysisSession::report(analysis::ReportPlan plan,
                             ReportCompletion completion) {
  {
    std::scoped_lock lock(state_->mutex);
    if (state_->stopping || state_->reports.size() >= 8)
      return false;
    state_->reports.push_back({std::move(plan), std::move(completion)});
  }
  state_->wake.notify_one();
  return true;
}
void AnalysisSession::stop() {
  {
    std::scoped_lock lock(state_->mutex);
    state_->stopping = true;
    state_->pending.reset();
    state_->reports.clear();
  }
  state_->wake.notify_all();
}
bool AnalysisSession::wait_for_stopped(
    std::chrono::milliseconds timeout) const {
  std::unique_lock lock(state_->mutex);
  return state_->ended.wait_for(lock, timeout,
                                [&] { return state_->finished; });
}
} // namespace hdrshot
