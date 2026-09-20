#pragma once

#include <cstdint>
#include <span>

namespace hdrshot {

class Bt2020SrgbIccProfile {
 public:
  [[nodiscard]] static std::span<const std::uint8_t> bytes();
};

}  // namespace hdrshot
