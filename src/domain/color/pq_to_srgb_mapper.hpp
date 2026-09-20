#pragma once

#include "core/error.hpp"
#include "core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hdrshot {

struct SrgbTransferMapResult {
  std::vector<std::uint16_t> rgba_bt2020_srgb_u16;
  std::size_t clipped_pixel_count{};
  std::size_t clipped_channel_count{};
};

class PqToSrgbTransferMapper {
 public:
  [[nodiscard]] static Result<SrgbTransferMapResult, Error> map_rgba16(
      std::span<const std::uint16_t> rgba_pq_bt2020_u16,
      double pq_diffuse_white_nits);
};

}  // namespace hdrshot
