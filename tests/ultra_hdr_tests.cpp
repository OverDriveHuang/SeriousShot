#include "adapters/shared/libultrahdr_encoder.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "domain/color/display_p3_icc_profile.hpp"
#include "domain/output/ultra_hdr_input_renderer.hpp"
#include "test_support.hpp"
#include "ultra_hdr_metadata_test_support.hpp"
#include "ultra_hdr_icc_oracle.hpp"

#include <ultrahdr_api.h>
#include <ultrahdr/jpegdecoderhelper.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <iostream>
#include <fstream>
#include <iterator>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace hdrshot;

using DecoderHandle = std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)>;

struct Fixture {
  std::vector<std::uint16_t> rgba;
  SelectionRoiView view;
  AnnotationPixelPlan plan;
  std::vector<float> expected_linear;
};

Fixture linear_gradient_fixture() {
  constexpr std::int32_t width = 32;
  constexpr std::int32_t height = 16;
  Fixture fixture;
  fixture.rgba.resize(static_cast<std::size_t>(width * height * 4));
  fixture.expected_linear.resize(static_cast<std::size_t>(width * height));
  for (std::int32_t y = 0; y < height; ++y) {
    fixture.plan.source_visible_spans.push_back(SourceVisibleSpan{y, 0, width});
    for (std::int32_t x = 0; x < width; ++x) {
      const auto pixel = static_cast<std::size_t>(y * width + x);
      const float linear = 0.02F + 3.98F * static_cast<float>(x) /
          static_cast<float>(width - 1);
      fixture.expected_linear[pixel] = linear;
      const auto encoded = ExtendedP3Mapper::encode_binary16(
          ExtendedP3Mapper::encode_extended_srgb(linear));
      fixture.rgba[pixel * 4U] = encoded;
      fixture.rgba[pixel * 4U + 1U] = encoded;
      fixture.rgba[pixel * 4U + 2U] = encoded;
      fixture.rgba[pixel * 4U + 3U] =
          ExtendedP3Mapper::encode_binary16(1.0F);
    }
  }
  fixture.plan.output_size_px = PixelSize{width, height};
  fixture.view = SelectionRoiView{
      FrameId{1}, DisplayGeneration{1}, SelectionRevision{1},
      PixelRect{0, 0, width, height}, PixelSize{width, height}, 1.0,
      ColorEncoding{ColorPrimaries::display_p3, TransferFunction::extended_srgb,
                    AlphaMode::straight, 0.0},
      DisplayDynamicRange::hdr, fixture.rgba, 0U,
      static_cast<std::size_t>(width * 4)};
  return fixture;
}

bool contains_ascii(
    const std::vector<std::uint8_t>& bytes,
    const std::string_view needle) {
  return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) !=
      bytes.end();
}

LinearDisplayP3HalfImage render_fixture(Fixture& fixture) {
  CpuUltraHdrInputRenderer renderer;
  const auto rendered = renderer.render(UltraHdrInputRenderRequest{
      &fixture.view, &fixture.plan, kUltraHdrReferenceWhiteNits});
  HDRSHOT_CHECK(rendered.has_value());
  HDRSHOT_CHECK(rendered.value().size_px == fixture.view.size_px);
  HDRSHOT_CHECK(rendered.value().rgba_half.size() == fixture.rgba.size());
  HDRSHOT_CHECK(std::abs(rendered.value().maximum_linear_component - 4.0) < 0.02);
  for (std::size_t pixel = 0; pixel < fixture.expected_linear.size(); ++pixel) {
    const auto actual = ExtendedP3Mapper::decode_binary16(
        rendered.value().rgba_half[pixel * 4U]);
    const auto alpha = ExtendedP3Mapper::decode_binary16(
        rendered.value().rgba_half[pixel * 4U + 3U]);
    HDRSHOT_CHECK(actual.has_value() && alpha.has_value());
    HDRSHOT_CHECK(std::abs(actual.value() - fixture.expected_linear[pixel]) < 0.01F);
    HDRSHOT_CHECK(alpha.value() == 1.0F);
  }
  return rendered.value();
}

void cpu_renderer_outputs_fixed_203_nit_linear_p3() {
  auto fixture = linear_gradient_fixture();
  const auto rendered = render_fixture(fixture);
  HDRSHOT_CHECK(rendered.reference_white_nits == 203.0);
}

void cpu_renderer_rejects_invalid_pixel_ownership() {
  CpuUltraHdrInputRenderer renderer;
  for (int mutation = 0; mutation < 4; ++mutation) {
    auto fixture = linear_gradient_fixture();
    if (mutation == 0) {
      // Total length is still the row width, but half overlaps and half is missing.
      fixture.plan.source_visible_spans[0].length = 16;
      fixture.plan.source_visible_spans.insert(
          fixture.plan.source_visible_spans.begin(), SourceVisibleSpan{0, 0, 16});
    } else if (mutation == 1) {
      std::swap(fixture.plan.source_visible_spans[0],
                fixture.plan.source_visible_spans[1]);
    } else if (mutation == 2) {
      fixture.plan.source_visible_spans.push_back(SourceVisibleSpan{16, 0, 32});
    } else {
      fixture.plan.source_visible_spans[0].x = -1;
    }
    const auto result = renderer.render({&fixture.view, &fixture.plan, 203.0});
    HDRSHOT_CHECK(!result.has_value());
    HDRSHOT_CHECK(result.error().code == ErrorCode::state_inconsistent);
  }
}

using hdrshot::test::check_dual_metadata;

void codec_writes_dual_metadata_and_decodes_linear_hdr() {
  auto fixture = linear_gradient_fixture();
  auto rendered = render_fixture(fixture);
  LibUltraHdrEncoder encoder;
  const auto encoded = encoder.encode(
      UltraHdrEncodeRequest{&rendered, UltraHdrJpegQuality::balanced});
  HDRSHOT_CHECK(encoded.has_value());
  HDRSHOT_CHECK(encoded.value().bytes.size() > 1000U);
  HDRSHOT_CHECK(is_uhdr_image(
      const_cast<std::uint8_t*>(encoded.value().bytes.data()),
      static_cast<int>(encoded.value().bytes.size())) == 1);
  HDRSHOT_CHECK(contains_ascii(
      encoded.value().bytes, "http://ns.adobe.com/hdr-gain-map/1.0/"));
  HDRSHOT_CHECK(contains_ascii(encoded.value().bytes, "http://ns.adobe.com/xap/1.0/"));
  HDRSHOT_CHECK(contains_ascii(
      encoded.value().bytes, "urn:iso:std:iso:ts:21496:-1"));

  DecoderHandle decoder{uhdr_create_decoder(), &uhdr_release_decoder};
  HDRSHOT_CHECK(decoder != nullptr);
  uhdr_compressed_image_t input{};
  input.data = const_cast<std::uint8_t*>(encoded.value().bytes.data());
  input.data_sz = encoded.value().bytes.size();
  input.capacity = input.data_sz;
  input.cg = UHDR_CG_UNSPECIFIED;
  input.ct = UHDR_CT_UNSPECIFIED;
  input.range = UHDR_CR_UNSPECIFIED;
  HDRSHOT_CHECK(
      uhdr_dec_set_image(decoder.get(), &input).error_code == UHDR_CODEC_OK);
  HDRSHOT_CHECK(
      uhdr_dec_set_out_img_format(
          decoder.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat).error_code ==
      UHDR_CODEC_OK);
  HDRSHOT_CHECK(
      uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR).error_code ==
      UHDR_CODEC_OK);
  HDRSHOT_CHECK(
      uhdr_dec_set_out_max_display_boost(decoder.get(), 4.0F).error_code ==
      UHDR_CODEC_OK);
  HDRSHOT_CHECK(uhdr_dec_probe(decoder.get()).error_code == UHDR_CODEC_OK);
  hdrshot::test::check_p3_base_and_alternate(decoder.get());
  hdrshot::test::check_p3_pq_a2b0_values(
      hdrshot::test::jpeg_icc(uhdr_dec_get_gainmap_image(decoder.get())));
  check_dual_metadata(decoder.get());
  HDRSHOT_CHECK(uhdr_dec_get_base_image(decoder.get()) != nullptr);
  HDRSHOT_CHECK(uhdr_dec_get_gainmap_image(decoder.get()) != nullptr);
  HDRSHOT_CHECK(uhdr_dec_get_gainmap_metadata(decoder.get()) != nullptr);
  HDRSHOT_CHECK(uhdr_decode(decoder.get()).error_code == UHDR_CODEC_OK);
  const auto* gain = uhdr_get_decoded_gainmap_image(decoder.get());
  HDRSHOT_CHECK(gain != nullptr && gain->fmt == UHDR_IMG_FMT_32bppRGBA8888);
  HDRSHOT_CHECK(gain->w == 32 && gain->h == 16);
  const auto* decoded = uhdr_get_decoded_image(decoder.get());
  HDRSHOT_CHECK(decoded != nullptr);
  HDRSHOT_CHECK(decoded->fmt == UHDR_IMG_FMT_64bppRGBAHalfFloat);
  HDRSHOT_CHECK(decoded->cg == UHDR_CG_DISPLAY_P3);
  HDRSHOT_CHECK(decoded->ct == UHDR_CT_LINEAR);
  HDRSHOT_CHECK(decoded->w == 32U && decoded->h == 16U);
  const auto* decoded_half = static_cast<const std::uint16_t*>(
      decoded->planes[UHDR_PLANE_PACKED]);
  HDRSHOT_CHECK(decoded_half != nullptr);
  for (std::size_t pixel : {std::size_t{8}, std::size_t{15}, std::size_t{24}}) {
    const auto actual = ExtendedP3Mapper::decode_binary16(decoded_half[pixel * 4U]);
    HDRSHOT_CHECK(actual.has_value());
    const auto expected = fixture.expected_linear[pixel];
    HDRSHOT_CHECK(std::abs(actual.value() - expected) < 0.22F);
  }
}

void rgb_gain_ranges_are_common_and_content_adaptive() {
  // First-pass R2/G4/B8 ranges differ; merge their union BEFORE quantization.
  // Realtime would use fixed 10000/203 instead of this content-adaptive range.
  LinearDisplayP3HalfImage image;
  image.size_px = {96, 32};
  image.maximum_linear_component = 8.0;
  image.rgba_half.assign(96U * 32U * 4U, 0);
  const float levels[] = {2.0F, 4.0F, 8.0F};
  for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 96; ++x) {
    const unsigned channel = x / 32;
    image.rgba_half[(y * 96U + x) * 4U + channel] =
        ExtendedP3Mapper::encode_binary16(levels[channel]);
    image.rgba_half[(y * 96U + x) * 4U + 3U] = 0x3C00U;
  }
  LibUltraHdrEncoder encoder;
  for (auto q : {UltraHdrJpegQuality::maximum, UltraHdrJpegQuality::balanced,
                 UltraHdrJpegQuality::compact}) {
    auto encoded = encoder.encode({&image, q});
    HDRSHOT_CHECK(encoded.has_value());
    DecoderHandle decoder{uhdr_create_decoder(), &uhdr_release_decoder};
    uhdr_compressed_image_t input{};
    input.data = encoded.value().bytes.data();
    input.data_sz = input.capacity = encoded.value().bytes.size();
    HDRSHOT_CHECK(uhdr_dec_set_image(decoder.get(), &input).error_code == UHDR_CODEC_OK);
    HDRSHOT_CHECK(uhdr_dec_set_out_img_format(decoder.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat).error_code == UHDR_CODEC_OK);
    HDRSHOT_CHECK(uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR).error_code == UHDR_CODEC_OK);
    HDRSHOT_CHECK(uhdr_dec_set_out_max_display_boost(decoder.get(), 100.0F).error_code == UHDR_CODEC_OK);
    HDRSHOT_CHECK(uhdr_dec_probe(decoder.get()).error_code == UHDR_CODEC_OK);
    check_dual_metadata(decoder.get());
    hdrshot::test::check_p3_base_and_alternate(decoder.get());
    const auto* meta = uhdr_dec_get_gainmap_metadata(decoder.get());
    HDRSHOT_CHECK(meta != nullptr);
    HDRSHOT_CHECK(std::abs(meta->hdr_capacity_max - 8.0F) < 1e-5F);
    const float expected_max[] = {9.015366F, 9.015366F, 9.015366F};
    for (unsigned c = 0; c < 3; ++c) {
      HDRSHOT_CHECK(std::abs(meta->min_content_boost[c] - 1.0F) < 1e-5F);
      // A fixed algorithm regression oracle, not an image-quality threshold.
      HDRSHOT_CHECK(std::abs(meta->max_content_boost[c] - expected_max[c]) < 0.04F);
      HDRSHOT_CHECK(meta->gamma[c] == 1.0F);
      HDRSHOT_CHECK(std::abs(meta->offset_sdr[c] - 1e-7F) < 1e-10F);
      HDRSHOT_CHECK(std::abs(meta->offset_hdr[c] - 1e-7F) < 1e-10F);
    }
    HDRSHOT_CHECK(uhdr_decode(decoder.get()).error_code == UHDR_CODEC_OK);
    const auto* gain = uhdr_get_decoded_gainmap_image(decoder.get());
    HDRSHOT_CHECK(gain && gain->fmt == UHDR_IMG_FMT_32bppRGBA8888);
    HDRSHOT_CHECK(gain->w == 96 && gain->h == 32);
    hdrshot::test::check_dual_reconstruction(decoder.get(), true);
  }
}

void constant_neutral_hdr_has_valid_equal_channel_ranges() {
  LinearDisplayP3HalfImage image;
  image.size_px = {32, 32}; image.maximum_linear_component = 4.0;
  image.rgba_half.assign(32U * 32U * 4U, ExtendedP3Mapper::encode_binary16(4.0F));
  for (std::size_t p = 0; p < 32U * 32U; ++p) image.rgba_half[p * 4U + 3U] = 0x3C00U;
  LibUltraHdrEncoder encoder;
  auto encoded = encoder.encode({&image, UltraHdrJpegQuality::balanced});
  HDRSHOT_CHECK(encoded.has_value());
  DecoderHandle decoder{uhdr_create_decoder(), &uhdr_release_decoder};
  uhdr_compressed_image_t input{};
  input.data = encoded.value().bytes.data(); input.data_sz = input.capacity = encoded.value().bytes.size();
  HDRSHOT_CHECK(uhdr_dec_set_image(decoder.get(), &input).error_code == UHDR_CODEC_OK);
  HDRSHOT_CHECK(uhdr_dec_set_out_img_format(decoder.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat).error_code == UHDR_CODEC_OK);
  HDRSHOT_CHECK(uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR).error_code == UHDR_CODEC_OK);
  HDRSHOT_CHECK(uhdr_dec_set_out_max_display_boost(decoder.get(), 4.0F).error_code == UHDR_CODEC_OK);
  HDRSHOT_CHECK(uhdr_dec_probe(decoder.get()).error_code == UHDR_CODEC_OK);
  check_dual_metadata(decoder.get());
  const auto* meta = uhdr_dec_get_gainmap_metadata(decoder.get());
  HDRSHOT_CHECK(meta && meta->hdr_capacity_max > meta->hdr_capacity_min);
  for (unsigned c = 0; c < 3; ++c) {
    HDRSHOT_CHECK(std::isfinite(meta->max_content_boost[c]));
    HDRSHOT_CHECK(meta->max_content_boost[c] > meta->min_content_boost[c]);
    HDRSHOT_CHECK(meta->min_content_boost[c] == meta->min_content_boost[0]);
    HDRSHOT_CHECK(meta->max_content_boost[c] == meta->max_content_boost[0]);
  }
  HDRSHOT_CHECK(uhdr_decode(decoder.get()).error_code == UHDR_CODEC_OK);
  const auto* decoded = uhdr_get_decoded_image(decoder.get());
  hdrshot::test::check_dual_reconstruction(decoder.get(), false);
  HDRSHOT_CHECK(decoded != nullptr);
  const auto* half = static_cast<const std::uint16_t*>(decoded->planes[UHDR_PLANE_PACKED]);
  for (unsigned c = 0; c < 3; ++c)
    HDRSHOT_CHECK(std::abs(ExtendedP3Mapper::decode_binary16(half[c]).value() - 4.0F) < 0.2F);
}

void quality_presets_are_decodable_and_reduce_size() {
  auto fixture = linear_gradient_fixture();
  auto rendered = render_fixture(fixture);
  LibUltraHdrEncoder encoder;
  std::vector<std::size_t> sizes;
  for (const auto quality : {
           UltraHdrJpegQuality::maximum,
           UltraHdrJpegQuality::balanced,
           UltraHdrJpegQuality::compact}) {
    const auto encoded = encoder.encode(UltraHdrEncodeRequest{&rendered, quality});
    HDRSHOT_CHECK(encoded.has_value());
    HDRSHOT_CHECK(is_uhdr_image(
        const_cast<std::uint8_t*>(encoded.value().bytes.data()),
        static_cast<int>(encoded.value().bytes.size())) == 1);
    sizes.push_back(encoded.value().bytes.size());
  }
  HDRSHOT_CHECK(sizes[2] <= sizes[1]);
  HDRSHOT_CHECK(sizes[1] <= sizes[0]);
}

void colored_p3_pixels_do_not_get_reinterpreted_as_bt2020() {
  LinearDisplayP3HalfImage image;
  image.size_px = {64, 64};
  image.reference_white_nits = 203.0;
  image.maximum_linear_component = 2.0;
  const float channels[4] = {2.0F, 0.8F, 0.3F, 1.0F};
  image.rgba_half.resize(64U * 64U * 4U);
  for (std::size_t at = 0; at < image.rgba_half.size(); ++at) {
    image.rgba_half[at] = ExtendedP3Mapper::encode_binary16(channels[at % 4]);
  }
  LibUltraHdrEncoder encoder;
  auto jpeg = encoder.encode({&image, UltraHdrJpegQuality::maximum});
  HDRSHOT_CHECK(jpeg.has_value());
  DecoderHandle decoder{uhdr_create_decoder(), &uhdr_release_decoder};
  uhdr_compressed_image_t input{};
  input.data = jpeg.value().bytes.data();
  input.capacity = input.data_sz = jpeg.value().bytes.size();
  HDRSHOT_CHECK(uhdr_dec_set_image(decoder.get(), &input).error_code == UHDR_CODEC_OK);
  HDRSHOT_CHECK(uhdr_dec_set_out_img_format(decoder.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat).error_code == UHDR_CODEC_OK);
  HDRSHOT_CHECK(uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR).error_code == UHDR_CODEC_OK);
  HDRSHOT_CHECK(uhdr_dec_set_out_max_display_boost(decoder.get(), 2.0F).error_code == UHDR_CODEC_OK);
  hdrshot::test::check_p3_base_and_alternate(decoder.get());
  HDRSHOT_CHECK(uhdr_decode(decoder.get()).error_code == UHDR_CODEC_OK);
  const auto* decoded = uhdr_get_decoded_image(decoder.get());
  HDRSHOT_CHECK(decoded != nullptr && decoded->cg == UHDR_CG_DISPLAY_P3);
  const auto* rgba = static_cast<const std::uint16_t*>(decoded->planes[UHDR_PLANE_PACKED]);
  const auto middle = (32U * decoded->stride[UHDR_PLANE_PACKED] + 32U) * 4U;
  bool within_tolerance = true;
  for (std::size_t channel = 0; channel < 3; ++channel) {
    const auto value = ExtendedP3Mapper::decode_binary16(rgba[middle + channel]);
    HDRSHOT_CHECK(value.has_value());
    std::cout << "colored channel=" << channel << " source=" << channels[channel]
              << " decoded=" << value.value() << '\n';
    // A lossy-gamut regression guard, not an assertion of exact PQ recovery.
    // Bound error to 5% of this fixture's peak; keep per-channel diagnostics.
    within_tolerance &= std::abs(value.value() - channels[channel]) <
        0.05F * static_cast<float>(image.maximum_linear_component);
  }
  HDRSHOT_CHECK(within_tolerance);
}

void saturated_p3_blocks_preserve_linear_rgb() {
  // 24 blocks: R/G/B/Y/M/C at linear .25/1/4/16. This is a shared
  // regression guard against matrix mistakes, not a perceptual quality gate.
  const float colors[6][3]={{1,0,0},{0,1,0},{0,0,1},{1,1,0},{1,0,1},{0,1,1}};
  const float levels[4]={.25F,1.F,4.F,16.F};
  LinearDisplayP3HalfImage image;
  image.size_px={384,256}; image.reference_white_nits=203.; image.maximum_linear_component=16.;
  image.rgba_half.resize(384U*256U*4U);
  for (unsigned y=0;y<256;++y) for (unsigned x=0;x<384;++x) {
    for (unsigned c=0;c<3;++c) image.rgba_half[(y*384U+x)*4+c]=
        ExtendedP3Mapper::encode_binary16(colors[x/64][c]*levels[y/64]);
    image.rgba_half[(y*384U+x)*4+3]=ExtendedP3Mapper::encode_binary16(1.F);
  }
  LibUltraHdrEncoder encoder;
  for (auto q : {UltraHdrJpegQuality::maximum,UltraHdrJpegQuality::balanced,UltraHdrJpegQuality::compact}) {
    auto encoded=encoder.encode({&image,q}); HDRSHOT_CHECK(encoded.has_value());
    DecoderHandle decoder{uhdr_create_decoder(),&uhdr_release_decoder};
    uhdr_compressed_image_t input{};
    input.data=encoded.value().bytes.data(); input.capacity=input.data_sz=encoded.value().bytes.size();
    HDRSHOT_CHECK(uhdr_dec_set_image(decoder.get(),&input).error_code==UHDR_CODEC_OK);
    HDRSHOT_CHECK(uhdr_dec_set_out_img_format(decoder.get(),UHDR_IMG_FMT_64bppRGBAHalfFloat).error_code==UHDR_CODEC_OK);
    HDRSHOT_CHECK(uhdr_dec_set_out_color_transfer(decoder.get(),UHDR_CT_LINEAR).error_code==UHDR_CODEC_OK);
    HDRSHOT_CHECK(uhdr_dec_set_out_max_display_boost(decoder.get(),16.F).error_code==UHDR_CODEC_OK);
    HDRSHOT_CHECK(uhdr_decode(decoder.get()).error_code==UHDR_CODEC_OK);
    const auto* decoded=uhdr_get_decoded_image(decoder.get());
    HDRSHOT_CHECK(decoded && decoded->cg==UHDR_CG_DISPLAY_P3);
    const auto* half=static_cast<const std::uint16_t*>(decoded->planes[UHDR_PLANE_PACKED]);
    double worst=0;
    for (unsigned y=0;y<256;++y) for (unsigned x=0;x<384;++x) {
      if (x%64<8 || x%64>=56 || y%64<8 || y%64>=56) continue;
      for (unsigned c=0;c<3;++c) {
        const float value=ExtendedP3Mapper::decode_binary16(half[(y*decoded->stride[UHDR_PLANE_PACKED]+x)*4+c]).value();
        worst=std::max(worst,static_cast<double>(std::abs(value-colors[x/64][c]*levels[y/64])/levels[y/64]));
      }
    }
    std::cout<<"saturated 24 blocks q="<<ultra_hdr_jpeg_quality_value(q)<<" max_linear_RGB_relative_error="<<worst<<'\n';
    HDRSHOT_CHECK(worst<.05);
  }
}

void sdr_input_is_an_ordinary_jpeg() {
  LibUltraHdrEncoder encoder;
  // Flat P3 fixtures include black/white and wide-gamut saturated primaries.
  const float colors[][3] = {{0,0,0}, {.25F,.25F,.25F}, {1,1,1},
      {1,0,0}, {0,1,0}, {0,0,1}, {1,1,0}, {1,0,1}, {0,1,1}, {.5F,.25F,.125F}};
  for (auto quality : {UltraHdrJpegQuality::maximum, UltraHdrJpegQuality::balanced,
                       UltraHdrJpegQuality::compact}) {
    int worst = 0;
    for (const auto& color : colors) {
      LinearDisplayP3HalfImage image;
      image.size_px = {37, 19};  // Odd dimensions exercise row strides/edge MCUs.
      image.maximum_linear_component = *std::max_element(color, color + 3);
      image.rgba_half.resize(37U * 19U * 4U);
      for (std::size_t p = 0; p < 37U * 19U; ++p) {
        for (std::size_t c = 0; c < 3; ++c)
          image.rgba_half[p * 4U + c] = ExtendedP3Mapper::encode_binary16(color[c]);
        image.rgba_half[p * 4U + 3U] = 0x3C00U;
      }
      auto encoded = encoder.encode({&image, quality});
      HDRSHOT_CHECK(encoded.has_value());
      HDRSHOT_CHECK(encoded.value().kind == JpegOutputKind::display_p3_sdr);
      HDRSHOT_CHECK(is_uhdr_image(encoded.value().bytes.data(),
          static_cast<int>(encoded.value().bytes.size())) == 0);
      for (const auto forbidden : {"hdr-gain-map", "urn:iso:std:iso:ts:21496:-1",
                                   "GainMap", "MPF", "PQ Transfer"})
        HDRSHOT_CHECK(!contains_ascii(encoded.value().bytes, forbidden));
      const std::array<std::uint8_t, 2> eoi{0xff, 0xd9};
      const auto end = std::search(encoded.value().bytes.begin(), encoded.value().bytes.end(),
          eoi.begin(), eoi.end());
      HDRSHOT_CHECK(end == encoded.value().bytes.end() - 2);  // No second image.
      uhdr_mem_block_t block{};
      block.data = encoded.value().bytes.data(); block.data_sz = encoded.value().bytes.size();
      const auto profile = hdrshot::test::jpeg_icc(&block);
      HDRSHOT_CHECK(profile.size() > 128U);
      HDRSHOT_CHECK(hdrshot::test::icc_tag(profile, "cicp").empty());
      const auto trc = hdrshot::test::icc_tag(profile, "rTRC");
      HDRSHOT_CHECK(trc.size() >= 12U);
      HDRSHOT_CHECK(std::ranges::equal(profile, DisplayP3IccProfile::bytes()));
      // Compare D50-adapted P3 primaries against our fixed CC0 PNG profile.
      for (const auto primary : {"rXYZ", "gXYZ", "bXYZ"}) {
        const auto xyz = hdrshot::test::icc_tag(profile, primary);
        HDRSHOT_CHECK(xyz.size() == 20U);
        HDRSHOT_CHECK(std::ranges::equal(xyz,
            hdrshot::test::icc_tag(DisplayP3IccProfile::bytes(), primary)));
      }
      ultrahdr::JpegDecoderHelper decoder;
      HDRSHOT_CHECK(decoder.decompressImage(encoded.value().bytes.data(),
          encoded.value().bytes.size(), ultrahdr::DECODE_TO_RGB_CS).error_code == UHDR_CODEC_OK);
      HDRSHOT_CHECK(decoder.getXMPSize() == 0 && decoder.getIsoMetadataSize() == 0);
      const auto decoded = decoder.getDecompressedImage();
      HDRSHOT_CHECK(decoded.w == 37 && decoded.h == 19);
      const unsigned channels = decoded.fmt == UHDR_IMG_FMT_24bppRGB888 ? 3U : 4U;
      const auto* rgb = static_cast<const std::uint8_t*>(decoded.planes[UHDR_PLANE_PACKED]);
      for (unsigned y = 0; y < 19; ++y) for (unsigned x = 0; x < 37; ++x)
        for (unsigned c = 0; c < 3; ++c) {
          const double v = color[c];
          const int expected = static_cast<int>(std::lround(255.0 *
              (v <= .0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - .055)));
          worst = std::max(worst, std::abs(expected -
              rgb[(y * decoded.stride[UHDR_PLANE_PACKED] + x) * channels + c]));
        }
    }
    std::cout << "pure SDR JPEG q=" << ultra_hdr_jpeg_quality_value(quality)
              << " max_RGB8_code_error=" << worst << '\n';
    HDRSHOT_CHECK(worst <= 3);  // Numerical regression guard, not a JND claim.
  }
}

void first_half_step_above_one_still_encodes_hdr() {
  LinearDisplayP3HalfImage image;
  image.size_px = {32, 32};
  image.maximum_linear_component = ExtendedP3Mapper::decode_binary16(0x3C01U).value();
  image.rgba_half.assign(32U * 32U * 4U, 0x3C01U);
  for (std::size_t p = 0; p < 32U * 32U; ++p) image.rgba_half[p * 4U + 3U] = 0x3C00U;
  LibUltraHdrEncoder encoder;
  auto encoded = encoder.encode({&image, UltraHdrJpegQuality::balanced});
  HDRSHOT_CHECK(encoded.has_value() && encoded.value().kind == JpegOutputKind::ultra_hdr);
  DecoderHandle decoder{uhdr_create_decoder(), &uhdr_release_decoder};
  uhdr_compressed_image_t input{};
  input.data = encoded.value().bytes.data(); input.data_sz = input.capacity = encoded.value().bytes.size();
  HDRSHOT_CHECK(uhdr_dec_set_image(decoder.get(), &input).error_code == UHDR_CODEC_OK);
  HDRSHOT_CHECK(uhdr_dec_set_out_img_format(decoder.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat).error_code == UHDR_CODEC_OK);
  HDRSHOT_CHECK(uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR).error_code == UHDR_CODEC_OK);
  HDRSHOT_CHECK(uhdr_dec_set_out_max_display_boost(decoder.get(), 2.F).error_code == UHDR_CODEC_OK);
  HDRSHOT_CHECK(uhdr_dec_probe(decoder.get()).error_code == UHDR_CODEC_OK);
  const auto* meta = uhdr_dec_get_gainmap_metadata(decoder.get());
  HDRSHOT_CHECK(meta && meta->hdr_capacity_max > meta->hdr_capacity_min);
  HDRSHOT_CHECK(uhdr_decode(decoder.get()).error_code == UHDR_CODEC_OK);
  // Reusing the same encoder object after HDR must not retain HDR metadata.
  image.maximum_linear_component = 1.0;
  std::fill(image.rgba_half.begin(), image.rgba_half.end(), 0x3C00U);
  auto sdr = encoder.encode({&image, UltraHdrJpegQuality::balanced});
  HDRSHOT_CHECK(sdr.has_value() && sdr.value().kind == JpegOutputKind::display_p3_sdr);
  HDRSHOT_CHECK(is_uhdr_image(sdr.value().bytes.data(), static_cast<int>(sdr.value().bytes.size())) == 0);
}

void invalid_sdr_samples_are_not_silently_clipped() {
  LinearDisplayP3HalfImage image;
  image.size_px = {2, 2}; image.maximum_linear_component = 1.0;
  image.rgba_half.assign(16U, 0x3C00U);
  LibUltraHdrEncoder encoder;
  for (const auto bits : std::array<std::uint16_t, 4>{0x3C01U, 0x7C00U, 0x7E00U, 0xBC00U}) {
    image.rgba_half[8] = bits;
    auto result = encoder.encode({&image, UltraHdrJpegQuality::balanced});
    HDRSHOT_CHECK(!result.has_value());
    HDRSHOT_CHECK(result.error().safe_context.at("stage") == "sdr_sample_out_of_range");
  }
  image.rgba_half[8] = 0x8000U;
  HDRSHOT_CHECK(encoder.encode({&image, UltraHdrJpegQuality::balanced}).has_value());
}

void explicit_edge_clipping_stays_sdr() {
  LinearDisplayP3HalfImage image;
  image.size_px = {8,8}; image.maximum_linear_component=2.5;
  image.source_visible_maximum_linear_component=1.0;
  image.rgba_half.assign(256,0x3C00);
  for (std::size_t p=0;p<64;++p) for(std::size_t c=0;c<3;++c)
    image.rgba_half[p*4+c]=ExtendedP3Mapper::encode_binary16(2.5F);
  LibUltraHdrEncoder encoder;
  auto result=encoder.encode({&image,UltraHdrJpegQuality::maximum});
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value().kind==JpegOutputKind::display_p3_sdr);
  HDRSHOT_CHECK(is_uhdr_image(result.value().bytes.data(),int(result.value().bytes.size()))==0);
  ultrahdr::JpegDecoderHelper decoder;
  HDRSHOT_CHECK(decoder.decompressImage(result.value().bytes.data(),result.value().bytes.size(),
      ultrahdr::DECODE_TO_RGB_CS).error_code==UHDR_CODEC_OK);
  const auto decoded=decoder.getDecompressedImage();
  const auto* rgb=static_cast<const std::uint8_t*>(decoded.planes[UHDR_PLANE_PACKED]);
  HDRSHOT_CHECK(rgb[0]==255 && rgb[1]==255 && rgb[2]==255);
  image.source_visible_maximum_linear_component=-1;
  HDRSHOT_CHECK(!encoder.encode({&image,UltraHdrJpegQuality::maximum}).has_value());
}

}  // namespace

int main(int argc, char** argv) {
  using hdrshot::test::TestCase;
  // Optional offline validation of real exported JPEGs, using the same strict
  // structure/ICC tests and two independent metadata parses as the unit suite.
  if (argc > 1) {
    std::vector<TestCase> files;
    for (int i = 1; i < argc; ++i) {
      const std::string path = argv[i];
      files.push_back({argv[i], [path] {
        std::ifstream input_file(path, std::ios::binary);
        HDRSHOT_CHECK(input_file.good());
        std::vector<char> bytes((std::istreambuf_iterator<char>(input_file)), {});
        HDRSHOT_CHECK(!bytes.empty());
        DecoderHandle decoder{uhdr_create_decoder(), &uhdr_release_decoder};
        HDRSHOT_CHECK(decoder != nullptr);
        uhdr_compressed_image_t input{};
        input.data = bytes.data(); input.data_sz = input.capacity = bytes.size();
        HDRSHOT_CHECK(uhdr_dec_set_image(decoder.get(), &input).error_code == UHDR_CODEC_OK);
        HDRSHOT_CHECK(uhdr_dec_set_out_img_format(decoder.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat).error_code == UHDR_CODEC_OK);
        HDRSHOT_CHECK(uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR).error_code == UHDR_CODEC_OK);
        HDRSHOT_CHECK(uhdr_dec_set_out_max_display_boost(decoder.get(), 100).error_code == UHDR_CODEC_OK);
        HDRSHOT_CHECK(uhdr_dec_probe(decoder.get()).error_code == UHDR_CODEC_OK);
        hdrshot::test::check_p3_base_and_alternate(decoder.get());
        hdrshot::test::check_p3_pq_a2b0_values(
            hdrshot::test::jpeg_icc(uhdr_dec_get_gainmap_image(decoder.get())));
        HDRSHOT_CHECK(uhdr_decode(decoder.get()).error_code == UHDR_CODEC_OK);
        hdrshot::test::check_dual_reconstruction(decoder.get(), false);
      }});
    }
    return hdrshot::test::run(files);
  }
  return hdrshot::test::run(std::vector<TestCase>{
      {"AA residual edges clip in SDR JPEG", explicit_edge_clipping_stays_sdr},
      {"SDR input is an ordinary JPEG", sdr_input_is_an_ordinary_jpeg},
      {"First half step above one remains HDR", first_half_step_above_one_still_encodes_hdr},
      {"SDR invalid samples are rejected", invalid_sdr_samples_are_not_silently_clipped},
      {"CPU Ultra HDR input is 203-nit linear P3",
       cpu_renderer_outputs_fixed_203_nit_linear_p3},
      {"CPU rejects invalid pixel ownership", cpu_renderer_rejects_invalid_pixel_ownership},
      {"libultrahdr writes ISO and XMP and decodes RGB HDR",
       codec_writes_dual_metadata_and_decodes_linear_hdr},
      {"RGB gain uses common content-adaptive two-pass range",
       rgb_gain_ranges_are_common_and_content_adaptive},
      {"Constant neutral HDR keeps valid equal channel ranges",
       constant_neutral_hdr_has_valid_equal_channel_ranges},
      {"Ultra HDR quality presets decode and reduce size",
       quality_presets_are_decodable_and_reduce_size},
      {"Colored P3 values survive Ultra HDR", colored_p3_pixels_do_not_get_reinterpreted_as_bt2020},
      {"Saturated P3 blocks preserve linear RGB", saturated_p3_blocks_preserve_linear_rgb},
  });
}
