#include "platform/freetype/freetype_text_rasterizer_port.hpp"
#include "ports/annotation_font_contract.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#ifndef HDRSHOT_TEST_FONT_PATH
#error HDRSHOT_TEST_FONT_PATH must name the fixed Noto Sans SC asset
#endif

namespace {

constexpr const char* kFontIdentity = hdrshot::annotation_font::identity;

std::uint64_t hash_mask(const std::vector<std::uint8_t>& values) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto value : values) {
    hash ^= value;
    hash *= 1099511628211ULL;
  }
  return hash;
}

void fixed_font_renders_chinese_and_latin_deterministically() {
  hdrshot::FreeTypeTextRasterizerPort rasterizer(HDRSHOT_TEST_FONT_PATH, kFontIdentity);
  const auto request = hdrshot::TextRasterRequest{
      hdrshot::ObjectId{1}, "HDR 截图", hdrshot::PixelSize{320, 64}, 20, 2.0};
  const auto first = rasterizer.rasterize(request);
  const auto second = rasterizer.rasterize(request);
  HDRSHOT_CHECK(first.has_value());
  HDRSHOT_CHECK(second.has_value());
  HDRSHOT_CHECK(first.value().coverage_u8 == second.value().coverage_u8);
  HDRSHOT_CHECK(first.value().font_identity == kFontIdentity);
  HDRSHOT_CHECK(std::count_if(
      first.value().coverage_u8.begin(),
      first.value().coverage_u8.end(),
      [](const std::uint8_t value) { return value != 0; }) > 300);
  std::cout << "fixed_font_hash=" << hash_mask(first.value().coverage_u8) << '\n';
  HDRSHOT_CHECK(hash_mask(first.value().coverage_u8) == 2678395914446492182ULL);
}

void point_scale_changes_physical_glyph_size() {
  hdrshot::FreeTypeTextRasterizerPort rasterizer(HDRSHOT_TEST_FONT_PATH, kFontIdentity);
  const auto one_x = rasterizer.rasterize(hdrshot::TextRasterRequest{
      hdrshot::ObjectId{2}, "字", hdrshot::PixelSize{96, 96}, 20, 1.0});
  const auto two_x = rasterizer.rasterize(hdrshot::TextRasterRequest{
      hdrshot::ObjectId{2}, "字", hdrshot::PixelSize{96, 96}, 20, 2.0});
  HDRSHOT_CHECK(one_x.has_value());
  HDRSHOT_CHECK(two_x.has_value());
  const auto one_coverage = std::count_if(
      one_x.value().coverage_u8.begin(), one_x.value().coverage_u8.end(),
      [](const std::uint8_t value) { return value != 0; });
  const auto two_coverage = std::count_if(
      two_x.value().coverage_u8.begin(), two_x.value().coverage_u8.end(),
      [](const std::uint8_t value) { return value != 0; });
  HDRSHOT_CHECK(two_coverage > one_coverage * 2);
}

void invalid_utf8_and_missing_glyph_fail_explicitly() {
  hdrshot::FreeTypeTextRasterizerPort rasterizer(HDRSHOT_TEST_FONT_PATH, kFontIdentity);
  const auto invalid = rasterizer.rasterize(hdrshot::TextRasterRequest{
      hdrshot::ObjectId{3}, std::string{"\xC0\xAF", 2}, hdrshot::PixelSize{64, 64}, 20, 1.0});
  HDRSHOT_CHECK(!invalid.has_value());
  HDRSHOT_CHECK(invalid.error().code == hdrshot::ErrorCode::invalid_input);

  const auto emoji = rasterizer.rasterize(hdrshot::TextRasterRequest{
      hdrshot::ObjectId{3}, "😀", hdrshot::PixelSize{64, 64}, 20, 1.0});
  HDRSHOT_CHECK(!emoji.has_value());
  HDRSHOT_CHECK(emoji.error().code == hdrshot::ErrorCode::unsupported_encoding);
}

void text_measurement_matches_the_rasterizer_font_contract() {
  hdrshot::FreeTypeTextRasterizerPort rasterizer(HDRSHOT_TEST_FONT_PATH, kFontIdentity);
  const auto one_x = rasterizer.measure(hdrshot::TextMeasureRequest{"HDR 截图", 20, 1.0});
  const auto two_x = rasterizer.measure(hdrshot::TextMeasureRequest{"HDR 截图", 20, 2.0});
  HDRSHOT_CHECK(one_x.has_value());
  HDRSHOT_CHECK(two_x.has_value());
  HDRSHOT_CHECK(one_x.value().minimum_mask_size_px.width > 0);
  HDRSHOT_CHECK(one_x.value().minimum_mask_size_px.height > 0);
  HDRSHOT_CHECK(two_x.value().minimum_mask_size_px.width >
                one_x.value().minimum_mask_size_px.width);
  HDRSHOT_CHECK(two_x.value().minimum_mask_size_px.height >
                one_x.value().minimum_mask_size_px.height);
  HDRSHOT_CHECK(two_x.value().font_identity == kFontIdentity);
}

void small_cjk_bottom_horizontal_preserves_antialiased_edges() {
  hdrshot::FreeTypeTextRasterizerPort rasterizer(HDRSHOT_TEST_FONT_PATH, kFontIdentity);
  const auto mask = rasterizer.rasterize({hdrshot::ObjectId{1}, "在", {80, 58}, 24, 1.0});
  HDRSHOT_CHECK(mask.has_value());
  // Right of the vertical stem, the design's bottom horizontal spans three
  // coverage rows, with one opaque center and two fractional edges. The old
  // automatic hinter made all three rows fully opaque at wght=450.
  constexpr int x = 19;
  int bottom = 57;
  const auto sample = [&](const int y) {
    return mask.value().coverage_u8[static_cast<std::size_t>(y * 80 + x)];
  };
  while (bottom >= 0 && sample(bottom) == 0) --bottom;
  HDRSHOT_CHECK(bottom >= 2);
  HDRSHOT_CHECK(sample(bottom - 1) == 255);
  HDRSHOT_CHECK(sample(bottom - 2) > 0 && sample(bottom - 2) < 200);
  HDRSHOT_CHECK(sample(bottom) > 0 && sample(bottom) < 200);
  HDRSHOT_CHECK(static_cast<int>(sample(bottom - 2)) + sample(bottom - 1) +
                sample(bottom) < 600);
}

void production_padded_masks_cover_dense_and_mixed_text_at_all_scales() {
  hdrshot::FreeTypeTextRasterizerPort rasterizer(HDRSHOT_TEST_FONT_PATH, kFontIdentity);
  for (const auto size : {12, 16, 20, 22, 24, 28, 32, 40, 48}) {
    for (const auto scale : {1.0, 1.25, 1.5, 2.0}) {
      for (const auto* text : {"在里 土王三 目重画田", "HDR 在里 Agj 0123", "目重田 Agj 在里"}) {
        const auto metrics = rasterizer.measure({text, static_cast<std::uint16_t>(size), scale});
        HDRSHOT_CHECK(metrics.has_value());
        const auto extent = metrics.value().minimum_mask_size_px;
        const auto fitted = rasterizer.rasterize({hdrshot::ObjectId{1}, text, extent,
            static_cast<std::uint16_t>(size), scale});
        const auto wide = rasterizer.rasterize({hdrshot::ObjectId{1}, text,
            {extent.width + 8, extent.height}, static_cast<std::uint16_t>(size), scale});
        HDRSHOT_CHECK(fitted.has_value() && wide.has_value());
        for (int y = 0; y < extent.height; ++y) {
          for (int x = 0; x < extent.width + 8; ++x) {
            const auto value = wide.value().coverage_u8[
                static_cast<std::size_t>(y * (extent.width + 8) + x)];
            if (x < extent.width) {
              HDRSHOT_CHECK(value == fitted.value().coverage_u8[
                  static_cast<std::size_t>(y * extent.width + x)]);
            } else {
              HDRSHOT_CHECK(value == 0);
            }
          }
        }
        // Match normalize_text_bounds' production minimum and padding. Tight
        // nominal line-height can clip descenders in the old implementation
        // too; this regression verifies that the font fix adds no UI clipping.
        const auto padded_height = std::max(extent.height + static_cast<int>(std::ceil(4.0 * scale)),
            static_cast<int>(std::ceil((size * 1.4 + 12.0) * scale)));
        const auto padded = rasterizer.rasterize({hdrshot::ObjectId{1}, text,
            {extent.width + 8, padded_height}, static_cast<std::uint16_t>(size), scale});
        const auto tall = rasterizer.rasterize({hdrshot::ObjectId{1}, text,
            {extent.width + 8, padded_height + 16}, static_cast<std::uint16_t>(size), scale});
        HDRSHOT_CHECK(padded.has_value() && tall.has_value());
        const auto sum = [](const std::vector<std::uint8_t>& coverage) {
          std::uint64_t total = 0;
          for (const auto value : coverage) total += value;
          return total;
        };
        if (sum(padded.value().coverage_u8) != sum(tall.value().coverage_u8)) {
          std::cout << "padding_clipped size=" << size << " scale=" << scale << " text=" << text
                    << " height=" << padded_height << " ink=" << sum(padded.value().coverage_u8)
                    << '/' << sum(tall.value().coverage_u8) << '\n';
        }
        HDRSHOT_CHECK(sum(padded.value().coverage_u8) == sum(tall.value().coverage_u8));
      }
    }
  }
}

void equivalent_physical_sizes_have_identical_coverage() {
  hdrshot::FreeTypeTextRasterizerPort rasterizer(HDRSHOT_TEST_FONT_PATH, kFontIdentity);
  const auto one = rasterizer.rasterize({hdrshot::ObjectId{1}, "在里 HDR", {180, 58}, 24, 1.0});
  const auto two = rasterizer.rasterize({hdrshot::ObjectId{1}, "在里 HDR", {180, 58}, 12, 2.0});
  HDRSHOT_CHECK(one.has_value() && two.has_value());
  HDRSHOT_CHECK(one.value().coverage_u8 == two.value().coverage_u8);
}

}  // namespace

int main() {
  using hdrshot::test::TestCase;
  return hdrshot::test::run(std::vector<TestCase>{
      {"fixed font Chinese and Latin deterministic", fixed_font_renders_chinese_and_latin_deterministically},
      {"point scale changes glyph coverage", point_scale_changes_physical_glyph_size},
      {"invalid UTF-8 and missing glyph explicit", invalid_utf8_and_missing_glyph_fail_explicitly},
      {"text measurement shares rasterizer metrics", text_measurement_matches_the_rasterizer_font_contract},
      {"small CJK bottom horizontal retains fractional edges", small_cjk_bottom_horizontal_preserves_antialiased_edges},
      {"production padded masks retain ink at nine sizes and four scales", production_padded_masks_cover_dense_and_mixed_text_at_all_scales},
      {"equal physical sizes have identical masks", equivalent_physical_sizes_have_identical_coverage},
  });
}
