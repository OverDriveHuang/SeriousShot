#pragma once

#include "core/error.hpp"
#include "core/geometry.hpp"
#include "core/ids.hpp"
#include "core/result.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace hdrshot {

struct TextRasterRequest {
  ObjectId object_id{};
  std::string utf8_text;
  PixelSize mask_size_px{};
  std::uint16_t font_size_pt{};
  double pixels_per_point{1.0};
};

struct TextCoverageMask {
  ObjectId object_id{};
  PixelSize size_px{};
  std::vector<std::uint8_t> coverage_u8;
  std::string font_identity;
};

struct TextMeasureRequest {
  std::string utf8_text;
  std::uint16_t font_size_pt{};
  double pixels_per_point{1.0};
};

struct TextMetrics {
  PixelSize minimum_mask_size_px{};
  std::string font_identity;
};

class TextRasterizerPort {
 public:
  virtual ~TextRasterizerPort() = default;
  [[nodiscard]] virtual Result<TextMetrics, Error> measure(
      const TextMeasureRequest&) {
    return Result<TextMetrics, Error>::failure(Error{
        ErrorCode::unsupported_encoding,
        "TextRasterizerPort",
        Retryability::never,
        {{"reason", "text_measurement_not_supported"}},
    });
  }
  [[nodiscard]] virtual Result<TextCoverageMask, Error> rasterize(
      const TextRasterRequest& request) = 0;
};

}  // namespace hdrshot
