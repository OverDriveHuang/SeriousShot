#pragma once

#include "core/error.hpp"
#include "core/frame.hpp"
#include "core/result.hpp"

#include <array>
#include <cstdint>

namespace hdrshot {

class ExtendedP3Mapper {
 public:
  [[nodiscard]] static Result<float, Error> decode_binary16(std::uint16_t bits);
  [[nodiscard]] static std::uint16_t encode_binary16(float value) noexcept;
  [[nodiscard]] static float inverse_extended_srgb(float encoded) noexcept;
  // Source transfer is explicit; UI/annotation color conversion is independent.
  [[nodiscard]] static float source_linear(float value, TransferFunction transfer) noexcept {
    return transfer == TransferFunction::linear ? value : inverse_extended_srgb(value);
  }
  [[nodiscard]] static float encode_extended_srgb(float linear) noexcept;
  [[nodiscard]] static std::uint16_t quantize_unorm16(float encoded) noexcept;
  [[nodiscard]] static std::array<float, 3> annotation_linear_display_p3(
      std::uint32_t srgb_rgb) noexcept;
  [[nodiscard]] static std::array<std::uint16_t, 3> annotation_display_p3_u16(
      std::uint32_t srgb_rgb) noexcept;
};

}  // namespace hdrshot
