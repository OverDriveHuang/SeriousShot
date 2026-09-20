#pragma once
#include "ports/analysis_port.hpp"
#include <chrono>
#include <functional>
#include <memory>

namespace hdrshot {
// The application owns ordering, latest-request policy and lifetime. Platform
// executors remain synchronous and serialized; UI adapters marshal completions.
class AnalysisSession final {
public:
  using Completion = std::function<void(Result<analysis::ResultRef, Error>)>;
  using ReportCompletion = std::function<void(Result<LinearSourceRef, Error>)>;
  AnalysisSession(analysis::Input input,
                  std::shared_ptr<AnalysisPort> executor);
  ~AnalysisSession();
  AnalysisSession(const AnalysisSession &) = delete;
  AnalysisSession &operator=(const AnalysisSession &) = delete;
  bool request(analysis::Request request, Completion completion);
  bool report(analysis::ReportPlan plan, ReportCompletion completion);
  // Nonblocking close: finishes the current native call, discards pending work
  // and does not deliver callbacks into the closed window.
  void stop();
  bool wait_for_stopped(std::chrono::milliseconds timeout) const;

private:
  struct State;
  std::shared_ptr<State> state_;
};
} // namespace hdrshot
