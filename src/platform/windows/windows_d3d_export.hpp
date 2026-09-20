#pragma once
#include "ports/export_pixel_processor_port.hpp"
#include "ports/ultra_hdr_ports.hpp"
#include <memory>
namespace hdrshot {
class WindowsD3DExportPixelProcessor final : public ExportPixelProcessorPort, public UltraHdrInputRendererPort {
 public:
  static Result<std::unique_ptr<WindowsD3DExportPixelProcessor>,Error> create();
  ~WindowsD3DExportPixelProcessor() override;
  Result<ExportPixelProcessResult,Error> process(const ExportPixelProcessRequest& request) override;
  Result<LinearDisplayP3HalfImage,Error> render(const UltraHdrInputRenderRequest& request) override;
 private:
  struct Impl;
  explicit WindowsD3DExportPixelProcessor(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
}  // namespace hdrshot
