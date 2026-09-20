// Offline experiment only. No production encoder selection or screen capture.
#include "domain/color/display_p3_icc_profile.hpp"
#include "domain/png/streaming_png_encoder.hpp"
#include "png_benchmark_corpus.hpp"

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <array>
#include <chrono>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <zlib.h>

namespace {
using namespace hdrshot;
using benchmark::CorpusCase;
using Bytes = std::vector<std::uint8_t>;
constexpr std::string_view kSoftware = "SeriousShot; build PNG encoder experiment";

template<class T> struct OwnedCF {
  T value{};
  explicit OwnedCF(T v) : value(v) { if (!v) throw std::runtime_error("null CF object"); }
  ~OwnedCF() { CFRelease(value); }
  OwnedCF(const OwnedCF&) = delete;
  OwnedCF& operator=(const OwnedCF&) = delete;
};
void require(bool valid, const char* message) {
  if (!valid) throw std::runtime_error(message);
}
std::uint32_t be32(std::span<const std::uint8_t> b) {
  require(b.size() >= 4, "truncated integer");
  return (std::uint32_t(b[0]) << 24U) | (std::uint32_t(b[1]) << 16U) |
         (std::uint32_t(b[2]) << 8U) | b[3];
}
struct Chunk {
  std::string type;
  std::span<const std::uint8_t> payload;
  std::span<const std::uint8_t> entire;
};
std::vector<Chunk> chunks(const Bytes& png) {
  constexpr std::array<std::uint8_t,8> signature{137,80,78,71,13,10,26,10};
  require(png.size() >= 8 && std::equal(signature.begin(),signature.end(),png.begin()), "signature");
  std::vector<Chunk> out;
  const std::span<const std::uint8_t> bytes(png);
  std::size_t p = 8;
  while (p + 12 <= bytes.size()) {
    const auto n = std::size_t(be32(bytes.subspan(p,4)));
    require(n <= bytes.size() - p - 12, "chunk length");
    const auto crc = crc32(0, bytes.data()+p+4, static_cast<uInt>(n+4));
    require(crc == be32(bytes.subspan(p+8+n,4)), "CRC");
    out.push_back({std::string(reinterpret_cast<const char*>(bytes.data()+p+4),4),
                   bytes.subspan(p+8,n), bytes.subspan(p,n+12)});
    p += n + 12;
  }
  require(p == bytes.size() && !out.empty() && out.front().type == "IHDR" &&
          out.back().type == "IEND", "PNG structure");
  return out;
}
Bytes shared(const CorpusCase& f, bool hdr, int bits) {
  const auto metadata = hdr
      ? PngColorMetadata{kDisplayP3PqFullRange,std::nullopt,ContentLightLevelInfo{10000000,4000000}}
      : PngColorMetadata{kDisplayP3SrgbFullRange,IccProfilePayload{"Display P3",DisplayP3IccProfile::bytes()}};
  // cLLI numbers are serialization fixtures, not a claim about these synthetic pixels.
  auto result = StreamingPngEncoder::encode_16bit_rgb({f.width,f.height,metadata,
      [&](std::uint32_t y) -> Result<std::vector<std::uint16_t>,Error> {
        const auto first = f.rgb.begin() + static_cast<std::ptrdiff_t>(std::size_t(y)*f.width*3);
        return Result<std::vector<std::uint16_t>,Error>::success({first,first+f.width*3});
      }, kSoftware, static_cast<std::uint8_t>(bits)});
  require(bool(result), "shared encode failed");
  return std::move(result.value().bytes);
}
Bytes native(const CorpusCase& f, bool hdr, int filter, double quality) {
  @autoreleasepool {
    OwnedCF data(CFDataCreateMutable(kCFAllocatorDefault,0));
    OwnedCF destination(CGImageDestinationCreateWithData(data.value,CFSTR("public.png"),1,nullptr));
    const auto icc = DisplayP3IccProfile::bytes();
    OwnedCF icc_data(CFDataCreateWithBytesNoCopy(kCFAllocatorDefault,icc.data(),
        static_cast<CFIndex>(icc.size()),kCFAllocatorNull));
    OwnedCF space(hdr ? CGColorSpaceCreateWithName(kCGColorSpaceDisplayP3_PQ)
                      : CGColorSpaceCreateWithICCData(icc_data.value));
    OwnedCF provider(CGDataProviderCreateWithData(nullptr,f.rgb.data(),f.rgb.size()*2,nullptr));
    OwnedCF image(CGImageCreate(f.width,f.height,16,48,std::size_t(f.width)*6,space.value,
        static_cast<CGBitmapInfo>(kCGImageAlphaNone) | static_cast<CGBitmapInfo>(kCGImageByteOrder16Host),
        provider.value,nullptr,false,kCGRenderingIntentDefault));
    NSMutableDictionary* png_properties = [@{
      (__bridge NSString*)kCGImagePropertyPNGSoftware: @(kSoftware.data())
    } mutableCopy];
    if (filter >= 0) png_properties[(__bridge NSString*)kCGImagePropertyPNGCompressionFilter] = @(filter);
    NSMutableDictionary* properties = [@{(__bridge NSString*)kCGImagePropertyPNGDictionary: png_properties} mutableCopy];
    // Diagnostic only: do not assume this generic key controls PNG effort.
    if (quality >= 0) properties[(__bridge NSString*)kCGImageDestinationLossyCompressionQuality] = @(quality);
    CGImageDestinationAddImage(destination.value,image.value,(__bridge CFDictionaryRef)properties);
    require(CGImageDestinationFinalize(destination.value), "Image I/O finalize failed");
    const auto* first = CFDataGetBytePtr(data.value);
    return {first,first + CFDataGetLength(data.value)};
  }
}
// Experimental, lossless metadata envelope: replace only ancillary chunks.
// IDAT is never decoded/re-encoded here. Production has no such backend yet.
Bytes canonicalize(const Bytes& imageio, const Bytes& reference) {
  const auto source = chunks(imageio);
  const auto expected = chunks(reference);
  require(source.front().payload.size() == expected.front().payload.size() &&
      std::equal(source.front().payload.begin(),source.front().payload.end(),expected.front().payload.begin()),
      "native IHDR differs (depth, alpha or interlace)");
  Bytes result(imageio.begin(),imageio.begin()+8);
  auto append = [&](const Chunk& c) { result.insert(result.end(),c.entire.begin(),c.entire.end()); };
  append(source.front());
  for (const auto& c : expected) if (c.type != "IHDR" && c.type != "IDAT" && c.type != "IEND") append(c);
  bool idat = false;
  for (const auto& c : source) {
    require((c.type[0] & 32) != 0 || c.type == "IHDR" || c.type == "IDAT" || c.type == "IEND", "unknown critical chunk");
    if (c.type == "IDAT") { append(c); idat = true; }
  }
  require(idat, "missing IDAT");
  append(source.back());
  return result;
}
void verify(const Bytes& png, const CorpusCase& f, bool hdr) {
  Bytes idat;
  bool cicp=false,icc=false,clli=false,software=false;
  for (const auto& c : chunks(png)) {
    if (c.type == "IHDR") {
      require(c.payload.size()==13 && be32(c.payload)==f.width && be32(c.payload.subspan(4))==f.height &&
          c.payload[8]==16 && c.payload[9]==2 && c.payload[10]==0 && c.payload[11]==0 && c.payload[12]==0,
          "RGB16 noninterlaced IHDR");
    } else if (c.type == "IDAT") idat.insert(idat.end(),c.payload.begin(),c.payload.end());
    else if (c.type == "cICP") {
      require(!cicp && c.payload.size()==4 && c.payload[0]==12 && c.payload[1]==(hdr?16:13) &&
              c.payload[2]==0 && c.payload[3]==1, "cICP mismatch"); cicp=true;
    } else if (c.type == "iCCP") {
      const auto end = std::find(c.payload.begin(),c.payload.end(),0);
      require(!icc && end!=c.payload.end() && end+1!=c.payload.end() && *(end+1)==0, "iCCP format");
      const auto offset=static_cast<std::size_t>(end-c.payload.begin())+2;
      const auto expected=DisplayP3IccProfile::bytes();
      Bytes decoded(expected.size()); uLongf n=decoded.size();
      require(uncompress(decoded.data(),&n,c.payload.data()+offset,c.payload.size()-offset)==Z_OK &&
          n==expected.size() && std::equal(decoded.begin(),decoded.end(),expected.begin()), "ICC bytes mismatch");
      icc=true;
    } else if (c.type == "cLLI") {
      require(!clli && c.payload.size()==8 && be32(c.payload)==10000000 && be32(c.payload.subspan(4))==4000000,
              "cLLI mismatch"); clli=true;
    } else if (c.type == "tEXt") {
      const std::string expected=std::string("Software\0",9)+std::string(kSoftware);
      if (c.payload.size()==expected.size() && std::equal(c.payload.begin(),c.payload.end(),expected.begin())) software=true;
    } else require(c.type=="IEND", "unexpected ancillary chunk");
  }
  require(cicp && icc==!hdr && clli==hdr && software, "missing metadata");
  const auto stride=std::size_t(f.width)*6;
  Bytes raw((stride+1)*f.height); uLongf n=raw.size();
  require(uncompress(raw.data(),&n,idat.data(),idat.size())==Z_OK && n==raw.size(), "inflate");
  Bytes previous(stride),current(stride);
  for (std::uint32_t y=0; y<f.height; ++y) {
    const auto row=std::size_t(y)*(stride+1); const auto filter=raw[row];
    require(filter<=4,"filter");
    for (std::size_t x=0; x<stride; ++x) {
      const int a=x>=6?current[x-6]:0,b=previous[x],c=x>=6?previous[x-6]:0;
      int prediction=0;
      if (filter==1) prediction=a;
      if (filter==2) prediction=b;
      if (filter==3) prediction=(a+b)/2;
      if (filter==4) {
        const int p=a+b-c,pa=std::abs(p-a),pb=std::abs(p-b),pc=std::abs(p-c);
        prediction=pa<=pb && pa<=pc?a:pb<=pc?b:c;
      }
      current[x]=static_cast<std::uint8_t>(raw[row+1+x]+prediction);
      const auto value=f.rgb[std::size_t(y)*f.width*3+x/2];
      require(current[x]==(x%2?value&255U:value>>8U), "decoded RGB16 changed");
    }
    previous.swap(current);
  }
}
template<class F> auto timed(F&& f) {
  const auto start=std::chrono::steady_clock::now(); auto value=f();
  const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  return std::pair{std::move(value),ms};
}
} // namespace

int main(int argc,char** argv) {
  try {
    bool large=false,photo=false; int repeats=1,only_bits=0; double quality=-1;
    std::string range="both",filter_name="default";
    for(int i=1;i<argc;++i) {
      const std::string arg=argv[i];
      if(arg=="--5k") large=true;
      else if(arg=="--photo") photo=true;
      else if(arg.starts_with("--repeats=")) repeats=std::stoi(arg.substr(10));
      else if(arg.starts_with("--bits=")) only_bits=std::stoi(arg.substr(7));
      else if(arg.starts_with("--range=")) range=arg.substr(8);
      else if(arg.starts_with("--filter=")) filter_name=arg.substr(9);
      else if(arg.starts_with("--quality=")) quality=std::stod(arg.substr(10));
      else throw std::runtime_error("unknown argument");
    }
    require(repeats>=1 && repeats<=10 && (only_bits==0||only_bits==10||only_bits==12||only_bits==16),"arguments");
    require(range=="both"||range=="hdr"||range=="sdr","range");
    require(std::isfinite(quality) && quality>=-1 && quality<=1,"quality");
    int filter=-1;
    if(filter_name=="sub") filter=IMAGEIO_PNG_FILTER_SUB;
    else if(filter_name=="up") filter=IMAGEIO_PNG_FILTER_UP;
    else if(filter_name=="avg") filter=IMAGEIO_PNG_FILTER_AVG;
    else if(filter_name=="subup") filter=IMAGEIO_PNG_FILTER_SUB|IMAGEIO_PNG_FILTER_UP;
    else if(filter_name=="paeth") filter=IMAGEIO_PNG_FILTER_PAETH;
    else if(filter_name=="all") filter=IMAGEIO_PNG_ALL_FILTERS;
    else if(filter_name=="none") filter=IMAGEIO_PNG_FILTER_NONE;
    else if(filter_name.starts_with("mask_")) {
      filter=std::stoi(filter_name.substr(5));
      require(filter>=8 && filter<=248 && (filter&7)==0,"filter mask");
    } else require(filter_name=="default" || filter_name=="sweep","filter option");
    std::vector<std::pair<std::string,int>> filters;
    if(filter_name=="sweep") {
      for(int mask=1;mask<32;++mask) filters.emplace_back("mask_"+std::to_string(mask*8),mask*8);
    } else filters.emplace_back(filter_name,filter);
    OwnedCF types(CGImageDestinationCopyTypeIdentifiers());
    require(CFArrayContainsValue(types.value,CFRangeMake(0,CFArrayGetCount(types.value)),CFSTR("public.png")),"PNG unavailable");
    auto corpus=benchmark::make_corpus(large?5120:257,large?2880:129);
    if (!large) {
      CorpusCase exact{"full_range_odd",257,87,{}};
      exact.rgb.resize(std::size_t(exact.width)*exact.height*3);
      for (std::size_t i=0;i<exact.rgb.size();++i) exact.rgb[i]=static_cast<std::uint16_t>(i);
      corpus.push_back(std::move(exact));
      corpus.push_back({"one_pixel",1,1,{0,32768,65535}});
    }
    std::cout<<"case,width,height,bits,range,filter,run,shared_ms,shared_bytes,imageio_ms,wrap_ms,raw_bytes,canonical_bytes,exact,raw_chunks,quality,idat_crc\n";
    for(const auto& original:corpus) {
      if(photo && original.name!="seeded_photo_like") continue;
      for(const int bits:{10,12,16}) {
        if(only_bits && only_bits!=bits) continue;
        auto f=original; const auto levels=(1U<<bits)-1U;
        for(auto& v:f.rgb) {
          const auto q=(std::uint32_t(v)*levels+32767U)/65535U;
          v=static_cast<std::uint16_t>((q*65535U+levels/2)/levels);
        }
        for(const bool hdr:{false,true}) {
          if((range=="hdr"&&!hdr)||(range=="sdr"&&hdr)) continue;
          for(int run=0;run<repeats;++run) {
            auto [reference,shared_ms]=timed([&]{return shared(f,hdr,bits);});
            verify(reference,f,hdr);
            for(const auto& [name,mask]:filters) {
            auto [encoded,imageio_ms]=timed([&]{return native(f,hdr,mask,quality);});
            auto [canonical,wrap_ms]=timed([&]{return canonicalize(encoded,reference);});
            verify(canonical,f,hdr);
            std::string raw_chunks;
            uLong idat_crc=crc32(0,nullptr,0);
            for(const auto& c:chunks(encoded)) {
              if(c.type!="IDAT") raw_chunks+=c.type+"/";
              else idat_crc=crc32(idat_crc,c.payload.data(),static_cast<uInt>(c.payload.size()));
            }
            std::cout<<f.name<<','<<f.width<<','<<f.height<<','<<bits<<','<<(hdr?"HDR":"SDR")<<','<<name<<','<<run
              <<','<<shared_ms<<','<<reference.size()<<','<<imageio_ms<<','<<wrap_ms<<','<<encoded.size()<<','<<canonical.size()
              <<",yes,"<<raw_chunks<<','<<quality<<','<<idat_crc<<'\n'<<std::flush;
            }
          }
        }
      }
    }
  } catch(const std::exception& e) { std::cerr<<"Image I/O experiment: "<<e.what()<<'\n'; return 1; }
}
