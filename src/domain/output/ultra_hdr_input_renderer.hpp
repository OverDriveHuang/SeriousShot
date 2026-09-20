#pragma once

#include "ports/ultra_hdr_ports.hpp"

namespace hdrshot {

class CpuUltraHdrInputRenderer final : public UltraHdrInputRendererPort {
 public:
  [[nodiscard]] Result<LinearDisplayP3HalfImage, Error> render(
      const UltraHdrInputRenderRequest& request) override;
};

}  // namespace hdrshot
