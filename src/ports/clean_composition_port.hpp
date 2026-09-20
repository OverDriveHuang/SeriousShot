#pragma once
#include "domain/annotation/annotation_render_plan.hpp"
#include "domain/frame/frame_cropper.hpp"
#include <memory>
#include <span>

namespace hdrshot {
// Platform-neutral sparse work. The executor never owns UI or decides coverage.
struct CleanCompositionSample {
  std::array<float, 4> annotation; // premultiplied RGB + remaining source weight
  std::array<float, 4> source;     // linear P3; only read for partial coverage
};
class CleanCompositionPort {
 public:
  virtual ~CleanCompositionPort() = default;
  virtual Result<std::vector<std::array<float, 4>>, Error> compose(
      std::span<const CleanCompositionSample> samples) = 0;
  virtual Result<LinearSampleRef, Error> compose_native(
      LinearSourceRef, std::span<const PositionedCleanSample>) {
    return Result<LinearSampleRef, Error>::failure({ErrorCode::unsupported_encoding,
        "CleanCompositionPort", Retryability::never, {{"reason", "native_composition_unavailable"}}});
  }
};
class CpuCleanComposition final : public CleanCompositionPort {
 public:
  Result<std::vector<std::array<float, 4>>, Error> compose(
      std::span<const CleanCompositionSample> samples) override;
};
} // namespace hdrshot
