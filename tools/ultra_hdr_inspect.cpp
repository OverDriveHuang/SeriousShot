// Read-only JPEG inspection against the project's linked libultrahdr.
// No capture, re-encode, ImageIO, or output-image writes.
#include <ultrahdr_api.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

static void check(uhdr_error_info_t s) {
  if (s.error_code != UHDR_CODEC_OK)
    throw std::runtime_error(s.has_detail ? s.detail : "decoder failed");
}
static float half(std::uint16_t h) {
  const auto exponent = (h >> 10U) & 31U;
  const auto fraction = h & 1023U;
  const float value = exponent == 0 ? std::ldexp(float(fraction), -24)
      : exponent == 31 ? (fraction ? std::numeric_limits<float>::quiet_NaN()
                                   : std::numeric_limits<float>::infinity())
      : std::ldexp(float(1024U + fraction), int(exponent) - 25);
  return h & 0x8000U ? -value : value;
}

int main(int argc, char** argv) try {
  if (argc != 2) return 2;
  std::ifstream file(argv[1], std::ios::binary);
  if (!file) throw std::runtime_error("cannot open input");
  std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
  uhdr_compressed_image_t image{};
  image.data = bytes.data(); image.data_sz = image.capacity = bytes.size();
  image.cg = UHDR_CG_UNSPECIFIED; image.ct = UHDR_CT_UNSPECIFIED;
  image.range = UHDR_CR_UNSPECIFIED;
  std::cout << std::setprecision(10);
  for (float boost : {1.F, 100.F}) {
    std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)>
        dec(uhdr_create_decoder(), uhdr_release_decoder);
    if (!dec) throw std::runtime_error("create decoder failed");
    check(uhdr_dec_set_image(dec.get(), &image));
    check(uhdr_dec_set_out_img_format(dec.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat));
    check(uhdr_dec_set_out_color_transfer(dec.get(), UHDR_CT_LINEAR));
    check(uhdr_dec_set_out_max_display_boost(dec.get(), boost));
    check(uhdr_dec_probe(dec.get()));
    const auto* m = uhdr_dec_get_gainmap_metadata(dec.get());
    if (!m) throw std::runtime_error("no metadata");
    if (boost == 1.F) {
      std::cout << "capacity_min=" << m->hdr_capacity_min
                << " capacity_max=" << m->hdr_capacity_max
                << " use_base_cg=" << m->use_base_cg << '\n';
      for (int c = 0; c < 3; ++c)
        std::cout << "channel=" << c << " gain_min=" << m->min_content_boost[c]
                  << " gain_max=" << m->max_content_boost[c]
                  << " gamma=" << m->gamma[c] << " offset_sdr=" << m->offset_sdr[c]
                  << " offset_hdr=" << m->offset_hdr[c] << '\n';
      std::cout << "base_jpeg_bytes=" << uhdr_dec_get_base_image(dec.get())->data_sz
                << " gain_jpeg_bytes=" << uhdr_dec_get_gainmap_image(dec.get())->data_sz << '\n';
    }
    check(uhdr_decode(dec.get()));
    const auto* out = uhdr_get_decoded_image(dec.get());
    if (!out || out->fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat ||
        out->ct != UHDR_CT_LINEAR || out->cg != UHDR_CG_DISPLAY_P3)
      throw std::runtime_error("unexpected output format/transfer/gamut");
    const auto* data = static_cast<const std::uint16_t*>(out->planes[UHDR_PLANE_PACKED]);
    double sum = 0; float maximum = 0; std::uint64_t above = 0, nonfinite = 0;
    unsigned int mx = 0, my = 0, mc = 0;
    for (unsigned int y = 0; y < out->h; ++y) for (unsigned int x = 0; x < out->w; ++x) {
      float peak = 0;
      for (unsigned int c = 0; c < 3; ++c) {
        float v = half(data[(std::size_t(y) * out->stride[UHDR_PLANE_PACKED] + x) * 4U + c]);
        if (!std::isfinite(v)) ++nonfinite;
        peak = std::max(peak, v);
        if (v > maximum) { maximum = v; mx = x; my = y; mc = c; }
      }
      sum += peak; above += peak > 1;
    }
    const auto* gain = uhdr_get_decoded_gainmap_image(dec.get());
    if (!gain || gain->fmt != UHDR_IMG_FMT_8bppYCbCr400)
      throw std::runtime_error("unexpected gain map format");
    unsigned int gain_min = 255, gain_max = 0; std::uint64_t gain_nonzero = 0;
    for (unsigned int y = 0; y < gain->h; ++y) for (unsigned int x = 0; x < gain->w; ++x) {
      auto v = static_cast<const std::uint8_t*>(gain->planes[0])[std::size_t(y) * gain->stride[0] + x];
      gain_min = std::min(gain_min, unsigned(v)); gain_max = std::max(gain_max, unsigned(v));
      gain_nonzero += v != 0;
    }
    std::cout << "decode_boost=" << boost << " size=" << out->w << 'x' << out->h
              << " max_linear_component=" << maximum << " max_component_nits=" << maximum * 203.0
              << " peak_xyc=" << mx << ',' << my << ',' << mc
              << " mean_peak=" << sum / (double(out->w) * out->h)
              << " pixels_above_1=" << above << " nonfinite=" << nonfinite
              << " gain_code_min=" << gain_min << " gain_code_max=" << gain_max
              << " gain_nonzero_pixels=" << gain_nonzero << '\n';
  }
  return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
