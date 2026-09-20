#pragma once

// Identical inputs for shared PNG and macOS Image I/O diagnostic benchmarks.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace hdrshot::benchmark {

struct CorpusCase {
  std::string name;
  std::uint32_t width{};
  std::uint32_t height{};
  std::vector<std::uint16_t> rgb;
};

inline std::uint16_t q16(const double value) {
  return static_cast<std::uint16_t>(
      std::clamp(value, 0.0, 1.0) * 65535.0 + 0.5);
}

inline std::vector<CorpusCase> make_corpus(std::uint32_t width = 960U, std::uint32_t height = 540U) {
  std::vector<CorpusCase> corpus;

  CorpusCase ui{"sdr_ui_edges", width, height, {}};
  ui.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
  for (std::uint32_t y = 0U; y < height; ++y) {
    for (std::uint32_t x = 0U; x < width; ++x) {
      const bool panel = x > 80U && x < 880U && y > 55U && y < 485U;
      const bool toolbar = panel && y > 420U;
      const bool button = toolbar && x > 640U && x < 820U;
      const auto value = button ? 0.82 : toolbar ? 0.18 : panel ? 0.32 : 0.08;
      const auto offset = (static_cast<std::size_t>(y) * width + x) * 3U;
      ui.rgb[offset] = q16(value);
      ui.rgb[offset + 1U] = q16(button ? 0.72 : value);
      ui.rgb[offset + 2U] = q16(button ? 0.16 : value);
    }
  }
  corpus.push_back(std::move(ui));

  CorpusCase gradient{"sdr_smooth_gradient", width, height, {}};
  gradient.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
  for (std::uint32_t y = 0U; y < height; ++y) {
    for (std::uint32_t x = 0U; x < width; ++x) {
      const auto fx = static_cast<double>(x) / static_cast<double>(width - 1U);
      const auto fy = static_cast<double>(y) / static_cast<double>(height - 1U);
      const auto offset = (static_cast<std::size_t>(y) * width + x) * 3U;
      gradient.rgb[offset] = q16(fx);
      gradient.rgb[offset + 1U] = q16(0.15 + 0.75 * fy);
      gradient.rgb[offset + 2U] = q16(0.08 + 0.82 * (0.65 * fx + 0.35 * fy));
    }
  }
  corpus.push_back(std::move(gradient));

  CorpusCase highlights{"hdr_local_highlights", width, height, {}};
  highlights.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
  for (std::uint32_t y = 0U; y < height; ++y) {
    for (std::uint32_t x = 0U; x < width; ++x) {
      const auto dx = (static_cast<double>(x) - 690.0) / 150.0;
      const auto dy = (static_cast<double>(y) - 190.0) / 110.0;
      const auto highlight = std::exp(-(dx * dx + dy * dy) * 2.0);
      const auto base = 0.34 + 0.12 * static_cast<double>(y) / height;
      const auto offset = (static_cast<std::size_t>(y) * width + x) * 3U;
      highlights.rgb[offset] = q16(base + 0.55 * highlight);
      highlights.rgb[offset + 1U] = q16(base + 0.48 * highlight);
      highlights.rgb[offset + 2U] = q16(base + 0.30 * highlight);
    }
  }
  corpus.push_back(std::move(highlights));

  CorpusCase photo{"seeded_photo_like", width, height, {}};
  photo.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
  std::uint32_t state = 0xC001D00DU;
  for (std::uint32_t y = 0U; y < height; ++y) {
    for (std::uint32_t x = 0U; x < width; ++x) {
      state = state * 1664525U + 1013904223U;
      const auto noise = static_cast<double>((state >> 8U) & 0xFFFFU) / 65535.0 - 0.5;
      const auto fx = static_cast<double>(x) / width;
      const auto fy = static_cast<double>(y) / height;
      const auto base = 0.18 + 0.55 * fx + 0.12 * std::sin(fy * 18.0);
      const auto offset = (static_cast<std::size_t>(y) * width + x) * 3U;
      photo.rgb[offset] = q16(base + noise * 0.035);
      photo.rgb[offset + 1U] = q16(base * 0.88 + fy * 0.08 + noise * 0.03);
      photo.rgb[offset + 2U] = q16(base * 0.70 + (1.0 - fx) * 0.12 + noise * 0.04);
    }
  }
  corpus.push_back(std::move(photo));
  return corpus;
}

}  // namespace hdrshot::benchmark
