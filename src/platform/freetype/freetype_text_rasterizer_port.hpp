#pragma once

#include "ports/text_rasterizer_port.hpp"

#include <memory>
#include <string>

namespace hdrshot {

class FreeTypeTextRasterizerPort final : public TextRasterizerPort {
 public:
  FreeTypeTextRasterizerPort(std::string font_path, std::string font_identity);
  ~FreeTypeTextRasterizerPort() override;

  FreeTypeTextRasterizerPort(const FreeTypeTextRasterizerPort&) = delete;
  FreeTypeTextRasterizerPort& operator=(const FreeTypeTextRasterizerPort&) = delete;

  [[nodiscard]] Result<TextMetrics, Error> measure(
      const TextMeasureRequest& request) override;

  [[nodiscard]] Result<TextCoverageMask, Error> rasterize(
      const TextRasterRequest& request) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hdrshot
