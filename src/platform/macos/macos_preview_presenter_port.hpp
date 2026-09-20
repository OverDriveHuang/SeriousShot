#pragma once

#include "platform/macos/metal_edr_presenter.hpp"
#include "ports/capture_preview_ports.hpp"

#include <memory>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <optional>
#include <set>
#include <thread>
#include <utility>

namespace hdrshot {

class MacPreviewPresenterPort final : public PreviewPresenterPort {
 public:
  using RenderBackend = std::function<Result<MacMetalPresentReceipt, Error>(
      void*, const MacMetalOverlayRequest&)>;

  MacPreviewPresenterPort(
      std::shared_ptr<MacMetalEdrPresenter> presenter,
      void* native_metal_layer,
      double target_current_maximum_edr);
  MacPreviewPresenterPort(
      RenderBackend render_backend,
      void* native_metal_layer,
      double target_current_maximum_edr);
  ~MacPreviewPresenterPort() override;

  void set_native_metal_layer(void* native_metal_layer);

  void present(const PresentPreviewRequest& request, Completion completion) override;
  void cancel(SessionId session_id, OperationId operation_id) override;

 private:
  struct WorkItem {
    PresentPreviewRequest request;
    Completion completion;
  };

  void worker_loop();
  void render(WorkItem work);

  std::shared_ptr<MacMetalEdrPresenter> presenter_;
  RenderBackend render_backend_;
  void* native_metal_layer_{};
  MacMetalSurfaceRange target_surface_range_{MacMetalSurfaceRange::sdr};
  std::mutex mutex_;
  std::condition_variable work_available_;
  std::optional<WorkItem> latest_pending_;
  bool stopping_{};
  std::thread worker_;
  std::set<std::pair<std::uint64_t, std::uint64_t>> cancelled_;
};

}  // namespace hdrshot
