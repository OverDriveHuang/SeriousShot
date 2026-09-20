#pragma once

#include "ports/export_pixel_processor_port.hpp"
#include "ports/ultra_hdr_ports.hpp"
#include "ports/clean_composition_port.hpp"
#include "ports/source_range_probe_port.hpp"

#include <memory>

namespace hdrshot {

class MacMetalExportPixelProcessor final : public ExportPixelProcessorPort,
                                           public UltraHdrInputRendererPort,
                                           public CleanCompositionPort,
                                           public SourceRangeProbePort {
 public:
  [[nodiscard]] static Result<std::unique_ptr<MacMetalExportPixelProcessor>, Error> create();

  ~MacMetalExportPixelProcessor() override;
  MacMetalExportPixelProcessor(MacMetalExportPixelProcessor&&) noexcept;
  MacMetalExportPixelProcessor& operator=(MacMetalExportPixelProcessor&&) noexcept;
  MacMetalExportPixelProcessor(const MacMetalExportPixelProcessor&) = delete;
  MacMetalExportPixelProcessor& operator=(const MacMetalExportPixelProcessor&) = delete;

  [[nodiscard]] Result<ExportPixelProcessResult, Error> process(
      const ExportPixelProcessRequest& request) override;
  [[nodiscard]] Result<LinearDisplayP3HalfImage, Error> render(
      const UltraHdrInputRenderRequest& request) override;

  Result<std::vector<std::array<float, 4>>, Error> compose(
      std::span<const CleanCompositionSample> samples) override;
  Result<LinearSampleRef, Error> compose_native(LinearSourceRef source,
      std::span<const PositionedCleanSample> samples) override;
  Result<RangeFitResult, Error> probe(const SelectionRoiView& source,
      const AnnotationPixelPlan& plan, RangeProbeOptimization optimization) override;

 private:
  struct Impl;
  explicit MacMetalExportPixelProcessor(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace hdrshot
