#include "platform/windows/windows_gpu_source.hpp"
#include "platform/windows/windows_capture_policy.hpp"
#include "platform/windows/windows_color.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "test_support.hpp"
#include <cmath>
#include <limits>
#include <utility>
using namespace hdrshot;
namespace {
void compatibility_options_reach_gpu_once() {
  // Exactly representable binary16 white, chroma, negative, extended samples.
  const std::vector<std::uint16_t> raw{
      0x4000,0x4000,0x4000,0x7e00, 0x3400,0x3800,0x3e00,0,
      0xb400,0,0,0, 0,0x4000,0x4a00,0};
  // FP64 oracle constructed from xy primaries, independent of product matrix.
  constexpr double reference[4][3] = {
      {2,2,2}, {0.2943845078214094,0.49170145028725964,1.4062492709346368},
      {-0.2056154921785906,-0.008298549712740409,-0.00427065768028001},
      {0.35507606257127494,1.9336116022980774,11.071034024706927}};
  for (bool hdr : {false,true}) for (double white : {80.0,100.0,203.0,204.0,240.0,400.0,480.0}) {
    const auto normal=WindowsColor::scrgb_to_edr_scale({hdr,white});
    HDRSHOT_CHECK(normal.has_value());
    for (bool gain_on : {false,true}) for (bool bypass : {false,true}) {
      const auto adjustment=resolve_capture_adjustment({gain_on,bypass,0.5},hdr,normal.value());
      HDRSHOT_CHECK(adjustment.has_value());
      auto source=windows_normalize_scrgb_source({4,1},raw,
          adjustment.value().white_scale,adjustment.value().effective_gain);
      HDRSHOT_CHECK(source.has_value());
      const auto pixels=source.value()->read_region({0,0,4,1});
      HDRSHOT_CHECK(pixels.has_value());
      const double scale=(gain_on?0.5:1.0)*(hdr&&!bypass?80.0/white:1.0);
      for (int p=0;p<4;++p) {
        for (int c=0;c<3;++c)
          HDRSHOT_CHECK_NEAR(pixels.value()[p*4+c],reference[p][c]*scale,2e-6);
        HDRSHOT_CHECK_NEAR(pixels.value()[p*4+3],1.0,0);
      }
    }
  }
}
void gain_vectors_and_alpha() {
  const std::vector<std::uint16_t> raw{0x4000, 0x4000, 0x4000, 0x7e00};
  for (const auto [gain, expected] :
       {std::pair{0.0, 0.0}, {0.5, 1.0}, {1.0, 2.0}, {3.0, 6.0}}) {
    auto source = windows_normalize_scrgb_source({1, 1}, raw, 1.0F, gain);
    HDRSHOT_CHECK(source.has_value());
    HDRSHOT_CHECK(source.value()->byte_count() == 16U);
    auto region = source.value()->read_region({0, 0, 1, 1});
    HDRSHOT_CHECK(region.has_value());
    for (int c = 0; c < 3; ++c)
      HDRSHOT_CHECK_NEAR(region.value()[c], expected, 2e-6);
    HDRSHOT_CHECK_NEAR(region.value()[3], 1.0, 0);
  }
  auto white = windows_normalize_scrgb_source({1, 1}, raw, 0.4F, 0.5);
  HDRSHOT_CHECK(white.has_value());
  auto scaled = white.value()->read_region({0, 0, 1, 1});
  HDRSHOT_CHECK(scaled.has_value());
  HDRSHOT_CHECK_NEAR(scaled.value()[0], 0.4, 2e-6);
}
void invalid_source_and_gain() {
  const std::vector<std::uint16_t> raw{0x4000, 0x4000, 0x4000, 0x3c00};
  HDRSHOT_CHECK(!windows_normalize_scrgb_source({1, 1}, raw, 1.0F, -0.01));
  HDRSHOT_CHECK(!windows_normalize_scrgb_source({1, 1}, raw, 1.0F, 3.01));
  HDRSHOT_CHECK(!windows_normalize_scrgb_source(
      {1, 1}, raw, 1.0F, std::numeric_limits<double>::quiet_NaN()));
  HDRSHOT_CHECK(!windows_normalize_scrgb_source({1, 1}, raw, 0.0F, 1.0));
  HDRSHOT_CHECK(!windows_normalize_scrgb_source({2, 1}, raw, 1.0F, 1.0));
  auto invalid = raw;
  invalid[0] = 0x7c00;
  HDRSHOT_CHECK(!windows_normalize_scrgb_source({1, 1}, invalid, 1.0F, 1.0));
}
void negative_linear_input_is_preserved() {
  const std::vector<std::uint16_t> raw{0xb400, 0, 0, 0}; // -0.25 red
  auto source = windows_normalize_scrgb_source({1, 1}, raw, 1.0F, 1.0);
  HDRSHOT_CHECK(source.has_value());
  auto pixel = source.value()->read_region({0, 0, 1, 1});
  HDRSHOT_CHECK(pixel.has_value());
  HDRSHOT_CHECK_NEAR(pixel.value()[0], -0.25 * 0.82246197, 2e-6);
  HDRSHOT_CHECK_NEAR(pixel.value()[1], -0.25 * 0.03319420, 2e-6);
}
void bounded_single_pixel_readback_matches_full_float_source() {
  constexpr PixelSize size{17,9};
  std::vector<std::uint16_t> raw;
  for(int y=0;y<size.height;++y) for(int x=0;x<size.width;++x) {
    // Binary16-exact inputs. The matrix, white and gain yield FP32 values
    // whose precision must survive an isolated 1x1 staging readback.
    raw.insert(raw.end(),{static_cast<std::uint16_t>(0x3400+x*16),
        static_cast<std::uint16_t>(0x3800+y*32),
        static_cast<std::uint16_t>(0x3c00+(x+y)*8),0x7e00});
  }
  const auto source=windows_normalize_scrgb_source(size,raw,80.F/203.F,0.5);
  HDRSHOT_CHECK(source.has_value());
  HDRSHOT_CHECK(source.value()->wait_until_ready().has_value());
  const auto full=source.value()->read_region({0,0,17,9}); HDRSHOT_CHECK(full.has_value());
  bool has_non_half_value=false;
  for(int y=0;y<size.height;++y) for(int x=0;x<size.width;++x) {
    const auto pixel=source.value()->read_region({x,y,1,1});
    HDRSHOT_CHECK(pixel.has_value() && pixel.value().size()==4);
    const auto offset=static_cast<std::size_t>(y*17+x)*4;
    for(int c=0;c<4;++c) HDRSHOT_CHECK(pixel.value()[c]==full.value()[offset+c]);
    for(int c=0;c<3;++c) {
      const auto half=ExtendedP3Mapper::encode_binary16(pixel.value()[c]);
      has_non_half_value|=ExtendedP3Mapper::decode_binary16(half).value()!=pixel.value()[c];
    }
    HDRSHOT_CHECK(pixel.value()[3]==1.F);
  }
  HDRSHOT_CHECK(has_non_half_value);
  const auto edge=source.value()->read_region({14,7,3,2}); HDRSHOT_CHECK(edge.has_value());
  for(int y=0;y<2;++y) for(int x=0;x<3;++x) for(int c=0;c<4;++c)
    HDRSHOT_CHECK(edge.value()[static_cast<std::size_t>(y*3+x)*4+c]==
        full.value()[static_cast<std::size_t>((y+7)*17+x+14)*4+c]);
  for(PixelRect invalid:{PixelRect{-1,0,1,1},PixelRect{0,-1,1,1},PixelRect{17,0,1,1},
      PixelRect{0,9,1,1},PixelRect{0,0,0,1},PixelRect{0,0,1,0},PixelRect{16,8,2,1},
      PixelRect{16,8,1,2},PixelRect{1,0,std::numeric_limits<int>::max(),1}})
    HDRSHOT_CHECK(!source.value()->read_region(invalid));
}
} // namespace
int main() {
  return test::run({{"capture compatibility options reach GPU once", compatibility_options_reach_gpu_once},
                    {"GPU source gain vectors", gain_vectors_and_alpha},
                    {"GPU source input validation", invalid_source_and_gain},
                    {"GPU source retains negative linear input",
                     negative_linear_input_is_preserved},
                    {"bounded FP32 single-pixel and edge readback",bounded_single_pixel_readback_matches_full_float_source}});
}
