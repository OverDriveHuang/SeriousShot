#pragma once
#include "platform/windows/windows_capture.hpp"
#include "ports/capture_preview_ports.hpp"
#include "ports/magnifier_card_port.hpp"
#include <cstdint>
#include <memory>
namespace hdrshot {
struct WindowsPreviewResourceCounters {
  std::uint64_t source_texture_uploads{}, clean_content_uploads{};
  std::uint64_t background_draws{}, background_presents{}, card_draws{}, card_presents{}, card_ui_uploads{};
};
class WindowsD3DPreviewPresenter final : public PreviewPresenterPort, public MagnifierCardPresenterPort {
 public:
  using Completion = PreviewPresenterPort::Completion;
  static Result<std::shared_ptr<WindowsD3DPreviewPresenter>,Error> create(
      void* hwnd, WindowsDisplayInfo display,
      std::shared_ptr<DiagnosticsPort> diagnostics = {}, void* card_hwnd = nullptr);
  // Native GPU fixture: a null HWND creates an offscreen FP16 surface. This
  // reads the same shader output used by the HWND path, before DWM composition.
  Result<std::vector<std::uint16_t>,Error> render_offscreen(const PresentPreviewRequest& request);
  Result<std::vector<std::uint16_t>,Error> render_card_offscreen(const MagnifierCardFrame& frame);
  // Read-only native regression diagnostics; does not wait for the GPU.
  WindowsPreviewResourceCounters resource_counters() const;
  ~WindowsD3DPreviewPresenter() override;
  void present(const PresentPreviewRequest& request,Completion completion) override;
  void cancel(SessionId session,OperationId operation) override;
  void present_card(MagnifierCardFrame frame,MagnifierCardPresenterPort::Completion completion) override;
  void invalidate_card(std::uint64_t generation) override;
 private:
  struct Impl;
  explicit WindowsD3DPreviewPresenter(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
}  // namespace hdrshot
