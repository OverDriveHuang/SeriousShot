#include "domain/png/png_encoder.hpp"
#include "domain/png/streaming_png_encoder.hpp"
#include "domain/color/display_p3_icc_profile.hpp"
#include "png_benchmark_corpus.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <fstream>
#include <iterator>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <zlib.h>
#ifdef HDRSHOT_PNG_PARAMETER_SWEEP

// Only the fixed-parameter diagnostic target renames deflateInit2_. Production
// uses its adaptive policy. Undefine locally to call the real zlib initializer.
#undef deflateInit2_
extern "C" int deflateInit2_(z_streamp, int, int, int, int, int, const char*, int);
static int experiment_level = 6;
static int experiment_memory = 8;
static int experiment_strategy = Z_DEFAULT_STRATEGY;
extern "C" int hdrshot_probe_deflate_init(z_streamp stream, int, int, int, int, int,
                                          const char* version, int size) {
  return deflateInit2_(stream, experiment_level, Z_DEFLATED, 15,
                      experiment_memory, experiment_strategy, version, size);
}
#endif
#ifdef __APPLE__
#include <compression.h>
#endif

namespace {

using namespace hdrshot;

using benchmark::CorpusCase;
using benchmark::make_corpus;

// Read-only codec fixture: retain the original RGB code values, without a color
// transform. This tests compression, not capture/ICC semantics or tone mapping.
CorpusCase read_rgb16_png(const char* path) {
  std::ifstream file(path, std::ios::binary);
  std::vector<std::uint8_t> png((std::istreambuf_iterator<char>(file)), {});
  const std::array<std::uint8_t,8> signature{137,80,78,71,13,10,26,10};
  if (png.size()<33 || !std::equal(signature.begin(),signature.end(),png.begin()))
    throw std::runtime_error("input is not a PNG");
  auto u32=[&](std::size_t p) { return (std::uint32_t(png.at(p))<<24U) |
      (std::uint32_t(png.at(p+1))<<16U) | (std::uint32_t(png.at(p+2))<<8U) | png.at(p+3); };
  const auto width=u32(16),height=u32(20);
  const auto channels=png[25]==2 ? 3U : png[25]==6 ? 4U : 0U;
  if (!width || !height || width>16384 || height>16384 || png[24]!=16 || !channels || png[28]!=0)
    throw std::runtime_error("fixture needs non-interlaced RGB/RGBA16 PNG <=16384");
  std::vector<std::uint8_t> idat;
  for(std::size_t p=8;p<png.size();) {
    if(png.size()-p<12) throw std::runtime_error("chunk truncated");
    const auto n=std::size_t(u32(p));
    if(n>png.size()-p-12 || crc32(0,png.data()+p+4,static_cast<uInt>(n+4))!=u32(p+n+8))
      throw std::runtime_error("chunk size/CRC");
    if(std::string(reinterpret_cast<const char*>(png.data()+p+4),4)=="IDAT")
      idat.insert(idat.end(),png.begin()+static_cast<std::ptrdiff_t>(p+8),png.begin()+static_cast<std::ptrdiff_t>(p+8+n));
    p+=n+12;
  }
  const auto pixel_bytes=channels*2U;
  const auto stride=std::size_t(width)*pixel_bytes;
  std::vector<std::uint8_t> raw((stride+1)*height),previous(stride),current(stride);
  uLongf size=raw.size();
  if(uncompress(raw.data(),&size,idat.data(),idat.size())!=Z_OK || size!=raw.size())
    throw std::runtime_error("inflate input fixture");
  CorpusCase fixture{"file_rgb16_codes",width,height,{}};
  fixture.rgb.reserve(std::size_t(width)*height*3U);
  for(std::uint32_t y=0;y<height;++y) {
    const auto offset=std::size_t(y)*(stride+1);
    const auto filter=raw[offset];
    if(filter>4) throw std::runtime_error("filter input fixture");
    for(std::size_t x=0;x<stride;++x) {
      const int a=x>=pixel_bytes?current[x-pixel_bytes]:0,b=previous[x],c=x>=pixel_bytes?previous[x-pixel_bytes]:0;
      int predictor=0;
      if(filter==1) predictor=a;
      if(filter==2) predictor=b;
      if(filter==3) predictor=(a+b)/2;
      if(filter==4) { const int p=a+b-c,pa=std::abs(p-a),pb=std::abs(p-b),pc=std::abs(p-c); predictor=pa<=pb && pa<=pc?a:pb<=pc?b:c; }
      current[x]=static_cast<std::uint8_t>(raw[offset+1+x]+predictor);
    }
    for(std::uint32_t x=0;x<width;++x) {
      const auto p=std::size_t(x)*pixel_bytes;
      for(unsigned c=0;c<3;++c) fixture.rgb.push_back(static_cast<std::uint16_t>((std::uint16_t(current[p+2*c])<<8U)|current[p+2*c+1]));
      if(channels==4 && (current[p+6]!=255 || current[p+7]!=255)) throw std::runtime_error("non-opaque fixture");
    }
    previous.swap(current);
  }
  return fixture;
}

bool metadata_matches(const std::vector<std::uint8_t>& png, const PngColorMetadata& metadata,
                      const std::string_view software) {
  auto u32 = [&](std::size_t p) { return (std::uint32_t(png[p])<<24U) |
      (std::uint32_t(png[p+1])<<16U) | (std::uint32_t(png[p+2])<<8U) | png[p+3]; };
  bool cicp=false,icc=false,clli=false,text=false;
  for(std::size_t p=8;p+12<=png.size();) {
    const auto n=std::size_t(u32(p));
    if(n>png.size()-p-12 || crc32(0,png.data()+p+4,static_cast<uInt>(n+4))!=u32(p+8+n)) return false;
    const std::string type(reinterpret_cast<const char*>(png.data()+p+4),4);
    const auto begin=p+8;
    if(type=="IHDR") {
      if(n!=13 || png[begin+8]!=16 || png[begin+9]!=2 || png[begin+12]!=0) return false;
    } else if(type=="cICP") {
      if(cicp || n!=4 || !metadata.cicp) return false;
      const auto& c=*metadata.cicp;
      if(png[begin]!=c.color_primaries || png[begin+1]!=c.transfer_function ||
         png[begin+2]!=c.matrix_coefficients || png[begin+3]!=c.video_full_range_flag) return false;
      cicp=true;
    } else if(type=="cLLI") {
      if(clli || n!=8 || !metadata.content_light || u32(begin)!=metadata.content_light->max_cll_x10000 ||
          u32(begin+4)!=metadata.content_light->max_fall_x10000) return false;
      clli=true;
    } else if(type=="iCCP") {
      if(icc || !metadata.icc) return false;
      auto start=begin; while(start<begin+n && png[start]!=0) ++start;
      if(start+2>begin+n || png[start+1]!=0) return false;
      start+=2;
      const auto expected=metadata.icc->profile_bytes;
      std::vector<std::uint8_t> decoded(expected.size()); uLongf size=decoded.size();
      if(uncompress(decoded.data(),&size,png.data()+start,begin+n-start)!=Z_OK || size!=expected.size() ||
          !std::equal(decoded.begin(),decoded.end(),expected.begin())) return false;
      icc=true;
    } else if(type=="tEXt") {
      const auto expected=std::string("Software\0",9)+std::string(software);
      if(n!=expected.size() || !std::equal(expected.begin(),expected.end(),png.begin()+static_cast<std::ptrdiff_t>(begin))) return false;
      text=true;
    } else if(type!="IDAT" && type!="IEND") return false;
    p+=n+12;
  }
  return cicp==metadata.cicp.has_value() && icc==metadata.icc.has_value() &&
      clli==metadata.content_light.has_value() && text;
}

template <typename Callable>
auto timed(Callable&& callable) {
  const auto start = std::chrono::steady_clock::now();
  auto result = callable();
  const auto elapsed = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start).count();
  return std::pair{std::move(result), elapsed};
}

bool exact_decode(const CorpusCase& fixture, const std::vector<std::uint8_t>& filtered) {
  const auto stride = std::size_t(fixture.width) * 6;
  std::vector<std::uint8_t> previous(stride), current(stride);
  for (std::uint32_t y = 0; y < fixture.height; ++y) {
    const auto row = std::size_t(y) * (stride + 1);
    const auto filter = filtered[row];
    for (std::size_t x = 0; x < stride; ++x) {
      const int a = x >= 6 ? current[x-6] : 0, b = previous[x], c = x >= 6 ? previous[x-6] : 0;
      int prediction = 0;
      if (filter == 1) prediction = a;
      else if (filter == 2) prediction = b;
      else if (filter == 4) {
        const int p = a + b - c, pa = std::abs(p-a), pb = std::abs(p-b), pc = std::abs(p-c);
        prediction = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      } else if (filter != 0) return false;
      current[x] = static_cast<std::uint8_t>(filtered[row + 1 + x] + prediction);
      const auto value = fixture.rgb[std::size_t(y) * fixture.width * 3 + x / 2];
      const auto expected = x % 2 ? value & 255U : value >> 8U;
      if (current[x] != expected) return false;
    }
    previous.swap(current);
  }
  return true;
}

#ifdef __APPLE__
// Diagnostic-only alternative. Apple's ZLIB stream is raw DEFLATE, so verify
// with inflateInit2(-15), not PNG's zlib wrapper. No production backend switch.
std::vector<std::uint8_t> native_deflate(const std::vector<std::uint8_t>& raw,
                                        const std::size_t row_bytes) {
  compression_stream stream{};
  if (compression_stream_init(&stream, COMPRESSION_STREAM_ENCODE, COMPRESSION_ZLIB)
      == COMPRESSION_STATUS_ERROR) return {};
  std::vector<std::uint8_t> result;
  std::array<std::uint8_t, 65536> output;
  std::size_t submitted = 0;
  bool complete = false;
  while (!complete) {
    if (!stream.src_size && submitted < raw.size()) {
      stream.src_ptr = raw.data() + submitted;
      stream.src_size = std::min(row_bytes, raw.size() - submitted);
      submitted += stream.src_size;
    }
    stream.dst_ptr = output.data();
    stream.dst_size = output.size();
    const auto before = stream.src_size;
    const auto status = compression_stream_process(&stream,
        submitted == raw.size() ? COMPRESSION_STREAM_FINALIZE : 0);
    const auto written = output.size() - stream.dst_size;
    result.insert(result.end(), output.begin(), output.begin() + static_cast<std::ptrdiff_t>(written));
    if (status == COMPRESSION_STATUS_ERROR ||
        (status != COMPRESSION_STATUS_END && before == stream.src_size && !written)) {
      result.clear();
      break;
    }
    complete = status == COMPRESSION_STATUS_END;
  }
  compression_stream_destroy(&stream);
  return result;
}

bool verify_native(const std::vector<std::uint8_t>& data, const std::vector<std::uint8_t>& raw) {
  std::vector<std::uint8_t> decoded(raw.size());
  z_stream stream{};
  if (inflateInit2(&stream, -15) != Z_OK) return false;
  stream.next_in = const_cast<Bytef*>(data.data());
  stream.avail_in = static_cast<uInt>(data.size());
  stream.next_out = decoded.data();
  stream.avail_out = static_cast<uInt>(decoded.size());
  const auto status = inflate(&stream, Z_FINISH);
  const auto count = stream.total_out;
  inflateEnd(&stream);
  return status == Z_STREAM_END && count == raw.size() && decoded == raw;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  const bool file_mode = argc >= 3 && std::string(argv[1]).starts_with("--file");
  const bool large = file_mode || (argc >= 2 && std::string(argv[1]).starts_with("--5k"));
  const bool native = argc >= 2 && std::string(argv[1]).find("native") != std::string::npos;
  const bool production = argc >= 2 && std::string(argv[1]).find("production") != std::string::npos;
#ifdef HDRSHOT_PNG_PARAMETER_SWEEP
  if (!large || argc < 3 || argc > 5) {
    std::cerr << "usage: parameter_benchmark --5k[-photo][-12|-16][-sdr] level [memory=8] [strategy=0]\n";
    return 2;
  }
  experiment_level = std::atoi(argv[2]);
  if (argc >= 4) experiment_memory = std::atoi(argv[3]);
  if (argc >= 5) experiment_strategy = std::atoi(argv[4]);
  if (experiment_level < 1 || experiment_level > 9 || experiment_memory < 1 || experiment_memory > 9 ||
      experiment_strategy < 0 || experiment_strategy > Z_FIXED || native) return 2;
#endif
  if (large || native) {
    // Offline only; compression experiments never modify production precision.
    const auto bits = std::string(argv[1]).find("16") != std::string::npos ? 16U :
        std::string(argv[1]).find("12") != std::string::npos ? 12U : 10U;
#ifdef HDRSHOT_PNG_PARAMETER_SWEEP
    std::cout << (production ? "case,bits,encode_ms,png_bytes,validation\n" :
        "case,bits,range,level,memory,strategy,encode_ms,png_bytes,exact\n") << std::flush;
#else
    std::cout << (production ? "case,bits,encode_ms,png_bytes,validation\n" :
        "case,bits,encode_ms,png_bytes,deflate_level,deflate_ms,deflate_bytes\n") << std::flush;
#endif
    std::vector<CorpusCase> corpus;
    try {
      corpus = file_mode ? std::vector<CorpusCase>{read_rgb16_png(argv[2])} :
          make_corpus(large ? 5120 : 960, large ? 2880 : 540);
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
    if (!file_mode && std::string(argv[1]).find("mixed") != std::string::npos) {
      auto smooth_first = corpus[1];
      auto texture_first = corpus[3];
      smooth_first.name = "smooth_then_texture";
      texture_first.name = "texture_then_smooth";
      const auto middle = std::size_t(smooth_first.width) * (smooth_first.height / 2U) * 3U;
      std::copy(corpus[3].rgb.begin()+static_cast<std::ptrdiff_t>(middle),corpus[3].rgb.end(),
          smooth_first.rgb.begin()+static_cast<std::ptrdiff_t>(middle));
      std::copy(corpus[1].rgb.begin()+static_cast<std::ptrdiff_t>(middle),corpus[1].rgb.end(),
          texture_first.rgb.begin()+static_cast<std::ptrdiff_t>(middle));
      corpus.clear();
      corpus.push_back(std::move(smooth_first));
      corpus.push_back(std::move(texture_first));
    }
    for (auto fixture : corpus) {
      if (std::string(argv[1]).find("photo") != std::string::npos && fixture.name != "seeded_photo_like") continue;
      if (bits != 16U) for (auto& value : fixture.rgb) {
        const auto levels = (1U << bits) - 1U;
        const auto q = (static_cast<std::uint32_t>(value) * levels + 32767U) / 65535U;
        value = static_cast<std::uint16_t>((q * 65535U + levels / 2U) / levels);
      }
      auto metadata = PngColorMetadata{kDisplayP3PqFullRange, std::nullopt};
      std::string_view software;
      const bool sdr = std::string(argv[1]).find("sdr") != std::string::npos;
      if (production
#ifdef HDRSHOT_PNG_PARAMETER_SWEEP
          || true
#endif
      ) {
      metadata = sdr ? PngColorMetadata{kDisplayP3SrgbFullRange,
          IccProfilePayload{"Display P3", DisplayP3IccProfile::bytes()}} :
          PngColorMetadata{kDisplayP3PqFullRange, std::nullopt, ContentLightLevelInfo{10000000,4000000}};
      software = "SeriousShot; build PNG encoder experiment";
      }
      auto [encoded, encode_ms] = timed([&] {
        return StreamingPngEncoder::encode_16bit_rgb({fixture.width, fixture.height,
            metadata,
            [&](std::uint32_t y) -> Result<std::vector<std::uint16_t>, Error> {
              const auto begin = fixture.rgb.begin() + static_cast<std::ptrdiff_t>(y * fixture.width * 3U);
              return Result<std::vector<std::uint16_t>, Error>::success({begin, begin + fixture.width * 3U});
            }, software, static_cast<std::uint8_t>(bits)});
      });
      if (!encoded) return 2;
      const auto& png = encoded.value().bytes;
      std::vector<std::uint8_t> idat;
      for (std::size_t p = 8; p + 12 <= png.size();) {
        const auto n = (std::size_t(png[p]) << 24U) | (std::size_t(png[p+1]) << 16U) |
            (std::size_t(png[p+2]) << 8U) | png[p+3];
        if (p + n + 12 > png.size()) return 2;
        if (std::string(reinterpret_cast<const char*>(png.data() + p + 4), 4) == "IDAT")
          idat.insert(idat.end(), png.begin() + static_cast<std::ptrdiff_t>(p+8),
                      png.begin() + static_cast<std::ptrdiff_t>(p+8+n));
        p += n + 12;
      }
      uLongf count = (static_cast<uLong>(fixture.width) * 6U + 1U) * fixture.height;
      std::vector<std::uint8_t> raw(count), compressed(compressBound(count));
      if (uncompress(raw.data(), &count, idat.data(), idat.size()) != Z_OK) return 2;
      if (!exact_decode(fixture, raw)) { std::cerr << "pixel decode mismatch\n"; return 4; }
      if (production) {
        if (!metadata_matches(png, metadata, software)) return 4;
        std::cout << fixture.name << ',' << bits << ',' << encode_ms << ',' << png.size() << ",production-exact\n" << std::flush;
        continue;
      }
#ifdef HDRSHOT_PNG_PARAMETER_SWEEP
      if (!metadata_matches(png,metadata,software)) { std::cerr << "metadata/CRC mismatch\n"; return 4; }
      std::cout << fixture.name << ',' << bits << ',' << (sdr ? "SDR" : "HDR") << ',' << experiment_level << ','
                << experiment_memory << ',' << experiment_strategy << ',' << encode_ms << ',' << png.size()
                << ",yes\n" << std::flush;
      continue;
#endif
#ifdef __APPLE__
      if (native) {
        auto [data, ms] = timed([&] { return native_deflate(raw, std::size_t(fixture.width) * 6 + 1); });
        if (!verify_native(data, raw)) { std::cerr << "native decode mismatch\n"; return 4; }
        std::cout << fixture.name << ',' << bits << ',' << encode_ms << ',' << png.size()
                  << ",apple-compression," << ms << ',' << data.size() << '\n' << std::flush;
        continue;
      }
#else
      if (native) { std::cerr << "Apple Compression probe requires macOS\n"; return 2; }
#endif
      for (int level : {1, 3, 6}) {
        uLongf size = compressed.size();
        auto [code, ms] = timed([&] { return compress2(compressed.data(), &size, raw.data(), count, level); });
        if (code != Z_OK) return 2;
        std::cout << fixture.name << ',' << bits << ',' << encode_ms << ',' << png.size()
                  << ',' << level << ',' << ms << ',' << size << '\n' << std::flush;
      }
      if (fixture.name == "seeded_photo_like") for (int chain : {8, 16, 32}) {
        z_stream stream{};
        if (deflateInit(&stream, 6) != Z_OK || deflateTune(&stream, 8, 16, 128, chain) != Z_OK) return 2;
        stream.next_in = raw.data(); stream.avail_in = static_cast<uInt>(count);
        stream.next_out = compressed.data(); stream.avail_out = static_cast<uInt>(compressed.size());
        auto [code, ms] = timed([&] { return deflate(&stream, Z_FINISH); });
        const auto size = stream.total_out;
        deflateEnd(&stream);
        if (code != Z_STREAM_END) return 2;
        std::cout << fixture.name << ',' << bits << ',' << encode_ms << ',' << png.size()
                  << ",6-chain-" << chain << ',' << ms << ',' << size << '\n' << std::flush;
      }
    }
    return 0;
  }
  bool acceptable = true;
  std::cout << "case,reference_ms,balanced_ms,reference_bytes,balanced_bytes,size_ratio\n";
  for (const auto& fixture : make_corpus()) {
    auto [reference, reference_ms] = timed([&] {
      return PngEncoder::encode_16bit(EncodePng16Request{
          fixture.width, fixture.height, PngColorType::rgb, fixture.rgb,
          PngColorMetadata{kBt2100PqFullRange, std::nullopt}});
    });
    auto [balanced, balanced_ms] = timed([&] {
      return StreamingPngEncoder::encode_16bit_rgb(EncodeStreamingPng16Request{
          fixture.width, fixture.height,
          PngColorMetadata{kBt2100PqFullRange, std::nullopt},
          [&](const std::uint32_t y) -> Result<std::vector<std::uint16_t>, Error> {
            const auto row_samples = static_cast<std::size_t>(fixture.width) * 3U;
            const auto begin = fixture.rgb.begin() +
                static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * row_samples);
            return Result<std::vector<std::uint16_t>, Error>::success(
                {begin, begin + static_cast<std::ptrdiff_t>(row_samples)});
          }});
    });
    if (!reference || !balanced) {
      std::cerr << "encoding failed for " << fixture.name << '\n';
      return 2;
    }
    const auto ratio = static_cast<double>(balanced.value().bytes.size()) /
        static_cast<double>(reference.value().bytes.size());
    // The synthetic smooth-gradient case exposes the intentional level-6 vs
    // level-9 trade-off. Keep a deterministic regression ceiling while timing
    // remains reported evidence rather than a flaky pass/fail gate.
    acceptable = acceptable &&
        balanced.value().bytes.size() <= reference.value().bytes.size() * 160U / 100U + 1024U;
    std::cout << fixture.name << ',' << std::fixed << std::setprecision(3)
              << reference_ms << ',' << balanced_ms << ','
              << reference.value().bytes.size() << ','
              << balanced.value().bytes.size() << ',' << ratio << '\n';
  }
  return acceptable ? 0 : 3;
}
