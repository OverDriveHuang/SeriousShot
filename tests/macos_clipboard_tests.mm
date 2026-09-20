#include "platform/macos/macos_export_ports.hpp"
#include "application/export_workflow.hpp"
#include "adapters/shared/libultrahdr_encoder.hpp"
#include "domain/output/ultra_hdr_input_renderer.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "domain/png/streaming_png_encoder.hpp"
#include "ultra_hdr_metadata_test_support.hpp"
#include "test_support.hpp"

#import <AppKit/AppKit.h>
#import <CommonCrypto/CommonDigest.h>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <chrono>

namespace {
using namespace hdrshot;

PreparedExport fixture(SaveFormat format) {
  auto frame = std::make_shared<FrozenDesktop>(FrozenDesktop{
      FrameId{1}, 1, LogicalRect{0,0,32,32}, {CanonicalFrameSegment{
        DisplayId{1}, LogicalRect{0,0,32,32}, 1, PixelSize{32,32}, PixelFormat::rgba16_float,
        {ColorPrimaries::display_p3, TransferFunction::extended_srgb, AlphaMode::straight, 0},
        DisplayDynamicRange::hdr, {}}}});
  for (int y=0; y<32; ++y) for (int x=0; x<32; ++x) {
    const float v=0.2F+1.6F*static_cast<float>(x)/31.0F;
    for (float c : {v, v*0.8F, v*0.5F, 1.0F})
      frame->canonical_segments[0].rgba_half.push_back(ExtendedP3Mapper::encode_binary16(c));
  }
  ExportSnapshot snapshot{SessionId{1}, OperationId{1}, frame,
      SelectionSnapshot{1, PixelRect{0,0,32,32}}, AnnotationDocument::empty().snapshot(),
      nullptr, {}, DisplayId{1}, format};
  CpuUltraHdrInputRenderer renderer;
  LibUltraHdrEncoder encoder;
  auto prepared = ExportWorkflow::prepare(snapshot, nullptr, nullptr, &renderer, &encoder);
  HDRSHOT_CHECK(prepared.has_value());
  return std::move(prepared.value());
}

std::string sha256(NSData* data) {
  unsigned char hash[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(data.bytes, static_cast<CC_LONG>(data.length), hash);
  std::ostringstream out;
  for (auto b : hash) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(b);
  return out.str();
}

struct Board {
  NSPasteboard* pb = [NSPasteboard pasteboardWithUniqueName];
  std::string root = std::string(NSTemporaryDirectory().UTF8String) +
      "seriousshot-clipboard-" + NSUUID.UUID.UUIDString.UTF8String;
  ~Board() { [pb releaseGlobally]; std::filesystem::remove_all(root); }
  MacClipboardPort port() {
    HDRSHOT_CHECK(pb != nil && pb.name != nil);
    return MacClipboardPort(root, pb.name.UTF8String);
  }
};

void first_copy_of_each_format_replaces_types_and_preserves_metadata() {
  Board board;
  auto port=board.port();
  for (auto format : {SaveFormat::png_display_p3_dual_range, SaveFormat::ultra_hdr_jpeg,
                      SaveFormat::png_display_p3_dual_range}) {
    auto image=fixture(format);
    auto result=ExportWorkflow::copy_prepared(image, port);
    HDRSHOT_CHECK(result.has_value());
    const bool png=format==SaveFormat::png_display_p3_dual_range;
    HDRSHOT_CHECK(board.pb.pasteboardItems.count==1);
    auto* item=board.pb.pasteboardItems.firstObject;
    NSData* actual=[item dataForType:png ? NSPasteboardTypePNG : @"public.jpeg"];
    HDRSHOT_CHECK(actual.length==image.artifact.bytes.size());
    HDRSHOT_CHECK(std::memcmp(actual.bytes,image.artifact.bytes.data(),actual.length)==0);
    HDRSHOT_CHECK([item dataForType:png ? @"public.jpeg" : NSPasteboardTypePNG]==nil);
    HDRSHOT_CHECK([item dataForType:NSPasteboardTypeTIFF]==nil);
    auto* url=[NSURL URLWithString:[item stringForType:NSPasteboardTypeFileURL]];
    HDRSHOT_CHECK(url.isFileURL);
    HDRSHOT_CHECK([url.pathExtension isEqualToString:png ? @"png" : @"jpg"]);
    // Metadata assertions consume bytes read from the pasteboard, not a file.
    const auto* bytes=static_cast<const std::uint8_t*>(actual.bytes);
    if (png) {
      HDRSHOT_CHECK(bytes[24]==16 && bytes[25]==2);
      const std::vector<std::uint8_t> cicp{'c','I','C','P',12,16,0,1};
      HDRSHOT_CHECK(std::search(bytes,bytes+actual.length,cicp.begin(),cicp.end())!=bytes+actual.length);
    } else {
      std::unique_ptr<uhdr_codec_private_t,decltype(&uhdr_release_decoder)> decoder{
          uhdr_create_decoder(), &uhdr_release_decoder};
      uhdr_compressed_image_t input{};
      input.data=const_cast<void*>(actual.bytes);input.data_sz=input.capacity=actual.length;
      HDRSHOT_CHECK(uhdr_dec_set_image(decoder.get(),&input).error_code==UHDR_CODEC_OK);
      HDRSHOT_CHECK(uhdr_dec_probe(decoder.get()).error_code==UHDR_CODEC_OK);
      test::check_dual_metadata(decoder.get());
      test::check_p3_base_and_alternate(decoder.get());
      HDRSHOT_CHECK(uhdr_dec_get_gainmap_metadata(decoder.get())!=nullptr);
    }
  }
}

void failure_preserves_previous_clipboard_and_prior_files_survive() {
  Board board;
  const auto image=fixture(SaveFormat::png_display_p3_dual_range);
  NSString* originalURL;
  {
    auto port=board.port();
    HDRSHOT_CHECK(ExportWorkflow::copy_prepared(image,port).has_value());
    originalURL=[board.pb stringForType:NSPasteboardTypeFileURL];
    HDRSHOT_CHECK(ExportWorkflow::copy_prepared(image,port).has_value());
    HDRSHOT_CHECK(![[board.pb stringForType:NSPasteboardTypeFileURL] isEqualToString:originalURL]);
  }
  HDRSHOT_CHECK([NSFileManager.defaultManager fileExistsAtPath:[NSURL URLWithString:originalURL].path]);
  const auto change=board.pb.changeCount;
  MacClipboardPort broken([NSURL URLWithString:originalURL].path.UTF8String,board.pb.name.UTF8String);
  HDRSHOT_CHECK(!ExportWorkflow::copy_prepared(image,broken));
  HDRSHOT_CHECK(board.pb.changeCount==change);
  HDRSHOT_CHECK([board.pb dataForType:NSPasteboardTypePNG].length==image.artifact.bytes.size());
}
}

int main(int argc,char** argv) {
  @autoreleasepool {
    if (argc == 2 && std::string(argv[1]) == "--large-roundtrip") {
      // Named board only, even in this explicit performance mode. Independent
      // 10-bit RGB noise makes a genuinely large, valid PNG; no screen access.
      Board board;
      auto port = board.port();
      using Clock = std::chrono::steady_clock;
      const auto start = Clock::now();
      auto png = StreamingPngEncoder::encode_16bit_rgb({5120, 2880,
          PngColorMetadata{kDisplayP3PqFullRange, std::nullopt},
          [](std::uint32_t y) {
            std::vector<std::uint16_t> row(5120 * 3);
            std::uint32_t seed = y + 1;
            for (auto& value : row) {
              seed = seed * 1664525U + 1013904223U;
              const auto q = seed >> 22U;
              value = static_cast<std::uint16_t>((q * 65535U + 511U) / 1023U);
            }
            return Result<std::vector<std::uint16_t>, Error>::success(std::move(row));
          }});
      HDRSHOT_CHECK(png.has_value());
      const auto encoded = Clock::now();
      HDRSHOT_CHECK(port.write({png.value().bytes, {"image/png"}}).has_value());
      const auto written = Clock::now();
      // Release producer storage after constructing an independent expected
      // digest. Read the *pasteboard*, not the temporary file, after port write.
      NSData* expected = [NSData dataWithBytesNoCopy:png.value().bytes.data()
          length:png.value().bytes.size() freeWhenDone:NO];
      const auto digest = sha256(expected);
      const auto count = png.value().bytes.size();
      png.value().bytes.clear(); png.value().bytes.shrink_to_fit();
      NSData* actual = [board.pb dataForType:NSPasteboardTypePNG];
      HDRSHOT_CHECK(actual.length == count && sha256(actual) == digest);
      HDRSHOT_CHECK(static_cast<const std::uint8_t*>(actual.bytes)[24] == 16);
      const auto read = Clock::now();
      std::cout << "5k PNG bytes=" << count << " encode_ms="
          << std::chrono::duration<double, std::milli>(encoded-start).count()
          << " publish_ms=" << std::chrono::duration<double, std::milli>(written-encoded).count()
          << " verify_ms=" << std::chrono::duration<double, std::milli>(read-written).count()
          << " pasteboard_sha256_equal=true\n";
      return 0;
    }
    // Explicit-only desktop probe. Prints the in-memory encoded expectation;
    // the receiver must verify the actual pasted File rather than this cache.
    if (argc==3 && std::string(argv[1])=="--publish") {
      const std::string mode=argv[2];
      const bool jpeg=mode=="jpeg" || mode=="legacy-jpeg";
      if (mode!="png" && mode!="jpeg" && mode!="legacy-png" && mode!="legacy-jpeg") return 2;
      auto image=fixture(jpeg?SaveFormat::ultra_hdr_jpeg:SaveFormat::png_display_p3_dual_range);
      NSData* data=[NSData dataWithBytes:image.artifact.bytes.data() length:image.artifact.bytes.size()];
      if (mode.starts_with("legacy-")) {
        [NSPasteboard.generalPasteboard clearContents];
        HDRSHOT_CHECK([NSPasteboard.generalPasteboard setData:data forType:jpeg?@"public.jpeg":NSPasteboardTypePNG]);
      } else {
        MacClipboardPort port;
        HDRSHOT_CHECK(ExportWorkflow::copy_prepared(image,port).has_value());
      }
      std::cout << "{\"mime\":\"" << image.artifact.mime_type << "\",\"bytes\":" << data.length
                << ",\"sha256\":\"" << sha256(data) << "\"}\n";
      return 0;
    }
    return test::run({
      {"first PNG JPEG PNG copy preserves original metadata",first_copy_of_each_format_replaces_types_and_preserves_metadata},
      {"failure leaves clipboard intact and immutable files survive",failure_preserves_previous_clipboard_and_prior_files_survive}});
  }
}
