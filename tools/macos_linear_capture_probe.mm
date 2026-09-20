// Opt-in desktop probe: production MacCapturePort, numeric fixture only.
// Historical target name retained. ADR-018 source is encoded Extended P3;
// compare after one software inverse transfer, not as if raw samples were linear.
// Does not modify app settings/clipboard or export a desktop image. 30s hard exit.
#include "platform/macos/macos_capture_ports.hpp"
#include "platform/qt/qt_session_diagnostics_port.hpp"
#include "core/build_metadata.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#import <AppKit/AppKit.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreImage/CoreImage.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>

using Clock = std::chrono::steady_clock;
static NSArray* expected() {
  return @[@[@.25,@.25,@.25], @[@1,@1,@1], @[@2,@2,@2], @[@4,@4,@4], @[@8,@8,@8],
           @[@4,@0,@0], @[@0,@4,@0], @[@0,@0,@4], @[@4,@4,@0], @[@0,@4,@4]];
}
static void pump(double seconds) {
  NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
  while ([until timeIntervalSinceNow] > 0)
    [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                           beforeDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
}
struct Reply { std::mutex mutex; NSDictionary* value = nil; bool done = false; };
static NSDictionary* awaitReply(std::function<void(std::shared_ptr<Reply>)> start) {
  auto state = std::make_shared<Reply>();
  const auto started = Clock::now();
  start(state);
  while (std::chrono::duration<double>(Clock::now()-started).count() < 8) {
    { std::scoped_lock lock(state->mutex); if(state->done) return state->value; }
    pump(.01);
  }
  return @{ @"error": @"timeout_8s" };
}
static void finish(std::shared_ptr<Reply> state, NSDictionary* value) {
  std::scoped_lock lock(state->mutex); state->value = value; state->done = true;
}
static NSArray* pixels(const void* bytes, size_t length, size_t width, size_t height,
    size_t rowBytes, size_t componentBits, size_t pixelBits, CGRect roi, bool bigEndian=false) {
  if(!bytes || (componentBits!=16 && componentBits!=32) || pixelBits!=4*componentBits) return @[];
  NSMutableArray* result = [NSMutableArray array];
  for(int i=0;i<10;i++) {
    const size_t x = (size_t)(roi.origin.x + roi.size.width * ((i%5)+.5)/5);
    const size_t y = (size_t)(roi.origin.y + roi.size.height * ((i/5)+.5)/2);
    const size_t offset=y*rowBytes+x*(pixelBits/8);
    if(x>=width || y>=height || offset+pixelBits/8>length) return @[];
    NSMutableArray* v = [NSMutableArray array];
    for(size_t j=0;j<4;j++) {
      const auto* p=(const unsigned char*)bytes+offset+j*(componentBits/8);
      double value;
      if(componentBits==16) { uint16_t bits; memcpy(&bits,p,2); if(bigEndian) bits=__builtin_bswap16(bits);
        _Float16 half; memcpy(&half,&bits,2); value=(double)half;
      } else { uint32_t bits; memcpy(&bits,p,4); if(bigEndian) bits=__builtin_bswap32(bits);
        float f; memcpy(&f,&bits,4); value=f; }
      [v addObject:std::isfinite(value) ? @(value) : @"nonfinite"];
    }
    [result addObject:v];
  }
  return result;
}
int main(int argc, const char** argv) { @autoreleasepool {
  if(argc!=2) return 2;
  if([[NSFileManager defaultManager] fileExistsAtPath:@(argv[1])]) return 2;
  [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
  if(!CGPreflightScreenCaptureAccess()) { fprintf(stderr,"screen_recording_not_authorized\n"); return 3; }
  // A hard lifetime limit also closes the diagnostic window if an API wedges.
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW,30*NSEC_PER_SEC),dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{_Exit(4);});
  NSDictionary* contentStatus=awaitReply([&](auto state) {
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:NO
      completionHandler:^(SCShareableContent* c,NSError* e) {
        finish(state,e ? @{ @"error":e.description } : (c ? @{@"content":c} : @{}));
      }];
  });
  SCShareableContent* content=contentStatus[@"content"];
  if(contentStatus[@"error"] || !content || content.displays.count==0) {
    fprintf(stderr,"no_capture_content: %s\n",contentStatus.description.UTF8String); return 3;
  }
  const CGDirectDisplayID displayID=CGMainDisplayID();
  SCDisplay* display=nil; NSScreen* screen=nil;
  for(SCDisplay* d in content.displays) if(d.displayID==displayID) display=d;
  for(NSScreen* s in NSScreen.screens) if([s.deviceDescription[@"NSScreenNumber"] unsignedIntValue]==displayID) screen=s;
  if(!display||!screen) return 3;
  id<MTLDevice> device=MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> queue=[device newCommandQueue];
  NSError* shaderError=nil;
  NSString* shader=@"#include <metal_stdlib>\nusing namespace metal;\nstruct V{float4 p [[position]];};\n"
    @"vertex V vs(uint id [[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return{float4(p[id],0,1)};}\n"
    @"fragment half4 fs(V in [[stage_in]],constant float2 &size [[buffer(0)]]) {\n"
    @"uint x=min(uint(in.p.x/size.x*5),4u),y=min(uint(in.p.y/size.y*2),1u);\n"
    @"float3 c[10]={float3(.25),float3(1),float3(2),float3(4),float3(8),float3(4,0,0),float3(0,4,0),float3(0,0,4),float3(4,4,0),float3(0,4,4)};return half4(half3(c[y*5+x]),1);}";
  id<MTLLibrary> lib=[device newLibraryWithSource:shader options:nil error:&shaderError];
  MTLRenderPipelineDescriptor* pd=[MTLRenderPipelineDescriptor new];
  pd.vertexFunction=[lib newFunctionWithName:@"vs"]; pd.fragmentFunction=[lib newFunctionWithName:@"fs"];
  pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA16Float;
  id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&shaderError];
  if(!pipeline) { fprintf(stderr,"shader: %s\n",shaderError.description.UTF8String); return 3; }
  NSRect rect=NSMakeRect(NSMidX(screen.frame)-320,NSMidY(screen.frame)-180,640,360);
  NSWindow* window=[[NSWindow alloc] initWithContentRect:rect styleMask:NSWindowStyleMaskBorderless
                                backing:NSBackingStoreBuffered defer:NO screen:screen];
  window.releasedWhenClosed=NO; window.opaque=YES; window.backgroundColor=NSColor.blackColor;
  window.level=NSFloatingWindowLevel; window.ignoresMouseEvents=YES;
  CAMetalLayer* layer=[CAMetalLayer layer]; layer.device=device; layer.pixelFormat=MTLPixelFormatRGBA16Float;
  layer.wantsExtendedDynamicRangeContent=YES; layer.framebufferOnly=NO;
  CGColorSpaceRef p3=CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearDisplayP3);layer.colorspace=p3;CGColorSpaceRelease(p3);
  window.contentView.wantsLayer=YES;window.contentView.layer=layer;layer.frame=window.contentView.bounds;
  layer.contentsScale=screen.backingScaleFactor;
  layer.drawableSize=CGSizeMake(640*screen.backingScaleFactor,360*screen.backingScaleFactor);
  [window orderFrontRegardless];
  NSArray* fixtureReadback=@[];
  auto render=[&] {
    id<CAMetalDrawable> drawable=[layer nextDrawable]; if(!drawable) return;
    id<MTLCommandBuffer> cb=[queue commandBuffer];
    MTLRenderPassDescriptor* pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=drawable.texture;pass.colorAttachments[0].loadAction=MTLLoadActionClear;
    pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc=[cb renderCommandEncoderWithDescriptor:pass]; [enc setRenderPipelineState:pipeline];
    float size[2]={(float)drawable.texture.width,(float)drawable.texture.height}; [enc setFragmentBytes:size length:8 atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[enc endEncoding];
    const size_t row=drawable.texture.width*8, length=row*drawable.texture.height;
    id<MTLBuffer> read=[device newBufferWithLength:length options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit=[cb blitCommandEncoder];
    [blit copyFromTexture:drawable.texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(drawable.texture.width,drawable.texture.height,1) toBuffer:read
        destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:length];[blit endEncoding];
    [cb presentDrawable:drawable];[cb commit];[cb waitUntilCompleted];
    fixtureReadback=pixels(read.contents,length,drawable.texture.width,drawable.texture.height,row,16,64,
                          CGRectMake(0,0,drawable.texture.width,drawable.texture.height));
  };
  render(); pump(3); render(); pump(1);
  CGRect logicalROI=CGRectMake(rect.origin.x-screen.frame.origin.x,
      NSMaxY(screen.frame)-NSMaxY(rect),rect.size.width,rect.size.height);

  auto log = std::make_shared<hdrshot::QtSessionDiagnosticsPort>(
      std::string(argv[1]) + ".logs");
  (void)log->start(std::string(hdrshot::build_timestamp()), "macOS");
  log->set_detailed_logging(true);
  hdrshot::MacCapturePort capture(log);
  NSMutableArray* results = [NSMutableArray array];
  bool passed = true;
  for(int repeat=0;repeat<3;++repeat) {
    render(); pump(.1);
    NSDictionary* result = awaitReply([&](auto state) {
      capture.capture({hdrshot::SessionId{1}, hdrshot::OperationId{static_cast<uint64_t>(repeat+1)},
          1, {hdrshot::DisplayId{displayID}}, hdrshot::PixelFormat::rgba16_float, true},
          [state, logicalROI, display](hdrshot::Result<hdrshot::NativeFrameBatch,hdrshot::Error> batch) {
        if(!batch || batch.value().frames.size()!=1) {
          finish(state,@{@"error":batch ? @"frame_count" : @(hdrshot::to_string(batch.error().code).c_str())}); return;
        }
        const auto& frame=batch.value().frames[0];
        CGRect roi=CGRectMake(logicalROI.origin.x*frame.size_px.width/display.frame.size.width,
            logicalROI.origin.y*frame.size_px.height/display.frame.size.height,
            logicalROI.size.width*frame.size_px.width/display.frame.size.width,
            logicalROI.size.height*frame.size_px.height/display.frame.size.height);
        auto raw=pixels(frame.rgba_half.data(),frame.rgba_half.size()*2,frame.size_px.width,
            frame.size_px.height,frame.size_px.width*8,16,64,roi);
        finish(state,@{@"raw":raw,@"extendedSrgb":@(frame.encoding.transfer==hdrshot::TransferFunction::extended_srgb),
            @"p3":@(frame.encoding.primaries==hdrshot::ColorPrimaries::display_p3),
            @"width":@(frame.size_px.width),@"height":@(frame.size_px.height)});
      });
    });
    [results addObject:result];
    NSArray* raw=result[@"raw"];
    if(result[@"error"] || ![result[@"extendedSrgb"] boolValue] || ![result[@"p3"] boolValue] ||
        raw.count!=10) { passed=false; continue; }
    for(int i=0;i<10;++i) for(int c=0;c<3;++c)
      if(std::abs(hdrshot::ExtendedP3Mapper::inverse_extended_srgb([raw[i][c] floatValue])-
          [expected()[i][c] doubleValue])>.01) passed=false;
  }
  [window orderOut:nil]; [window close];
  NSDictionary* report=@{@"passed":@(passed),@"os":NSProcessInfo.processInfo.operatingSystemVersionString,
      @"displayID":@(displayID),@"screenCount":@(NSScreen.screens.count),
      @"currentEDR":@(screen.maximumExtendedDynamicRangeColorComponentValue),
      @"potentialEDR":@(screen.maximumPotentialExtendedDynamicRangeColorComponentValue),
      @"expectedLinearP3":expected(),@"fixtureGPUReadback":fixtureReadback,@"results":results};
  NSError* error=nil;
  NSData* json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted|NSJSONWritingSortedKeys error:&error];
  if(!json || ![json writeToFile:@(argv[1]) options:NSDataWritingWithoutOverwriting error:&error]) return 5;
  printf("production_capture_fixture=%s\n", passed?"passed":"failed");
  return passed ? 0 : 1;
}}
