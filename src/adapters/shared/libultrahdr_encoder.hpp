#pragma once

#include "ports/ultra_hdr_ports.hpp"

namespace hdrshot {

class LibUltraHdrEncoder final : public UltraHdrEncoderPort {
 public:
  [[nodiscard]] Result<EncodedUltraHdrJpeg, Error> encode(
      const UltraHdrEncodeRequest& request) override;
};

}  // namespace hdrshot
