#pragma once

#include <cstdint>
#include <span>

namespace hdrshot {

class DisplayP3IccProfile {
 public:
  [[nodiscard]] static std::span<const std::uint8_t> bytes();
};

}  // namespace hdrshot
