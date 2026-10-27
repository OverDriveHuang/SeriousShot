#pragma once

#include "ports/capture_preview_ports.hpp"
#include "ports/capture_progress_port.hpp"

#include <memory>
#include <vector>

namespace hdrshot {

struct DisplayPreviewEndpoint {
  DisplayId display_id{};
  std::shared_ptr<PreviewPresenterPort> presenter;
};

class MultiDisplayPreviewPresenterPort final : public PreviewPresenterPort {
 public:
  explicit MultiDisplayPreviewPresenterPort(std::vector<DisplayPreviewEndpoint> endpoints,
      std::shared_ptr<CaptureRuntimePort> runtime = {});

  void present(const PresentPreviewRequest& request, Completion completion) override;
  void cancel(SessionId session_id, OperationId operation_id) override;

 private:
  std::vector<DisplayPreviewEndpoint> endpoints_;
  std::shared_ptr<CaptureRuntimePort> runtime_;
};

}  // namespace hdrshot
