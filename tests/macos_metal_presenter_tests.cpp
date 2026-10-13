#include "platform/macos/metal_edr_presenter.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <iostream>
#include <vector>

namespace {

std::uint16_t float_to_half_bits(const float value) {
  const _Float16 half = static_cast<_Float16>(value);
  std::uint16_t bits{};
  static_assert(sizeof(half) == sizeof(bits));
  std::memcpy(&bits, &half, sizeof(bits));
  return bits;
}

float linear_to_extended_srgb(const float linear) {
  return linear <= 0.0031308F
      ? linear * 12.92F
      : 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
}

hdrshot::MacMetalOverlayRequest make_request() {
  constexpr std::size_t width = 8;
  constexpr std::size_t height = 4;
  const float edr_by_column[width] = {0.25F, 1.0F, 2.0F, 4.0F, 0.25F, 1.0F, 2.0F, 4.0F};
  std::vector<std::uint16_t> samples;
  samples.reserve(width * height * 4U);
  for (std::size_t y = 0; y < height; ++y) {
    for (std::size_t x = 0; x < width; ++x) {
      const auto encoded = float_to_half_bits(linear_to_extended_srgb(edr_by_column[x]));
      samples.push_back(encoded);
      samples.push_back(encoded);
      samples.push_back(encoded);
      samples.push_back(float_to_half_bits(1.0F));
    }
  }
  return hdrshot::MacMetalOverlayRequest{
      width,
      height,
      std::make_shared<const std::vector<std::uint16_t>>(std::move(samples)),
      hdrshot::PixelRect{2, 1, 4, 2},
      0.35F,
      2.03F,
      0,
  };
}

std::size_t channel_offset(
    const hdrshot::MacMetalOverlayReadback& readback,
    const std::size_t x,
    const std::size_t y,
    const std::size_t channel = 0) {
  return (y * readback.width_px + x) * 4U + channel;
}

std::unique_ptr<hdrshot::MacMetalEdrPresenter> make_presenter() {
  auto created = hdrshot::MacMetalEdrPresenter::create();
  if (!created.has_value()) {
    for (const auto& [key, value] : created.error().safe_context) {
      std::cerr << key << '=' << value << '\n';
    }
  }
  HDRSHOT_CHECK(created.has_value());
  return std::move(created.value());
}

void frozen_surface_contract_and_linear_mask_pass_readback() {
  auto presenter = make_presenter();
  const auto request = make_request();
  const auto rendered = presenter->render_offscreen(request);
  HDRSHOT_CHECK(rendered.has_value());
  const auto& readback = rendered.value();
  HDRSHOT_CHECK(readback.texture_format == "MTLPixelFormatRGBA16Float");
  HDRSHOT_CHECK(readback.layer_color_space == "kCGColorSpaceExtendedLinearDisplayP3");
  HDRSHOT_CHECK(readback.optical_output_scale_nits == 100.0);

  HDRSHOT_CHECK_NEAR(
      readback.rgba_linear_display_p3[channel_offset(readback, 3, 1)], 4.0F, 0.02F);
  const auto dimmed_highlight = readback.rgba_linear_display_p3[channel_offset(readback, 7, 1)];
  const auto selected_highlight =
      readback.rgba_linear_display_p3[channel_offset(readback, 3, 1)];
  const auto dimmed_reference_white =
      readback.rgba_linear_display_p3[channel_offset(readback, 1, 1)];
  std::cout << "selected400=" << selected_highlight
            << " dimmed400=" << dimmed_highlight
            << " dimmed100=" << dimmed_reference_white << '\n';
  HDRSHOT_CHECK_NEAR(dimmed_highlight, 1.4F, 0.02F);
  HDRSHOT_CHECK(dimmed_highlight > 1.0F);
  HDRSHOT_CHECK_NEAR(
      dimmed_reference_white, 0.35F, 0.01F);
}

void ui_border_uses_explicit_203_nit_value() {
  auto presenter = make_presenter();
  auto request = make_request();
  request.ui_border_width_px = 1;
  const auto rendered = presenter->render_offscreen(request);
  HDRSHOT_CHECK(rendered.has_value());
  HDRSHOT_CHECK_NEAR(
      rendered.value().rgba_linear_display_p3[channel_offset(rendered.value(), 2, 1)],
      2.03F,
      0.01F);
}

void sdr_surface_clamps_preview_and_ui_to_standard_range() {
  auto presenter = make_presenter();
  auto request = make_request();
  request.target_surface_range = hdrshot::MacMetalSurfaceRange::sdr;
  request.ui_white_edr = 1.0F;
  request.ui_border_width_px = 1;
  const auto rendered = presenter->render_offscreen(request);
  HDRSHOT_CHECK(rendered.has_value());
  for (std::size_t index = 0; index < rendered.value().rgba_linear_display_p3.size();
       index += 4U) {
    HDRSHOT_CHECK(rendered.value().rgba_linear_display_p3[index] >= 0.0F);
    HDRSHOT_CHECK(rendered.value().rgba_linear_display_p3[index] <= 1.0F);
  }
  HDRSHOT_CHECK_NEAR(
      rendered.value().rgba_linear_display_p3[channel_offset(rendered.value(), 3, 1)],
      1.0F,
      0.001F);
  HDRSHOT_CHECK_NEAR(
      rendered.value().rgba_linear_display_p3[channel_offset(rendered.value(), 2, 1)],
      1.0F,
      0.001F);
}

void empty_selection_is_a_valid_fully_dimmed_waiting_state() {
  auto presenter = make_presenter();
  auto request = make_request();
  request.selection_px = {};
  request.ui_border_width_px = 2;
  const auto rendered = presenter->render_offscreen(request);
  HDRSHOT_CHECK(rendered.has_value());
  const float expected_by_column[8] = {
      0.0875F, 0.35F, 0.70F, 1.40F, 0.0875F, 0.35F, 0.70F, 1.40F};
  for (std::size_t y = 0; y < request.height_px; ++y) {
    for (std::size_t x = 0; x < request.width_px; ++x) {
      HDRSHOT_CHECK_NEAR(
          rendered.value().rgba_linear_display_p3[
              channel_offset(rendered.value(), x, y)],
          expected_by_column[x],
          0.02F);
    }
  }
}

void invalid_dim_contract_is_rejected() {
  auto presenter = make_presenter();
  auto request = make_request();
  request.outside_linear_dim_factor = 1.5F;
  const auto rendered = presenter->render_offscreen(request);
  HDRSHOT_CHECK(!rendered.has_value());
  HDRSHOT_CHECK(rendered.error().code == hdrshot::ErrorCode::invalid_color_contract);
}

void linear_source_is_not_decoded_twice() {
  auto request = make_request();
  std::vector<std::uint16_t> samples(8*4*4);
  const float levels[] = {.25F,1.F,2.F,4.F,8.F,4.F,2.F,1.F};
  for (std::size_t y=0;y<4;++y) for (std::size_t x=0;x<8;++x) {
    for (std::size_t c=0;c<3;++c) samples[(y*8+x)*4+c] = float_to_half_bits(levels[x]);
    samples[(y*8+x)*4+3] = float_to_half_bits(1);
  }
  request.rgba_half_extended_p3 = std::make_shared<const std::vector<std::uint16_t>>(samples);
  request.source_is_linear = true;
  request.selection_px = {0,0,8,4};
  auto result = make_presenter()->render_offscreen(request);
  HDRSHOT_CHECK(result.has_value());
  for (std::size_t y=0;y<4;++y) for (std::size_t x=0;x<8;++x)
    HDRSHOT_CHECK(result.value().rgba_linear_display_p3[(y*8+x)*4] == levels[x]);
}

void encoded_sdr_is_linearized_before_preview() {
  auto request = make_request();
  std::vector<std::uint16_t> samples(8*4*4, float_to_half_bits(0.5F));
  for (std::size_t i=3;i<samples.size();i+=4) samples[i]=float_to_half_bits(1.F);
  request.rgba_half_extended_p3 = std::make_shared<const std::vector<std::uint16_t>>(samples);
  request.source_is_linear = false;
  request.selection_px = {0,0,8,4};
  auto result = make_presenter()->render_offscreen(request);
  HDRSHOT_CHECK(result.has_value());
  // Encoded 0.5 is ~0.214 linear, not 0.5. This is a numerical regression,
  // not a substitute for capturing an actual Finder/vibrancy surface.
  HDRSHOT_CHECK_NEAR(result.value().rgba_linear_display_p3[0], 0.21404114F, 0.0003F);
}

void magnifier_reticle_symmetric_rails_and_external_box() {
  constexpr int width=128,height=112;
  auto presenter=make_presenter();
  for(float background:{0.015625F,1.0F,4.0F}) {
    auto samples=std::make_shared<std::vector<std::uint16_t>>(width*height*4,float_to_half_bits(background));
    for(std::size_t i=3;i<samples->size();i+=4) (*samples)[i]=float_to_half_bits(1.0F);
    hdrshot::MacMetalOverlayRequest request;
    request.width_px=width;request.height_px=height;
    request.rgba_half_extended_p3=samples;request.source_is_linear=true;
    request.ui_border_width_px=0;request.outside_linear_dim_factor=0.25F;
    for(int zoom:{4,8,12,16}) for(bool fused:{false,true}) {
      // Touch opposite viewport corners, including the last physical pixel.
      for(hdrshot::PixelPoint origin:{hdrshot::PixelPoint{0,0},
          hdrshot::PixelPoint{width-5*zoom,height-5*zoom}}) {
        request.magnifier={true,{63,61},{origin.x,origin.y,5*zoom,5*zoom},zoom,1};
        const auto rendered=presenter->render_offscreen(request,fused);
        HDRSHOT_CHECK(rendered.has_value());
        const auto& result=rendered.value();
        auto check=[&](int x,int y,float expected) {
          const auto offset=channel_offset(result,static_cast<std::size_t>(origin.x+x),static_cast<std::size_t>(origin.y+y));
          for(std::size_t c=0;c<3;++c) HDRSHOT_CHECK_NEAR(result.rgba_linear_display_p3[offset+c],expected,0.002F);
          HDRSHOT_CHECK(result.rgba_linear_display_p3[offset+3]==1.0F);
        };
        const int middle=5*zoom/2,last=5*zoom-1;
        // Cross sections: unchanged source, black, original white, black,
        // unchanged source. Fixed five samples expose width/center drift.
        const float cross_section[]{background,0.0F,request.ui_white_edr,0.0F,background};
        for(int i=0;i<5;++i) {
          check(middle+i-2,0,cross_section[i]);check(middle+i-2,last,cross_section[i]);
          check(0,middle+i-2,cross_section[i]);check(last,middle+i-2,cross_section[i]);
        }
        // No cap extends the arms into the existing one-zoom central gap.
        for(int side:{-1,1}) for(int offset=-1;offset<=1;++offset) {
          check(middle+offset,middle+side*zoom,background);
          check(middle+side*zoom,middle+offset,background);
          check(middle+offset,middle+side*(zoom+1),offset==0?request.ui_white_edr:0.0F);
          check(middle+side*(zoom+1),middle+offset,offset==0?request.ui_white_edr:0.0F);
        }
        const int left=2*zoom,right=3*zoom-1;
        for(int edge:{left,right}) {
          const int outward=edge==left?-1:1;
          check(edge,middle,request.ui_white_edr);check(middle,edge,request.ui_white_edr);
          check(edge+outward,middle,0.0F);check(middle,edge+outward,0.0F);
          check(edge+2*outward,middle,background);check(middle,edge+2*outward,background);
        }
        for(int y:{left-1,right+1}) for(int x:{left-1,right+1}) check(x,y,0.0F);
        // At 4x all four interior pixels must survive; larger cells retain
        // their entire interior too, with no black border painted inward.
        for(int y=left+1;y<right;++y) for(int x=left+1;x<right;++x) check(x,y,background);
        const int outside_x=origin.x==0?width-1:0,outside_y=origin.y==0?height-1:0;
        HDRSHOT_CHECK_NEAR(result.rgba_linear_display_p3[channel_offset(result,static_cast<std::size_t>(outside_x),static_cast<std::size_t>(outside_y))],background*0.25F,0.001F);
      }
    }
  }
}
}  // namespace

int main() {
  const auto availability = hdrshot::MacMetalEdrPresenter::create();
  if (!availability) {
    const auto reason = availability.error().safe_context.find("reason");
    if (reason != availability.error().safe_context.end() &&
        reason->second == "metal_device_unavailable") {
      std::cout << "[SKIP] Metal device unavailable in the current desktop session\n";
      return 77;
    }
    std::cerr << "Metal presenter preflight failed: "
              << hdrshot::to_string(availability.error().code) << '\n';
    return 1;
  }
  using hdrshot::test::TestCase;
  return hdrshot::test::run(std::vector<TestCase>{
      {"Linear P3 source readback is exact", linear_source_is_not_decoded_twice},
      {"Encoded SDR P3 is linearized before preview", encoded_sdr_is_linearized_before_preview},
      {"Magnifier symmetric 3px rails and external box on dark, light and HDR", magnifier_reticle_symmetric_rails_and_external_box},
      {"Metal EDR surface and linear mask readback", frozen_surface_contract_and_linear_mask_pass_readback},
      {"Metal UI border is 203-nit SDR-referred", ui_border_uses_explicit_203_nit_value},
      {"Metal SDR surface clamps preview and UI", sdr_surface_clamps_preview_and_ui_to_standard_range},
      {"Metal empty selection is a fully dimmed waiting state", empty_selection_is_a_valid_fully_dimmed_waiting_state},
      {"Metal presenter rejects invalid dim", invalid_dim_contract_is_rejected},
  });
}
