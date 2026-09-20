#include "platform/freetype/freetype_text_rasterizer_port.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#ifndef HDRSHOT_TEST_FONT_PATH
#error HDRSHOT_TEST_FONT_PATH must name the fixed Noto Sans SC asset
#endif

namespace {

constexpr const char* kFontIdentity =
    "NotoSansSC-VF.ttf@f8d1575;wght=450;hinting=native#d68bafcb48a2707749396aa12bbbd833cb70401f3a9a689fd2902c7e0d295964";

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
  HDRSHOT_CHECK(hash_mask(first.value().coverage_u8) == 6976100536247766231ULL);
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

}  // namespace

int main() {
  using hdrshot::test::TestCase;
  return hdrshot::test::run(std::vector<TestCase>{
      {"fixed font Chinese and Latin deterministic", fixed_font_renders_chinese_and_latin_deterministically},
      {"point scale changes glyph coverage", point_scale_changes_physical_glyph_size},
      {"invalid UTF-8 and missing glyph explicit", invalid_utf8_and_missing_glyph_fail_explicitly},
      {"text measurement shares rasterizer metrics", text_measurement_matches_the_rasterizer_font_contract},
  });
}
