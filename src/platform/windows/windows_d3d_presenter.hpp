#pragma once
#include "platform/windows/windows_capture.hpp"
#include "ports/capture_preview_ports.hpp"
#include <memory>
namespace hdrshot {
class WindowsD3DPreviewPresenter final : public PreviewPresenterPort {
 public:
  static Result<std::shared_ptr<WindowsD3DPreviewPresenter>,Error> create(void* hwnd,WindowsDisplayInfo display);
  // Native GPU fixture: a null HWND creates an offscreen FP16 surface. This
  // reads the same shader output used by the HWND path, before DWM composition.
  Result<std::vector<std::uint16_t>,Error> render_offscreen(const PresentPreviewRequest& request);
  ~WindowsD3DPreviewPresenter() override;
  void present(const PresentPreviewRequest& request,Completion completion) override;
  void cancel(SessionId session,OperationId operation) override;
 private:
  struct Impl;
  explicit WindowsD3DPreviewPresenter(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
}  // namespace hdrshot
