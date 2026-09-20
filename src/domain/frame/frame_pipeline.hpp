#pragma once

#include "core/error.hpp"
#include "core/frame.hpp"
#include "core/result.hpp"

#include <vector>

namespace hdrshot {

class SourceColorInterpreter {
 public:
  [[nodiscard]] static Result<CanonicalFrameSegment, Error> interpret(
      NativeCaptureFrame frame,
      const DisplaySnapshot& display);
};

class DisplayFrameAssembler {
 public:
  [[nodiscard]] static Result<FrozenDesktop, Error> assemble(
      FrameId frame_id,
      DisplayGeneration generation,
      const DisplaySnapshotSet& displays,
      std::vector<CanonicalFrameSegment> segments);
};

}  // namespace hdrshot
