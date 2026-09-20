#include "platform/macos/macos_analysis_backend.hpp"
#include "domain/analysis/engine.hpp"
#include "domain/annotation/annotation_compositing.hpp"
#include "platform/macos/macos_gpu_source.hpp"
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <os/log.h>
#include <string>

namespace hdrshot {
namespace {
using namespace analysis;
constexpr const char *kIntrinsics = R"METAL(
#include <metal_stdlib>
using namespace metal;
inline float a_pow(float x,float y){return pow(x,y);}
inline float a_sqrt(float x){return sqrt(x);}
inline float a_atan2(float y,float x){return atan2(y,x);}
inline float a_floor(float x){return floor(x);}
inline float a_log2(float x){return log2(x);}
inline float a_copysign(float x,float y){return copysign(x,y);}
inline bool a_finite(float x){return (as_type<uint>(x)&0x7f800000u)!=0x7f800000u;}
)METAL";
constexpr const char *kKernels = R"METAL(
inline A3 a3v(float3 v){return a3(v.x,v.y,v.z);}
inline float3 v3(A3 v){return float3(v.x,v.y,v.z);}
struct WorkParams { uint width,height,space,radius;float white;uint vertical; };
kernel void copy_roi(texture2d<float,access::read> source[[texture(0)]],texture2d<float,access::write> output[[texture(1)]],constant uint2& origin[[buffer(0)]],uint2 p[[thread_position_in_grid]]){
 if(p.x<output.get_width()&&p.y<output.get_height())output.write(float4(source.read(p+origin).rgb,1),p);
}
kernel void upload_linear(device const float4* source[[buffer(0)]],texture2d<float,access::write> output[[texture(0)]],uint2 p[[thread_position_in_grid]]){
 if(p.x<output.get_width()&&p.y<output.get_height())output.write(float4(source[p.y*output.get_width()+p.x].rgb,1),p);
}
struct Patch {uint first,count,x,y,offset,flags;float r,g,b,a;};
kernel void apply_patches(texture2d<float,access::read_write> output[[texture(0)]],device const Patch* spans[[buffer(0)]],device const float4* samples[[buffer(1)]],constant uint2& limits[[buffer(2)]],uint i[[thread_position_in_grid]]){
 if(i>=limits.y)return;uint lo=0,hi=limits.x;while(lo+1<hi){uint mid=(lo+hi)/2;if(i<spans[mid].first)hi=mid;else lo=mid;}
 Patch span=spans[lo];uint offset=i-span.first;float4 v=span.flags?float4(span.r,span.g,span.b,span.a):samples[span.offset+offset];
 uint2 p=uint2(span.x+offset,span.y);float3 color=v.rgb;if(v.a>0)color+=v.a*max(output.read(p).rgb,0.0f);output.write(float4(color,1),p);
}
kernel void blur_horizontal(texture2d<float,access::read> source[[texture(0)]],texture2d<float,access::write> target[[texture(1)]],constant WorkParams& p[[buffer(0)]],constant float* weights[[buffer(1)]],uint2 q[[thread_position_in_grid]]){
 if(q.x>=p.width||q.y>=p.height)return;float4 sum=0;
 for(int d=-int(p.radius);d<=int(p.radius);++d){int x=int(q.x)+d;if(x<0||x>=int(p.width))continue;float3 v=source.read(uint2(x,q.y)).rgb;if(!finite3(a3v(v)))continue;float w=weights[d+int(p.radius)];sum+=float4(v,1)*w;}
 target.write(sum,q);
}
kernel void build_work(texture2d<float,access::read> source[[texture(0)]],texture2d<float,access::read> horizontal[[texture(1)]],texture2d<float,access::write> target[[texture(2)]],constant WorkParams& p[[buffer(0)]],constant float* weights[[buffer(1)]],uint2 q[[thread_position_in_grid]]){
 if(q.x>=p.width||q.y>=p.height)return;A3 color=a3v(source.read(q).rgb);if(!finite3(color)){target.write(float4(0),q);return;}
 if(p.radius){float4 sum=0;for(int d=-int(p.radius);d<=int(p.radius);++d){int y=int(q.y)+d;if(y<0||y>=int(p.height))continue;sum+=horizontal.read(uint2(q.x,y))*weights[d+int(p.radius)];}if(!(sum.a>0)){target.write(float4(0),q);return;}color=a3v(sum.rgb/sum.a);}
 A3 result=working_rgb(color,p.space,p.white);target.write(float4(v3(result),finite3(result)?1.0f:0.0f),q);
}
struct ViewParams {uint width,height,space,false_color;float scale,offset_x,offset_y,white;uint mask_kind;float mask_x,mask_y,mask_width,mask_height,mask_dim;};
inline float3 source_pixel(texture2d<float,access::read> source,int2 p,uint space){
 p=clamp(p,int2(0),int2(source.get_width()-1,source.get_height()-1));A3 s=a3v(source.read(uint2(p)).rgb);if(!finite3(s))return float3(.35,0,.35);return v3(space<2?clip3(s,1.0f):s);
}
inline float3 source_display(texture2d<float,access::read> source,texture2d<float,access::read> work,constant ViewParams& p,float2 q){
 float2 pos=(q-float2(p.offset_x,p.offset_y))/p.scale;
 if(any(pos<0)||pos.x>=source.get_width()||pos.y>=source.get_height())return float3(.004,.006,.008);
 float3 color;
 if(p.false_color){float4 w=work.read(uint2(pos));color=w.a>0?v3(false_color_rgb(dot3(y_coefficients(working_gamut(p.space)),a3v(w.rgb)))):float3(.35,0,.35);}
 else {float2 f=pos-.5f;int2 low=int2(floor(f));float2 t=f-float2(low);color=mix(mix(source_pixel(source,low,p.space),source_pixel(source,low+int2(1,0),p.space),t.x),mix(source_pixel(source,low+int2(0,1),p.space),source_pixel(source,low+int2(1,1),p.space),t.x),t.y);}
 if(!mask_contains(p.mask_kind,pos.x,pos.y,p.mask_x,p.mask_y,p.mask_width,p.mask_height))color*=p.mask_dim;
 return color;
}
inline float4 ui_pixel(uchar4 pixel,float white){A3 v=ui_rgb_to_linear_p3((uint(pixel.r)<<16)|(uint(pixel.g)<<8)|uint(pixel.b));return float4(v3(v)*white,float(pixel.a)/255.0f);}
kernel void render_source(texture2d<float,access::read> source[[texture(0)]],texture2d<float,access::read> work[[texture(1)]],texture2d<float,access::write> output[[texture(2)]],constant ViewParams& p[[buffer(0)]],device const uchar4* marks[[buffer(1)]],constant uint& has_marks[[buffer(2)]],uint2 q[[thread_position_in_grid]]){
 if(q.x>=p.width||q.y>=p.height)return;
 float3 color=source_display(source,work,p,float2(q)+.5f);
 if(has_marks){uchar4 mark=marks[q.y*p.width+q.x];if(mark.a){float4 top=ui_pixel(mark,1.0f);color=mix(color,top.rgb,top.a);}}
 output.write(float4(color,1),q);
}
struct ReportParams {uint width,height;int source_x,source_y,source_width,source_height;float ui_white;};
kernel void compose_report(texture2d<float,access::read> source[[texture(0)]],texture2d<float,access::read> work[[texture(1)]],texture2d<float,access::write> output[[texture(2)]],device const uchar4* underlay[[buffer(0)]],device const uchar4* overlay[[buffer(1)]],constant ReportParams& r[[buffer(2)]],constant ViewParams& p[[buffer(3)]],uint2 q[[thread_position_in_grid]]){
 if(q.x>=r.width||q.y>=r.height)return;uint index=q.y*r.width+q.x;float4 base=ui_pixel(underlay[index],r.ui_white);float3 color=base.rgb*base.a;
 int2 local=int2(q)-int2(r.source_x,r.source_y);if(all(local>=0)&&local.x<r.source_width&&local.y<r.source_height)color=source_display(source,work,p,float2(local)+.5f);
 float4 top=ui_pixel(overlay[index],r.ui_white);output.write(float4(mix(color,top.rgb,top.a),1),q);
}
// Only style colors use 16-bit fixed-point sums. Measurement values stay FP32.
// Split uint64 addition remains deterministic beyond 2^24 pixels, where an
// atomic float sum can saturate, and avoids retry loops under hot-bin contention.
inline void color_add(device atomic_uint* colors,uint index,float3 color){
 for(uint c=0;c<3;++c){uint value=uint(floor(clamp(color[c],0.0f,1.0f)*65535.0f+.5f));if(value==0)continue;
  device atomic_uint* low=colors+(index*3+c)*2;uint old=atomic_fetch_add_explicit(low,value,memory_order_relaxed);if(old>0xffffffffu-value)atomic_fetch_add_explicit(low+1,1u,memory_order_relaxed);}
}
inline float3 display_color(A3 work,uint space){return v3(display_srgb_from_work(work,space));}
struct StatsParams {uint width,height,space,mask_kind;float white,mx,my,mw,mh;uint wave_width,wave_height,vector_width,vector_height,bins,wave_flags,hist_flags,hist_mode,vector_mode,vector_enabled;float amplitude_zoom,amplitude_pan,vector_scale_x,vector_scale_y,vector_center_x,vector_center_y;};
kernel void vector_bounds(texture2d<float,access::read> work[[texture(0)]],device float4* output[[buffer(0)]],constant WorkParams& p[[buffer(1)]],uint i[[thread_position_in_grid]],uint lane[[thread_index_in_threadgroup]],uint group[[threadgroup_position_in_grid]]){
 threadgroup float4 bounds[256];float4 v=0;
 if(i<p.width*p.height){float4 w=work.read(uint2(i%p.width,i/p.width));if(w.a>0){Signals s=signals_from_work(a3v(w.rgb),p.space,p.white);if(s.valid)v=abs(float4(s.ncl.y,s.ncl.z,s.perceptual.y,s.perceptual.z));}}
 bounds[lane]=v;threadgroup_barrier(mem_flags::mem_threadgroup);
 for(uint stride=128;stride>0;stride/=2){if(lane<stride)bounds[lane]=max(bounds[lane],bounds[lane+stride]);threadgroup_barrier(mem_flags::mem_threadgroup);}
 if(lane==0)output[group]=bounds[0];
}
kernel void project_signals(texture2d<float,access::read> work[[texture(0)]],texture2d<float,access::write> wave[[texture(1)]],texture2d<float,access::write> vector[[texture(2)]],constant WorkParams& p[[buffer(0)]],constant uint& flags[[buffer(1)]],uint2 q[[thread_position_in_grid]]){
 if(q.x>=p.width||q.y>=p.height)return;
 float4 w=work.read(q);Signals s=signals_from_work(a3v(w.rgb),p.space,p.white);
 if(flags&1u)wave.write(float4(v3(s.encoded),s.intensity),q);
 if(flags&2u)vector.write(float4(s.perceptual.y,s.perceptual.z,0,0),q);
}
kernel void refine_statistics(texture2d<float,access::read> work[[texture(0)]],texture2d<float,access::read> wave[[texture(1)]],texture2d<float,access::read> vector[[texture(2)]],device atomic_uint* counts[[buffer(0)]],device atomic_uint* colors[[buffer(1)]],constant StatsParams& p[[buffer(2)]],uint2 q[[thread_position_in_grid]]){
 if(q.x>=p.width||q.y>=p.height||!mask_contains(p.mask_kind,float(q.x)+.5f,float(q.y)+.5f,p.mx,p.my,p.mw,p.mh))return;
 float4 w=work.read(q);if(w.a==0)return;
 float3 color=display_color(a3v(w.rgb),p.space);uint wave_size=p.wave_width*p.wave_height;
 if(p.wave_flags){float4 encoded=wave.read(q);float4 values=float4(encoded.a,encoded.rgb);
  for(uint c=0;c<4;++c)if(p.wave_flags&(1u<<c)){float v=(values[c]-p.amplitude_pan)*p.amplitude_zoom;
   if(v>=0&&v<=1){uint k=signal_bin(v,p.wave_height)*p.wave_width+source_column_bin(q.x,p.width,p.wave_width);
    atomic_fetch_add_explicit(counts+c*wave_size+k,1u,memory_order_relaxed);if(c==0)color_add(colors,k,color);}}}
 if(p.vector_enabled){float2 plane;if(p.vector_mode==0){A3 ncl=ncl_from_signal(a3v(wave.read(q).rgb),working_gamut(p.space));plane=float2(ncl.y,ncl.z);}else plane=vector.read(q).rg;
  float2 v=.5f+(plane-float2(p.vector_center_x,p.vector_center_y))*float2(p.vector_scale_x,p.vector_scale_y);
  if(all(v>=0)&&all(v<=1)){uint k=signal_bin(v.y,p.vector_height)*p.vector_width+signal_bin(v.x,p.vector_width);
   atomic_fetch_add_explicit(counts+4*wave_size+k,1u,memory_order_relaxed);color_add(colors,wave_size+k,color);}}
}
struct Partial {float4 source;float4 work;uint4 counts;float4 x;};
kernel void statistics(texture2d<float,access::read> source[[texture(0)]],texture2d<float,access::read> work[[texture(1)]],device atomic_uint* counts[[buffer(0)]],device atomic_uint* colors[[buffer(1)]],device Partial* partials[[buffer(2)]],constant StatsParams& p[[buffer(3)]],uint i[[thread_position_in_grid]],uint lane[[thread_index_in_threadgroup]],uint group[[threadgroup_position_in_grid]]){
 threadgroup float4 source_sum[256];threadgroup float4 work_sum[256];threadgroup uint4 number_sum[256];threadgroup float x_sum[256];
 float4 ss=0,ww=0;uint4 ns=0;float xx=0;
 if(i<p.width*p.height){uint2 xy=uint2(i%p.width,i/p.width);
  if(mask_contains(p.mask_kind,float(xy.x)+.5f,float(xy.y)+.5f,p.mx,p.my,p.mw,p.mh)){
   float4 s=source.read(xy),w=work.read(xy);Signals signals=signals_from_work(a3v(w.rgb),p.space,p.white);
   if(!finite3(a3v(s.rgb))||w.a==0||!signals.valid){ns.y=1;}else{
    ss=float4(s.rgb,0);ww=float4(w.rgb,0);ns.x=1;ns.z=signals.hue_valid?1u:0u;xx=(float(xy.x)+.5f)/float(p.width);float3 color=display_color(a3v(w.rgb),p.space);
    uint wave_size=p.wave_width*p.wave_height,vector_size=p.vector_width*p.vector_height;
    float4 values=float4(signals.intensity,v3(signals.encoded));
    for(uint c=0;c<4;++c)if((p.wave_flags&(1u<<c))!=0){uint k=signal_bin(values[c],p.wave_height)*p.wave_width+source_column_bin(xy.x,p.width,p.wave_width);atomic_fetch_add_explicit(counts+c*wave_size+k,1u,memory_order_relaxed);if(c==0)color_add(colors,k,color);}
    if(p.vector_enabled){float2 plane=p.vector_mode==0?float2(signals.ncl.y,signals.ncl.z):float2(signals.perceptual.y,signals.perceptual.z);float2 v=.5f+plane*float2(p.vector_scale_x,p.vector_scale_y);
     uint k=signal_bin(v.y,p.vector_height)*p.vector_width+signal_bin(v.x,p.vector_width);atomic_fetch_add_explicit(counts+4*wave_size+k,1u,memory_order_relaxed);color_add(colors,wave_size+k,color);}
    if(p.hist_mode!=5||signals.hue_valid){float3 h=p.hist_mode==0?float3(signals.intensity):(p.hist_mode==5?float3(signals.hue/360.0f):((p.hist_mode==2||p.hist_mode==4)?v3(signals.adobe):v3(signals.encoded)));
     for(uint c=0;c<4;++c)if((p.hist_flags&(1u<<c))!=0){uint b=signal_bin(h[c==0?0:c-1],p.bins);atomic_fetch_add_explicit(counts+4*wave_size+vector_size+c*p.bins+b,1u,memory_order_relaxed);if(c==0)color_add(colors,wave_size+vector_size+b,color);}}
   }
  }
 }
 source_sum[lane]=ss;work_sum[lane]=ww;number_sum[lane]=ns;x_sum[lane]=xx;threadgroup_barrier(mem_flags::mem_threadgroup);
 for(uint stride=128;stride>0;stride/=2){if(lane<stride){source_sum[lane]+=source_sum[lane+stride];work_sum[lane]+=work_sum[lane+stride];number_sum[lane]+=number_sum[lane+stride];x_sum[lane]+=x_sum[lane+stride];}threadgroup_barrier(mem_flags::mem_threadgroup);}
 if(lane==0){Partial out;out.source=source_sum[0];out.work=work_sum[0];out.counts=number_sum[0];out.x=float4(x_sum[0],0,0,0);partials[group]=out;}
}
kernel void sample_region(texture2d<float,access::read> source[[texture(0)]],texture2d<float,access::read> work[[texture(1)]],device float4* output[[buffer(0)]],constant uint4& rect[[buffer(1)]],uint2 q[[thread_position_in_grid]]){
 if(q.x>=rect.z||q.y>=rect.w)return;uint index=q.y*rect.z+q.x;output[index*2]=source.read(q+rect.xy);output[index*2+1]=work.read(q+rect.xy);
}
)METAL";

Error native_error(
    const char *reason, NSError *error = nil,
    std::source_location origin = std::source_location::current()) {
  auto e = analysis_error(reason, ErrorCode::presenter_failed, origin);
  e.module = "MacAnalysis";
  if (error) {
    e.safe_context["nativeCode"] = std::to_string(error.code);
    e.safe_context["nativeDescription"] = error.localizedDescription.UTF8String;
  }
  return e;
}
struct Runtime {
  id<MTLDevice> device{nil};
  id<MTLCommandQueue> queue{nil};
  id<MTLLibrary> library{nil};
  std::map<std::string, id<MTLComputePipelineState>> pipelines;
  std::optional<Error> failure;
  Runtime() {
    @autoreleasepool {
      device = macos_gpu_device();
      queue = macos_gpu_queue();
      if (!device || !queue) {
        failure = native_error("metal_device_unavailable");
        return;
      }
      std::string source =
          std::string(kIntrinsics) + analysis_math::metal_source + kKernels;
      NSError *error = nil;
      MTLCompileOptions *options = [MTLCompileOptions new];
      options.mathMode = MTLMathModeSafe;
      options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
      library = [device
          newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                       options:options
                         error:&error];
      if (!library) {
        failure = native_error("analysis_shader_compilation_failed", error);
        return;
      }
      for (const char *name :
           {"copy_roi", "upload_linear", "apply_patches", "blur_horizontal",
            "build_work", "render_source", "compose_report", "statistics",
            "sample_region", "vector_bounds", "project_signals",
            "refine_statistics"}) {
        auto function =
            [library newFunctionWithName:[NSString stringWithUTF8String:name]];
        auto pipeline = [device newComputePipelineStateWithFunction:function
                                                              error:&error];
        if (!pipeline) {
          failure = native_error("analysis_pipeline_failed", error);
          return;
        }
        pipelines[name] = pipeline;
      }
    }
  }
};
std::shared_ptr<Runtime> runtime() {
  static auto value = std::make_shared<Runtime>();
  return value;
}
id<MTLTexture> texture(Runtime &r, PixelSize size) {
  if (size.width <= 0 || size.height <= 0 || size.width > 16384 ||
      size.height > 16384)
    return nil;
  auto desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                   width:NSUInteger(size.width)
                                  height:NSUInteger(size.height)
                               mipmapped:NO];
  desc.storageMode = MTLStorageModePrivate;
  desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  return [r.device newTextureWithDescriptor:desc];
}
void dispatch(id<MTLComputeCommandEncoder> e, id<MTLComputePipelineState> p,
              PixelSize size) {
  [e setComputePipelineState:p];
  NSUInteger w = p.threadExecutionWidth,
             h = std::max<NSUInteger>(
                 1,
                 std::min<NSUInteger>(8, p.maxTotalThreadsPerThreadgroup / w));
  [e dispatchThreads:MTLSizeMake(NSUInteger(size.width),
                                 NSUInteger(size.height), 1)
      threadsPerThreadgroup:MTLSizeMake(w, h, 1)];
}
Result<bool, Error> wait_command(id<MTLCommandBuffer> command) {
  [command commit];
  [command waitUntilCompleted];
  return command.status == MTLCommandBufferStatusCompleted
             ? Result<bool, Error>::success(true)
             : Result<bool, Error>::failure(
                   native_error("analysis_execution_failed", command.error));
}
struct WorkParams {
  std::uint32_t width, height, space, radius;
  float white;
  std::uint32_t vertical;
};
struct ViewParams {
  std::uint32_t width, height, space, false_color;
  float scale, offset_x, offset_y, white;
  std::uint32_t mask_kind;
  float mask_x, mask_y, mask_width, mask_height, mask_dim;
};
ViewParams parameters(const SourceView &v) {
  return {std::uint32_t(v.target_size.width),
          std::uint32_t(v.target_size.height),
          unsigned(v.settings.working_space),
          v.false_color ? 1u : 0u,
          float(v.scale),
          float(v.offset_x),
          float(v.offset_y),
          float(v.settings.reference_white_nits),
          v.mask.enabled ? (v.mask.shape == MaskShape::rectangle ? 1u : 2u)
                         : 0u,
          float(v.mask.bounds.x),
          float(v.mask.bounds.y),
          float(v.mask.bounds.width),
          float(v.mask.bounds.height),
          float(v.mask_outside_factor)};
}
struct WorkCache {
  LinearSourceRef source;
  Settings settings;
  id<MTLTexture> pixels{nil};
  double prepare_ms{};
  double prepare_gpu_ms{};
  std::size_t peak{};
  std::array<float, 4> vector_bounds{}; // abs Cb/Cr/T/P or a*/b*, full W
  id<MTLTexture> projected_wave{nil};   // lazy RGBA32F encoded RGB + I/Y'
  id<MTLTexture> projected_vector{nil}; // lazy RG32F T/P or a*/b*
  std::size_t projected_bytes{};
};
struct Shared {
  std::shared_ptr<Runtime> gpu = runtime();
  std::mutex mutex;
  std::shared_ptr<const WorkCache> work;
};
Result<std::shared_ptr<const WorkCache>, Error>
build_work(Shared &state, const Input &input, const Settings &settings) {
  using Out = Result<std::shared_ptr<const WorkCache>, Error>;
  {
    std::lock_guard lock(state.mutex);
    if (state.work && state.work->source == input.source &&
        state.work->settings == settings)
      return Out::success(state.work);
    state.work.reset(); // release stale W before allocating its replacement
  }
  auto &r = *state.gpu;
  if (r.failure)
    return Out::failure(*r.failure);
  auto src = macos_source_texture(input.source);
  if (!src || !valid_settings(settings))
    return Out::failure(analysis_error("invalid_native_analysis_input"));
  auto start = std::chrono::steady_clock::now();
  auto size = input.source->size_px();
  auto work = texture(r, size);
  if (!work)
    return Out::failure(native_error("work_texture_allocation_failed"));
  auto weights = gaussian_weights(settings.blur_sigma_px);
  std::uint32_t radius = std::uint32_t(weights.size() / 2);
  auto horizontal = radius ? texture(r, size) : src;
  if (!horizontal)
    return Out::failure(native_error("blur_texture_allocation_failed"));
  auto command = [r.queue commandBuffer];
  if (!command)
    return Out::failure(native_error("work_command_failed"));
  WorkParams p{unsigned(size.width),
               unsigned(size.height),
               unsigned(settings.working_space),
               radius,
               float(settings.reference_white_nits),
               0};
  if (radius) {
    auto e = [command computeCommandEncoder];
    if (!e)
      return Out::failure(native_error("blur_encoder_failed"));
    [e setTexture:src atIndex:0];
    [e setTexture:horizontal atIndex:1];
    [e setBytes:&p length:sizeof(p) atIndex:0];
    [e setBytes:weights.data() length:weights.size() * sizeof(float) atIndex:1];
    dispatch(e, r.pipelines.at("blur_horizontal"), size);
    [e endEncoding];
  }
  auto e = [command computeCommandEncoder];
  if (!e)
    return Out::failure(native_error("work_encoder_failed"));
  [e setTexture:src atIndex:0];
  [e setTexture:horizontal atIndex:1];
  [e setTexture:work atIndex:2];
  [e setBytes:&p length:sizeof(p) atIndex:0];
  [e setBytes:weights.data() length:weights.size() * sizeof(float) atIndex:1];
  dispatch(e, r.pipelines.at("build_work"), size);
  [e endEncoding];
  const auto bound_groups =
      (std::size_t(size.width) * std::size_t(size.height) + 255) / 256;
  auto bounds =
      [r.device newBufferWithLength:bound_groups * sizeof(std::array<float, 4>)
                            options:MTLResourceStorageModeShared];
  auto bound_encoder = [command computeCommandEncoder];
  auto bound_pipeline = r.pipelines.at("vector_bounds");
  if (!bounds || !bound_encoder ||
      bound_pipeline.maxTotalThreadsPerThreadgroup < 256)
    return Out::failure(native_error("vector_bounds_allocation_failed"));
  [bound_encoder setComputePipelineState:bound_pipeline];
  [bound_encoder setTexture:work atIndex:0];
  [bound_encoder setBuffer:bounds offset:0 atIndex:0];
  [bound_encoder setBytes:&p length:sizeof(p) atIndex:1];
  [bound_encoder dispatchThreadgroups:MTLSizeMake(bound_groups, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
  [bound_encoder endEncoding];
  auto done = wait_command(command);
  if (!done)
    return Out::failure(done.error());
  auto ready = input.source->wait_until_ready();
  if (!ready)
    return Out::failure(ready.error());
  auto cache = std::make_shared<WorkCache>();
  cache->source = input.source;
  cache->settings = settings;
  cache->pixels = work;
  auto reduced_bounds =
      static_cast<const std::array<float, 4> *>(bounds.contents);
  for (std::size_t i = 0; i < bound_groups; ++i)
    for (std::size_t c = 0; c < 4; ++c)
      cache->vector_bounds[c] =
          std::max(cache->vector_bounds[c], reduced_bounds[i][c]);
  cache->peak = input.source->byte_count() * (radius ? 3 : 2) + bounds.length;
  cache->prepare_ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count();
  cache->prepare_gpu_ms = (command.GPUEndTime - command.GPUStartTime) * 1000.0;
  {
    std::lock_guard lock(state.mutex);
    state.work = cache;
  }
  return Out::success(std::move(cache));
}
Result<std::shared_ptr<const WorkCache>, Error>
ensure_projected(Shared &state, std::shared_ptr<const WorkCache> work,
                 bool wave, bool vector) {
  using Out = Result<std::shared_ptr<const WorkCache>, Error>;
  const bool add_wave = wave && !work->projected_wave;
  const bool add_vector = vector && !work->projected_vector;
  if (!add_wave && !add_vector)
    return Out::success(std::move(work));
  auto &r = *state.gpu;
  const auto size = work->source->size_px();
  auto cache = std::make_shared<WorkCache>(*work);
  auto allocate = [&](MTLPixelFormat format) {
    auto descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:format
                                     width:NSUInteger(size.width)
                                    height:NSUInteger(size.height)
                                 mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    return [r.device newTextureWithDescriptor:descriptor];
  };
  if (add_wave)
    cache->projected_wave = allocate(MTLPixelFormatRGBA32Float);
  if (add_vector)
    cache->projected_vector = allocate(MTLPixelFormatRG32Float);
  if ((add_wave && !cache->projected_wave) ||
      (add_vector && !cache->projected_vector))
    return Out::failure(native_error("projected_signals_allocation_failed"));
  auto command = [r.queue commandBuffer];
  auto encoder = [command computeCommandEncoder];
  if (!command || !encoder)
    return Out::failure(native_error("projected_signals_command_failed"));
  const std::uint32_t flags = (add_wave ? 1u : 0u) | (add_vector ? 2u : 0u);
  WorkParams p{unsigned(size.width),
               unsigned(size.height),
               unsigned(work->settings.working_space),
               0,
               float(work->settings.reference_white_nits),
               0};
  [encoder setTexture:work->pixels atIndex:0];
  [encoder setTexture:add_wave ? cache->projected_wave : work->pixels
              atIndex:1];
  [encoder setTexture:add_vector ? cache->projected_vector : work->pixels
              atIndex:2];
  [encoder setBytes:&p length:sizeof(p) atIndex:0];
  [encoder setBytes:&flags length:sizeof(flags) atIndex:1];
  dispatch(encoder, r.pipelines.at("project_signals"), size);
  [encoder endEncoding];
  auto done = wait_command(command);
  if (!done)
    return Out::failure(done.error());
  cache->projected_bytes = std::size_t(size.width) * size.height *
                           ((cache->projected_wave ? 16u : 0u) +
                            (cache->projected_vector ? 8u : 0u));
  cache->peak = std::max(cache->peak, work->source->byte_count() * 2 +
                                          cache->projected_bytes);
  {
    std::lock_guard lock(state.mutex);
    state.work = cache;
  }
  return Out::success(std::move(cache));
}
Result<std::size_t, Error>
refine_gpu_scopes(Shared &state, const WorkCache &work, const Request &request,
                  ResultData &result, bool refine_wave, bool refine_vector) {
  using Out = Result<std::size_t, Error>;
  const auto &o = request.scopes;
  if (refine_wave)
    result.waveform_detail = {};
  if (refine_vector)
    result.vectorscope_detail = {};
  result.detail_ms = result.detail_gpu_ms = 0;
  const bool wave = refine_wave && o.waveform_visible && o.wave_detail_width;
  const bool vector =
      refine_vector && o.vector_visible && o.vector_detail_width;
  if (!wave && !vector)
    return Out::success(0);
  const auto start = std::chrono::steady_clock::now();
  auto &r = *state.gpu;
  const auto size = work.source->size_px();
  const auto ww =
      wave ? std::min(o.wave_detail_width, unsigned(size.width)) : 0u;
  const auto wh = wave ? o.wave_detail_height : 0u;
  const auto vw = vector ? o.vector_detail_width : 0u,
             vh = vector ? o.vector_detail_height : 0u;
  const auto wave_size = std::size_t(ww) * wh,
             vector_size = std::size_t(vw) * vh;
  const auto count_bytes =
      std::max(std::size_t{4}, (wave_size * 4 + vector_size) * 4);
  const auto color_bytes =
      std::max(std::size_t{4}, (wave_size + vector_size) * 24);
  auto counts = [r.device newBufferWithLength:count_bytes
                                      options:MTLResourceStorageModeShared];
  auto colors = [r.device newBufferWithLength:color_bytes
                                      options:MTLResourceStorageModeShared];
  if (!counts || !colors)
    return Out::failure(native_error("detail_grid_allocation_failed"));
  std::memset(counts.contents, 0, count_bytes);
  std::memset(colors.contents, 0, color_bytes);
  if (refine_wave)
    result.waveform_detail_view = o.amplitude_view;
  if (vector) {
    const auto extents = vector_detail_extents(request);
    result.vector_detail_x_extent = extents[0];
    result.vector_detail_y_extent = extents[1];
    result.vector_detail_zoom = o.vector_zoom;
    result.vector_detail_center = o.vector_pan;
  }
  std::uint32_t wave_flags = !wave                                ? 0u
                             : o.wave_mode == WaveMode::intensity ? 1u
                             : o.wave_mode == WaveMode::parade_intensity_rgb
                                 ? 15u
                                 : 14u;
  struct StatsParams {
    std::uint32_t width, height, space, mask_kind;
    float white, mx, my, mw, mh;
    std::uint32_t ww, wh, vw, vh, bins, wave_flags, hist_flags, hist_mode,
        vector_mode, vector_enabled;
    float az, ap, vx, vy, cx, cy;
  };
  StatsParams p{unsigned(size.width),
                unsigned(size.height),
                unsigned(request.settings.working_space),
                request.mask.enabled
                    ? (request.mask.shape == MaskShape::rectangle ? 1u : 2u)
                    : 0u,
                float(request.settings.reference_white_nits),
                float(request.mask.bounds.x),
                float(request.mask.bounds.y),
                float(request.mask.bounds.width),
                float(request.mask.bounds.height),
                ww,
                wh,
                vw,
                vh,
                0,
                wave_flags,
                0,
                0,
                unsigned(o.vector_mode),
                vector ? 1u : 0u,
                float(o.amplitude_view.zoom),
                float(o.amplitude_view.pan),
                vector ? float(1 / (2 * result.vector_detail_x_extent)) : 0,
                vector ? float(1 / (2 * result.vector_detail_y_extent)) : 0,
                float(o.vector_pan[0]), float(o.vector_pan[1])};
  auto command = [r.queue commandBuffer];
  auto encoder = [command computeCommandEncoder];
  if (!command || !encoder)
    return Out::failure(native_error("detail_grid_command_failed"));
  [encoder setTexture:work.pixels atIndex:0];
  [encoder setTexture:work.projected_wave ? work.projected_wave : work.pixels
              atIndex:1];
  [encoder
      setTexture:work.projected_vector ? work.projected_vector : work.pixels
         atIndex:2];
  [encoder setBuffer:counts offset:0 atIndex:0];
  [encoder setBuffer:colors offset:0 atIndex:1];
  [encoder setBytes:&p length:sizeof(p) atIndex:2];
  dispatch(encoder, r.pipelines.at("refine_statistics"), size);
  [encoder endEncoding];
  auto done = wait_command(command);
  if (!done)
    return Out::failure(done.error());
  result.detail_gpu_ms = (command.GPUEndTime - command.GPUStartTime) * 1000;
  const auto *integers = static_cast<const std::uint32_t *>(counts.contents);
  const auto *sums = static_cast<const std::uint32_t *>(colors.contents);
  auto fill = [&](DensityGrid &grid, unsigned width, unsigned height,
                  std::size_t count_offset,
                  std::optional<std::size_t> color_offset) {
    grid.width = width;
    grid.height = height;
    const auto n = std::size_t(width) * height;
    grid.counts.assign(integers + count_offset, integers + count_offset + n);
    grid.maximum = *std::max_element(grid.counts.begin(), grid.counts.end());
    if (color_offset) {
      grid.color_sums.resize(n);
      for (std::size_t i = 0; i < n; ++i)
        for (std::size_t c = 0; c < 3; ++c) {
          const auto offset = ((*color_offset + i) * 3 + c) * 2;
          grid.color_sums[i][c] =
              float(double(std::uint64_t(sums[offset]) +
                           (std::uint64_t(sums[offset + 1]) << 32)) /
                    65535.);
        }
    }
  };
  for (std::size_t c = 0; c < 4; ++c)
    if (wave_flags & (1u << c)) {
      fill(result.waveform_detail[c], ww, wh, c * wave_size,
           c == 0 ? std::optional<std::size_t>{0} : std::nullopt);
      set_waveform_column_coverage(result.waveform_detail[c],
                                   unsigned(size.width));
    }
  if (vector)
    fill(result.vectorscope_detail, vw, vh, 4 * wave_size, wave_size);
  result.detail_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - start)
                         .count();
  return Out::success(count_bytes + color_bytes);
}
id<MTLBuffer> operation_buffer(Runtime &r, const SourceView &view) {
  const std::uint32_t transparent = 0;
  return
      [r.device newBufferWithBytes:view.operation_overlay
                                       ? view.operation_overlay->rgba.data()
                                       : reinterpret_cast<const std::uint8_t *>(
                                             &transparent)
                            length:view.operation_overlay
                                       ? view.operation_overlay->rgba.size()
                                       : 4
                           options:MTLResourceStorageModeShared];
}
void bind_operation_overlay(id<MTLComputeCommandEncoder> encoder,
                            id<MTLBuffer> buffer, const SourceView &view) {
  const std::uint32_t enabled = view.operation_overlay ? 1 : 0;
  [encoder setBuffer:buffer offset:0 atIndex:1];
  [encoder setBytes:&enabled length:sizeof(enabled) atIndex:2];
}
Result<LinearSourceRef, Error> render_source(Shared &state, const Input &input,
                                             const SourceView &view,
                                             bool build) {
  using Out = Result<LinearSourceRef, Error>;
  auto &r = *state.gpu;
  if (r.failure)
    return Out::failure(*r.failure);
  if (!input.source || !valid_source_view(view))
    return Out::failure(analysis_error("invalid_source_view"));
  auto src = macos_source_texture(input.source);
  if (!src)
    return Out::failure(analysis_error("unsupported_source_binding"));
  id<MTLTexture> work = src;
  if (view.false_color) {
    if (build) {
      auto cache = build_work(state, input, view.settings);
      if (!cache)
        return Out::failure(cache.error());
      work = cache.value()->pixels;
    } else {
      std::lock_guard lock(state.mutex);
      if (!state.work || state.work->source != input.source ||
          state.work->settings != view.settings)
        return Out::failure(analysis_error("analysis_work_pending",
                                           ErrorCode::precondition_failed));
      work = state.work->pixels;
    }
  }
  auto output = texture(r, view.target_size);
  auto marks = operation_buffer(r, view);
  auto command = [r.queue commandBuffer];
  if (!output || !command || !marks)
    return Out::failure(native_error("source_render_allocation_failed"));
  auto e = [command computeCommandEncoder];
  if (!e)
    return Out::failure(native_error("source_render_encoder_failed"));
  auto p = parameters(view);
  [e setTexture:src atIndex:0];
  [e setTexture:work atIndex:1];
  [e setTexture:output atIndex:2];
  [e setBytes:&p length:sizeof(p) atIndex:0];
  bind_operation_overlay(e, marks, view);
  dispatch(e, r.pipelines.at("render_source"), view.target_size);
  [e endEncoding];
  return macos_wrap_linear_texture(output, command, {input.source});
}
class MacAnalysisPort final : public AnalysisPort {
public:
  explicit MacAnalysisPort(std::shared_ptr<Shared> state)
      : state_(std::move(state)) {}
  Result<LinearSourceRef, Error> prepare(const SelectionRoiView &,
                                         const AnnotationPixelPlan &) override;
  Result<ResultRef, Error> analyze(const Input &, const Request &) override;
  Result<LinearSourceRef, Error> compose_report(const Input &,
                                                const ReportPlan &) override;

private:
  std::shared_ptr<Shared> state_;
  std::optional<Request> last_request_;
  LinearSourceRef last_source_;
  ResultRef last_result_;
};
class MacAnalysisPresenter final : public AnalysisPresenterPort {
  struct Frame {
    Input input;
    SourceView view;
    id<MTLTexture> source{nil}, work{nil};
    CAMetalLayer *layer{nil};
    std::uint64_t generation{};
    unsigned retries{};
  };
  struct Delivery {
    std::mutex mutex;
    std::optional<Frame> pending;
    bool running{}, closed{};
    std::uint64_t generation{};
    std::uint64_t submitted{}, completed{}, failed{}, dropped{},
        completed_generation{};
    std::function<void(Error)> on_error;
    std::shared_ptr<Runtime> gpu;
    std::shared_ptr<Shared> state;
    std::shared_ptr<const UiImage> marks_image;
    id<MTLBuffer> marks{nil};
  };
  // The native view/layer layout stays on the main thread. Only immutable
  // resources and a strong CAMetalLayer reference cross this boundary. One
  // in-flight command plus one replaceable pending frame bounds GPU pressure;
  // neither nextDrawable nor waitUntilCompleted blocks Qt input processing.
  static void failed(const std::shared_ptr<Delivery> &delivery,
                     const char *reason) noexcept {
    try {
      std::function<void(Error)> callback;
      {
        std::lock_guard lock(delivery->mutex);
        ++delivery->failed;
        callback = delivery->on_error;
      }
      os_log_error(OS_LOG_DEFAULT,
                   "SeriousShot MacAnalysis async presentation: %{public}s",
                   reason);
      if (callback)
        callback(native_error(reason));
    } catch (...) { /* Presentation failure reporting must not terminate GUI. */
    }
  }
  // false means a newer pending state already exists: keep draining it now.
  // true means this worker stops (closed, scheduled retry, or exhausted).
  static bool retry_acquire(const std::shared_ptr<Delivery> &delivery,
                            Frame frame, const char *reason) {
    bool retry = false;
    {
      std::lock_guard lock(delivery->mutex);
      if (delivery->closed) {
        delivery->running = false;
        return true;
      }
      if (delivery->pending)
        return false;
      if (frame.retries < 2) {
        ++frame.retries;
        delivery->pending = std::move(frame);
        retry = true;
      } else {
        ++delivery->dropped;
        delivery->running = false;
      }
    }
    if (retry) {
      // Capture an owning value. Capturing the reference parameter itself
      // would outlive the drain stack that supplied it after this worker exits.
      auto retained_delivery = delivery;
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 16 * NSEC_PER_MSEC),
                     dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0),
                     ^{
                       drain(retained_delivery);
                     });
    } else
      failed(delivery, reason);
    return true;
  }
  static void drain(const std::shared_ptr<Delivery> &delivery) noexcept {
    try {
      for (;;) {
        @autoreleasepool {
          Frame frame;
          {
            std::lock_guard lock(delivery->mutex);
            if (delivery->closed || !delivery->pending) {
              delivery->running = false;
              return;
            }
            frame = std::move(*delivery->pending);
            delivery->pending.reset();
          }
          auto &r = *delivery->gpu;
          id<CAMetalDrawable> drawable = [frame.layer nextDrawable];
          if (!drawable) {
            if (retry_acquire(delivery, std::move(frame),
                              "drawable_retry_exhausted"))
              return;
            continue;
          }
          {
            std::lock_guard lock(delivery->mutex);
            if (delivery->closed) {
              delivery->running = false;
              return;
            }
            // Waiting for a drawable may have outlived this input. Prefer the
            // newest same-sized request immediately; otherwise discard this
            // drawable and reacquire at the new surface size.
            if (delivery->pending) {
              if (delivery->pending->layer != frame.layer ||
                  delivery->pending->view.target_size != frame.view.target_size)
                continue;
              frame = std::move(*delivery->pending);
              delivery->pending.reset();
            }
          }
          if (drawable.texture.width !=
                  NSUInteger(frame.view.target_size.width) ||
              drawable.texture.height !=
                  NSUInteger(frame.view.target_size.height)) {
            // A resize can briefly return an older surface even with no more
            // input queued. Keep the final desired size for a bounded retry.
            if (retry_acquire(delivery, std::move(frame),
                              "drawable_size_retry_exhausted"))
              return;
            continue;
          }
          frame.work = frame.source;
          if (frame.view.false_color) {
            // Do not retain old W in pending frames or while nextDrawable
            // waits. Acquire the matching immutable W only for the single
            // actual GPU submission. A later analysis completion requeues if W
            // is not ready.
            std::lock_guard lock(delivery->state->mutex);
            const auto &cache = delivery->state->work;
            if (!cache || cache->source != frame.input.source ||
                cache->settings != frame.view.settings)
              continue;
            frame.work = cache->pixels;
          }
          if (!delivery->marks ||
              delivery->marks_image != frame.view.operation_overlay) {
            delivery->marks = operation_buffer(r, frame.view);
            delivery->marks_image = frame.view.operation_overlay;
          }
          if (!delivery->marks) {
            failed(delivery, "operation_buffer_allocation_failed");
            continue;
          }
          auto command = [r.queue commandBuffer];
          auto encoder = [command computeCommandEncoder];
          if (!command || !encoder) {
            failed(delivery, "present_command_allocation_failed");
            continue;
          }
          auto p = parameters(frame.view);
          [encoder setTexture:frame.source atIndex:0];
          [encoder setTexture:frame.work atIndex:1];
          [encoder setTexture:drawable.texture atIndex:2];
          [encoder setBytes:&p length:sizeof(p) atIndex:0];
          bind_operation_overlay(encoder, delivery->marks, frame.view);
          dispatch(encoder, r.pipelines.at("render_source"),
                   frame.view.target_size);
          [encoder endEncoding];
          [command presentDrawable:drawable];
          {
            std::lock_guard lock(delivery->mutex);
            ++delivery->submitted;
          }
          [command commit];
          [command waitUntilCompleted]; // bounded worker only; never GUI
          if (command.status == MTLCommandBufferStatusCompleted) {
            std::lock_guard lock(delivery->mutex);
            ++delivery->completed;
            delivery->completed_generation = frame.generation;
          } else
            failed(delivery, "present_gpu_execution_failed");
        }
      }
    } catch (...) {
      failed(delivery, "present_worker_exception");
      std::lock_guard lock(delivery->mutex);
      delivery->running = false;
      delivery->pending.reset();
    }
  }

public:
  explicit MacAnalysisPresenter(std::shared_ptr<Shared> state)
      : state_(std::move(state)), delivery_(std::make_shared<Delivery>()) {
    delivery_->gpu = state_->gpu;
    delivery_->state = state_;
  }
  ~MacAnalysisPresenter() override {
    std::lock_guard lock(delivery_->mutex);
    delivery_->closed = true;
    delivery_->pending.reset();
    // No join/wait on the GUI. The bounded worker retains its own resources
    // until its already-submitted command/drawable acquisition finishes.
  }
  MacAnalysisPresentationStatus status() const {
    std::lock_guard lock(delivery_->mutex);
    return {delivery_->generation, delivery_->submitted,
            delivery_->completed,  delivery_->failed,
            delivery_->dropped,    delivery_->completed_generation,
            delivery_->running,    delivery_->pending.has_value()};
  }
  void set_error_callback(std::function<void(Error)> callback) {
    std::lock_guard lock(delivery_->mutex);
    delivery_->on_error = std::move(callback);
  }
  Result<bool, Error> present(const Input &input, const SourceView &view,
                              std::uintptr_t surface) override {
    @autoreleasepool {
      auto &r = *state_->gpu;
      if (r.failure)
        return Result<bool, Error>::failure(*r.failure);
      if (!surface || !input.source || !valid_source_view(view))
        return Result<bool, Error>::failure(
            analysis_error("invalid_native_surface"));
      if (![NSThread isMainThread])
        return Result<bool, Error>::failure(
            native_error("analysis_surface_requires_main_thread"));
      auto src = macos_source_texture(input.source);
      if (!src)
        return Result<bool, Error>::failure(
            analysis_error("unsupported_source_binding"));
      if (view.false_color) {
        std::lock_guard lock(state_->mutex);
        if (!state_->work || state_->work->source != input.source ||
            state_->work->settings != view.settings)
          return Result<bool, Error>::failure(analysis_error(
              "analysis_work_pending", ErrorCode::precondition_failed));
      }
      NSView *native = (__bridge NSView *)reinterpret_cast<void *>(surface);
      native.wantsLayer = YES;
      CAMetalLayer *layer = nil;
      for (CALayer *candidate in native.layer.sublayers)
        if ([candidate isKindOfClass:CAMetalLayer.class] &&
            [candidate.name isEqualToString:@"hdrshot.analysis.source"]) {
          layer = (CAMetalLayer *)candidate;
          break;
        }
      if (!layer) {
        layer = [CAMetalLayer layer];
        layer.name = @"hdrshot.analysis.source";
        layer.device = r.device;
        layer.pixelFormat = MTLPixelFormatRGBA16Float;
        layer.framebufferOnly = NO;
        layer.maximumDrawableCount = 3;
        layer.allowsNextDrawableTimeout = YES;
        layer.opaque = YES;
        // Stay above Qt's backing-content layer even when it is recreated
        // after a paint/resize. This single surface includes all Source marks.
        layer.zPosition = 1;
        layer.wantsExtendedDynamicRangeContent = YES;
        CGColorSpaceRef cs =
            CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearDisplayP3);
        layer.colorspace = cs;
        CGColorSpaceRelease(cs);
        layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
        // QCocoaView owns its backing root. Keep it, but place the complete
        // Source composition above its background (no visible Qt sibling).
        [native.layer addSublayer:layer];
      }
      if (!layer.superlayer)
        return Result<bool, Error>::failure(
            native_error("analysis_layer_binding_failed"));
      layer.frame = native.bounds;
      layer.drawableSize =
          CGSizeMake(view.target_size.width, view.target_size.height);
      layer.contentsScale =
          native.bounds.size.width > 0
              ? double(view.target_size.width) / native.bounds.size.width
              : 1;
      bool start = false;
      {
        std::lock_guard lock(delivery_->mutex);
        delivery_->pending =
            Frame{input, view, src, nil, layer, ++delivery_->generation};
        start = !delivery_->running;
        delivery_->running = true;
      }
      if (start) {
        auto delivery = delivery_;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0),
                       ^{
                         drain(delivery);
                       });
      }
      return Result<bool, Error>::success(true);
    }
  }

private:
  std::shared_ptr<Shared> state_;
  std::shared_ptr<Delivery> delivery_;
};
Result<LinearSourceRef, Error>
MacAnalysisPort::prepare(const SelectionRoiView &source,
                         const AnnotationPixelPlan &annotations) try {
  using Out = Result<LinearSourceRef, Error>;
  @autoreleasepool {
    auto &r = *state_->gpu;
    if (r.failure)
      return Out::failure(*r.failure);
    if (!source.valid_storage() ||
        annotations.output_size_px != source.size_px ||
        !AnnotationPixelPlanValidator::valid(annotations))
      return Out::failure(analysis_error("invalid_clean_analysis_input"));
    auto output = texture(r, source.size_px);
    auto command = [r.queue commandBuffer];
    if (!output || !command)
      return Out::failure(native_error("analysis_roi_allocation_failed"));
    auto src = macos_source_texture(source.linear_source);
    std::vector<LinearSourceRef> dependencies;
    if (source.linear_source)
      dependencies.push_back(source.linear_source);
    if (src) {
      std::array<std::uint32_t, 2> origin{
          std::uint32_t((source.first_sample_offset / 4) % src.width),
          std::uint32_t((source.first_sample_offset / 4) / src.width)};
      auto e = [command computeCommandEncoder];
      [e setTexture:src atIndex:0];
      if (!e)
        return Out::failure(native_error("roi_encoder_failed"));
      [e setTexture:output atIndex:1];
      [e setBytes:origin.data() length:sizeof(origin) atIndex:0];
      dispatch(e, r.pipelines.at("copy_roi"), source.size_px);
      [e endEncoding];
    } else {
      // Explicit legacy/fixture boundary, never taken for the T22 native
      // source.
      auto cpu = FrameCropper::read_cpu_region(source);
      if (!cpu)
        return Out::failure(cpu.error());
      std::vector<std::array<float, 4>> values(
          std::size_t(source.size_px.width) *
          std::size_t(source.size_px.height));
      auto cpuview = FrameCropper::view(cpu.value());
      for (std::size_t i = 0; i < values.size(); ++i) {
        values[i][3] = 1;
        for (std::size_t c = 0; c < 3; ++c) {
          auto v =
              cpuview.sample(cpuview.first_sample_offset +
                             (i / std::size_t(source.size_px.width)) *
                                 cpuview.row_stride_samples +
                             (i % std::size_t(source.size_px.width)) * 4 + c);
          if (!v)
            return Out::failure(v.error());
          values[i][c] = ExtendedP3Mapper::source_linear(
              v.value(), cpuview.encoding.transfer);
        }
      }
      auto buffer =
          [r.device newBufferWithBytes:values.data()
                                length:values.size() * sizeof(values[0])
                               options:MTLResourceStorageModeShared];
      if (!buffer)
        return Out::failure(native_error("analysis_fixture_upload_failed"));
      auto e = [command computeCommandEncoder];
      [e setBuffer:buffer offset:0 atIndex:0];
      if (!e)
        return Out::failure(native_error("upload_encoder_failed"));
      [e setTexture:output atIndex:0];
      dispatch(e, r.pipelines.at("upload_linear"), source.size_px);
      [e endEncoding];
    }
    struct Patch {
      std::uint32_t first, count, x, y, offset, flags;
      float r, g, b, a;
    };
    struct Group {
      std::vector<Patch> spans;
      std::vector<std::array<float, 4>> samples;
      std::uint32_t count{};
    };
    std::map<LinearSampleRef, Group> groups;
    std::vector<LinearSampleRef> sample_dependencies;
    for (auto &span : annotations.annotation_owned_spans) {
      auto &group = groups[span.native_samples];
      Patch p{group.count,
              unsigned(span.length),
              unsigned(span.x),
              unsigned(span.y),
              unsigned(span.native_sample_offset),
              0,
              0,
              0,
              0,
              0};
      group.count += unsigned(span.length);
      if (!span.native_samples) {
        if (span.edge_samples.empty()) {
          auto v = annotation_linear_sample(span, 0);
          p.flags = 1;
          p.r = v[0];
          p.g = v[1];
          p.b = v[2];
          p.a = v[3];
        } else {
          p.offset = unsigned(group.samples.size());
          group.samples.insert(group.samples.end(), span.edge_samples.begin(),
                               span.edge_samples.end());
        }
      }
      group.spans.push_back(p);
    }
    for (auto &[owner, group] : groups) {
      auto patches =
          [r.device newBufferWithBytes:group.spans.data()
                                length:group.spans.size() * sizeof(Patch)
                               options:MTLResourceStorageModeShared];
      id<MTLBuffer> samples = nil;
      if (owner) {
        samples = macos_sample_buffer(owner);
        sample_dependencies.push_back(owner);
      } else {
        if (group.samples.empty())
          group.samples.push_back({0, 0, 0, 0});
        samples = [r.device newBufferWithBytes:group.samples.data()
                                        length:group.samples.size() * 16
                                       options:MTLResourceStorageModeShared];
      }
      if (!samples || !patches)
        return Out::failure(native_error("analysis_patch_binding_failed"));
      std::array<std::uint32_t, 2> limits{unsigned(group.spans.size()),
                                          group.count};
      auto e = [command computeCommandEncoder];
      [e setTexture:output atIndex:0];
      if (!e)
        return Out::failure(native_error("patch_encoder_failed"));
      [e setBuffer:patches offset:0 atIndex:0];
      [e setBuffer:samples offset:0 atIndex:1];
      [e setBytes:limits.data() length:sizeof(limits) atIndex:2];
      auto pipeline = r.pipelines.at("apply_patches");
      [e setComputePipelineState:pipeline];
      [e dispatchThreads:MTLSizeMake(group.count, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(pipeline.threadExecutionWidth, 1,
                                            1)];
      [e endEncoding];
    }
    return macos_wrap_linear_texture(output, command, std::move(dependencies),
                                     std::move(sample_dependencies));
  }
} catch (const std::bad_alloc &) {
  return Result<LinearSourceRef, Error>::failure(
      native_error("analysis_roi_host_allocation_failed"));
}
Result<LinearSourceRef, Error>
MacAnalysisPort::compose_report(const Input &input,
                                const ReportPlan &plan) try {
  using Out = Result<LinearSourceRef, Error>;
  @autoreleasepool {
    auto &r = *state_->gpu;
    if (r.failure)
      return Out::failure(*r.failure);
    if (!input.source || !valid_report_plan(plan))
      return Out::failure(analysis_error("invalid_report_plan"));
    auto src = macos_source_texture(input.source);
    if (!src)
      return Out::failure(analysis_error("unsupported_source_binding"));
    id<MTLTexture> work = src;
    if (plan.source_view.false_color) {
      auto cache = build_work(*state_, input, plan.source_view.settings);
      if (!cache)
        return Out::failure(cache.error());
      work = cache.value()->pixels;
    }
    auto output = texture(r, plan.underlay.size);
    auto under = [r.device newBufferWithBytes:plan.underlay.rgba.data()
                                       length:plan.underlay.rgba.size()
                                      options:MTLResourceStorageModeShared];
    auto over = [r.device newBufferWithBytes:plan.overlay.rgba.data()
                                      length:plan.overlay.rgba.size()
                                     options:MTLResourceStorageModeShared];
    auto command = [r.queue commandBuffer];
    if (!output || !under || !over || !command)
      return Out::failure(native_error("analysis_report_allocation_failed"));
    struct ReportParams {
      std::uint32_t width, height;
      std::int32_t x, y, w, h;
      float ui_white;
    };
    ReportParams rp{unsigned(plan.underlay.size.width),
                    unsigned(plan.underlay.size.height),
                    plan.source_rect.x,
                    plan.source_rect.y,
                    plan.source_rect.width,
                    plan.source_rect.height,
                    float(plan.ui_white_edr)};
    auto p = parameters(plan.source_view);
    auto e = [command computeCommandEncoder];
    if (!e)
      return Out::failure(native_error("report_encoder_failed"));
    [e setTexture:src atIndex:0];
    [e setTexture:work atIndex:1];
    [e setTexture:output atIndex:2];
    [e setBuffer:under offset:0 atIndex:0];
    [e setBuffer:over offset:0 atIndex:1];
    [e setBytes:&rp length:sizeof(rp) atIndex:2];
    [e setBytes:&p length:sizeof(p) atIndex:3];
    dispatch(e, r.pipelines.at("compose_report"), plan.underlay.size);
    [e endEncoding];
    return macos_wrap_linear_texture(output, command, {input.source});
  }
} catch (const std::bad_alloc &) {
  return Result<LinearSourceRef, Error>::failure(
      native_error("analysis_report_host_allocation_failed"));
}
Result<ResultRef, Error> MacAnalysisPort::analyze(const Input &input,
                                                  const Request &request) try {
  using Out = Result<ResultRef, Error>;
  @autoreleasepool {
    if (!input.source || !valid_request(request))
      return Out::failure(analysis_error("invalid_analysis_request"));
    auto &r = *state_->gpu;
    bool work_reused = false;
    {
      std::lock_guard lock(state_->mutex);
      work_reused = state_->work && state_->work->source == input.source &&
                    state_->work->settings == request.settings;
    }
    auto work = build_work(*state_, input, request.settings);
    if (!work)
      return Out::failure(work.error());
    auto result = std::make_shared<ResultData>();
    auto size = input.source->size_px();
    auto src = macos_source_texture(input.source);
    Request statistics_request = request;
    statistics_request.scopes.wave_width =
        std::min(request.scopes.wave_width, std::uint32_t(size.width));
    auto &o = statistics_request.scopes;
    const bool reuse =
        last_result_ && last_source_ == input.source && last_request_ &&
        same_statistics_request(*last_request_, statistics_request);
    const bool same_source =
        last_result_ && last_source_ == input.source && last_request_;
    const bool reuse_wave =
        same_source &&
        same_wave_statistics_request(*last_request_, statistics_request);
    const bool reuse_vector =
        same_source &&
        same_vector_statistics_request(*last_request_, statistics_request);
    const bool reuse_histogram =
        same_source &&
        same_histogram_statistics_request(*last_request_, statistics_request);
    const bool reuse_wave_detail =
        same_source &&
        same_wave_detail_request(*last_request_, statistics_request);
    const bool reuse_vector_detail =
        same_source &&
        same_vector_detail_request(*last_request_, statistics_request);
    const bool reuse_detail = reuse_wave_detail && reuse_vector_detail;
    auto statistics_start = std::chrono::steady_clock::now();
    std::size_t transient_bytes = 0;
    if (reuse) {
      *result = *last_result_;
      result->samples.clear();
      refresh_result_projection(*result, request);
      result->prepare_ms = 0;
      result->statistics_ms = 0;
      result->prepare_gpu_ms = 0;
      result->statistics_gpu_ms = 0;
      result->sampling_gpu_ms = 0;
    } else {
      refresh_result_projection(*result, request);
      if (reuse_wave)
        result->waveform = last_result_->waveform;
      if (reuse_vector)
        result->vectorscope = last_result_->vectorscope;
      if (reuse_histogram)
        result->histograms = last_result_->histograms;
      const auto base =
          vector_calibration(request.settings, o.vector_mode, 1, 1, 1);
      const auto bounds_offset = o.vector_mode == VectorMode::ycbcr ? 0u : 2u;
      result->vector_grid_x_extent = std::max(
          base.x_extent, double(work.value()->vector_bounds[bounds_offset]));
      result->vector_grid_y_extent =
          std::max(base.y_extent,
                   double(work.value()->vector_bounds[bounds_offset + 1]));
      const bool collect_wave = o.waveform_visible && !reuse_wave;
      const bool collect_vector = o.vector_visible && !reuse_vector;
      const bool collect_histogram = o.histogram_visible && !reuse_histogram;
      const auto wave_size = collect_wave
                                 ? std::size_t(o.wave_width) * o.wave_height
                                 : 0,
                 vector_size = collect_vector ? std::size_t(o.vector_width) *
                                                    o.vector_height
                                              : 0,
                 histogram_size =
                     collect_histogram ? std::size_t(o.histogram_bins) : 0;
      const auto count_elements = std::max(
          std::size_t{1}, wave_size * 4 + vector_size + histogram_size * 4);
      const auto color_elements = std::max(
          std::size_t{1}, (wave_size + vector_size + histogram_size) * 6);
      const auto groups =
          (std::size_t(size.width) * std::size_t(size.height) + 255) / 256;
      struct Partial {
        std::array<float, 4> source, work;
        std::array<std::uint32_t, 4> counts;
        std::array<float, 4> x;
      };
      static_assert(sizeof(Partial) == 64);
      auto counts = [r.device newBufferWithLength:count_elements * 4
                                          options:MTLResourceStorageModeShared];
      auto colors = [r.device newBufferWithLength:color_elements * 4
                                          options:MTLResourceStorageModeShared];
      auto partials =
          [r.device newBufferWithLength:groups * sizeof(Partial)
                                options:MTLResourceStorageModeShared];
      if (!counts || !colors || !partials)
        return Out::failure(
            native_error("statistics_buffer_allocation_failed"));
      transient_bytes =
          count_elements * 4 + color_elements * 4 + groups * sizeof(Partial);
      std::memset(counts.contents, 0, count_elements * 4);
      std::memset(colors.contents, 0, color_elements * 4);
      struct StatsParams {
        std::uint32_t width, height, space, mask_kind;
        float white, mx, my, mw, mh;
        std::uint32_t ww, wh, vw, vh, bins, wave_flags, hist_flags, hist_mode,
            vector_mode, vector_enabled;
        float az, ap, vx, vy, cx, cy;
      };
      std::uint32_t wave_flags = 0;
      if (o.waveform_visible && !reuse_wave) {
        if (o.wave_mode == WaveMode::intensity)
          wave_flags = 1;
        else if (o.wave_mode == WaveMode::parade_intensity_rgb)
          wave_flags = 15;
        else
          wave_flags = 14;
      }
      std::uint32_t hist_flags =
          o.histogram_visible && !reuse_histogram
              ? ((o.histogram_mode == HistogramMode::intensity ||
                  o.histogram_mode == HistogramMode::hue)
                     ? 1u
                     : 14u)
              : 0u;
      StatsParams p{unsigned(size.width),
                    unsigned(size.height),
                    unsigned(request.settings.working_space),
                    request.mask.enabled
                        ? (request.mask.shape == MaskShape::rectangle ? 1u : 2u)
                        : 0u,
                    float(request.settings.reference_white_nits),
                    float(request.mask.bounds.x),
                    float(request.mask.bounds.y),
                    float(request.mask.bounds.width),
                    float(request.mask.bounds.height),
                    collect_wave ? o.wave_width : 0u,
                    collect_wave ? o.wave_height : 0u,
                    collect_vector ? o.vector_width : 0u,
                    collect_vector ? o.vector_height : 0u,
                    collect_histogram ? o.histogram_bins : 0u,
                    wave_flags,
                    hist_flags,
                    unsigned(o.histogram_mode),
                    unsigned(o.vector_mode),
                    o.vector_visible && !reuse_vector ? 1u : 0u,
                    1.f,
                    0.f,
                    float(1 / (2 * result->vector_grid_x_extent)),
                    float(1 / (2 * result->vector_grid_y_extent)), 0.f, 0.f};
      auto command = [r.queue commandBuffer];
      auto e = [command computeCommandEncoder];
      if (!command || !e)
        return Out::failure(native_error("statistics_command_failed"));
      [e setTexture:src atIndex:0];
      [e setTexture:work.value()->pixels atIndex:1];
      [e setBuffer:counts offset:0 atIndex:0];
      [e setBuffer:colors offset:0 atIndex:1];
      [e setBuffer:partials offset:0 atIndex:2];
      [e setBytes:&p length:sizeof(p) atIndex:3];
      auto pipeline = r.pipelines.at("statistics");
      if (pipeline.maxTotalThreadsPerThreadgroup < 256)
        return Out::failure(native_error("statistics_threadgroup_unsupported"));
      [e setComputePipelineState:pipeline];
      [e dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
      [e endEncoding];
      auto done = wait_command(command);
      if (!done)
        return Out::failure(done.error());
      result->statistics_gpu_ms =
          (command.GPUEndTime - command.GPUStartTime) * 1000.0;
      const auto *integers =
          static_cast<const std::uint32_t *>(counts.contents);
      const auto *color_values =
          static_cast<const std::uint32_t *>(colors.contents);
      auto color_sum = [&](std::size_t cell, std::size_t channel) {
        const auto offset = (cell * 3 + channel) * 2;
        const std::uint64_t value =
            std::uint64_t(color_values[offset]) +
            (std::uint64_t(color_values[offset + 1]) << 32);
        return float(double(value) / 65535.0);
      };
      auto fill_grid = [&](DensityGrid &g, std::uint32_t width,
                           std::uint32_t height, std::size_t count_offset,
                           std::optional<std::size_t> color_offset) {
        g.width = width;
        g.height = height;
        std::size_t n = std::size_t(width) * height;
        g.counts.assign(integers + count_offset, integers + count_offset + n);
        g.maximum = *std::max_element(g.counts.begin(), g.counts.end());
        if (color_offset) {
          g.color_sums.resize(n);
          for (std::size_t i = 0; i < n; ++i)
            for (std::size_t c = 0; c < 3; ++c)
              g.color_sums[i][c] = color_sum(*color_offset + i, c);
        }
      };
      for (std::size_t c = 0; c < 4; ++c)
        if (wave_flags & (1u << c)) {
          fill_grid(result->waveform[c], o.wave_width, o.wave_height,
                    c * wave_size,
                    c == 0 ? std::optional<std::size_t>{0} : std::nullopt);
          set_waveform_column_coverage(result->waveform[c],
                                       unsigned(size.width));
        }
      if (o.vector_visible && !reuse_vector)
        fill_grid(result->vectorscope, o.vector_width, o.vector_height,
                  4 * wave_size, wave_size);
      for (std::size_t c = 0; c < 4; ++c)
        if (hist_flags & (1u << c)) {
          auto &h = result->histograms[c];
          h.counts.resize(o.histogram_bins);
          for (std::size_t b = 0; b < o.histogram_bins; ++b)
            h.counts[b] = integers[4 * wave_size + vector_size +
                                   c * o.histogram_bins + b];
          h.maximum = *std::max_element(h.counts.begin(), h.counts.end());
          if (c == 0) {
            h.color_sums.resize(o.histogram_bins);
            for (std::size_t i = 0; i < o.histogram_bins; ++i)
              for (std::size_t c = 0; c < 3; ++c)
                h.color_sums[i][c] = color_sum(wave_size + vector_size + i, c);
          }
        }
      Rgb source_mean{}, work_mean{};
      double source_x = 0;
      auto *reduced = static_cast<const Partial *>(partials.contents);
      for (std::size_t i = 0; i < groups; ++i) {
        result->valid_count += reduced[i].counts[0];
        result->invalid_count += reduced[i].counts[1];
        result->hue_count += reduced[i].counts[2];
        source_x += reduced[i].x[0];
        for (std::size_t c = 0; c < 3; ++c) {
          source_mean[c] += reduced[i].source[c];
          work_mean[c] += reduced[i].work[c];
        }
      }
      if (result->valid_count) {
        for (std::size_t c = 0; c < 3; ++c) {
          source_mean[c] /= double(result->valid_count);
          work_mean[c] /= double(result->valid_count);
        }
        source_x /= double(result->valid_count);
      }
      result->mask_mean =
          make_readout(source_mean, work_mean, result->valid_count,
                       result->invalid_count, request.settings);
      if (result->valid_count)
        result->mask_mean_position =
            project_sample(work_mean, source_x, request.settings, o);
      result->prepare_ms = work_reused ? 0 : work.value()->prepare_ms;
      result->prepare_gpu_ms = work_reused ? 0 : work.value()->prepare_gpu_ms;
      result->statistics_ms =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - statistics_start)
              .count();
    }
    if (reuse_wave_detail) {
      result->waveform_detail = last_result_->waveform_detail;
      result->waveform_detail_view = last_result_->waveform_detail_view;
    }
    if (reuse_vector_detail) {
      result->vectorscope_detail = last_result_->vectorscope_detail;
      result->vector_detail_x_extent = last_result_->vector_detail_x_extent;
      result->vector_detail_y_extent = last_result_->vector_detail_y_extent;
      result->vector_detail_zoom = last_result_->vector_detail_zoom;
      result->vector_detail_center = last_result_->vector_detail_center;
    }
    result->detail_ms = result->detail_gpu_ms = 0;
    if (!reuse_detail) {
      const auto detail_start = std::chrono::steady_clock::now();
      const bool wave =
          !reuse_wave_detail && o.waveform_visible && o.wave_detail_width;
      const bool vector =
          !reuse_vector_detail && o.vector_visible && o.vector_detail_width;
      if (wave || vector) {
        work = ensure_projected(
            *state_, work.value(),
            wave || (vector && o.vector_mode == VectorMode::ycbcr),
            vector && o.vector_mode == VectorMode::perceptual);
        if (!work)
          return Out::failure(work.error());
      }
      auto detail = refine_gpu_scopes(*state_, *work.value(), request, *result,
                                      !reuse_wave_detail, !reuse_vector_detail);
      if (!detail)
        return Out::failure(detail.error());
      transient_bytes = std::max(transient_bytes, detail.value());
      if (wave || vector)
        result->detail_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - detail_start)
                                .count();
    }
    auto sample_start = std::chrono::steady_clock::now();
    for (const auto &sample : request.samples) {
      bool copied = false;
      if (reuse && last_result_)
        for (const auto &old : last_result_->samples)
          if (old.request.id == sample.id && old.request.x == sample.x &&
              old.request.y == sample.y && old.request.side == sample.side &&
              old.request.respect_mask == sample.respect_mask) {
            result->samples.push_back(old);
            copied = true;
            break;
          }
      if (copied)
        continue;
      auto bounds = sample_bounds(size, sample);
      std::size_t n = std::size_t(bounds.width * bounds.height);
      if (!n) {
        SampleResult empty;
        empty.request = sample;
        empty.clipped_bounds = bounds;
        result->samples.push_back(std::move(empty));
        continue;
      }
      auto output = [r.device newBufferWithLength:n * 32
                                          options:MTLResourceStorageModeShared];
      auto command = [r.queue commandBuffer];
      auto e = [command computeCommandEncoder];
      if (!output || !command || !e)
        return Out::failure(native_error("sample_buffer_allocation_failed"));
      std::array<std::uint32_t, 4> rect{unsigned(bounds.x), unsigned(bounds.y),
                                        unsigned(bounds.width),
                                        unsigned(bounds.height)};
      [e setTexture:src atIndex:0];
      [e setTexture:work.value()->pixels atIndex:1];
      [e setBuffer:output offset:0 atIndex:0];
      [e setBytes:rect.data() length:sizeof(rect) atIndex:1];
      dispatch(e, r.pipelines.at("sample_region"),
               {int(bounds.width), int(bounds.height)});
      [e endEncoding];
      auto done = wait_command(command);
      if (!done)
        return Out::failure(done.error());
      result->sampling_gpu_ms +=
          (command.GPUEndTime - command.GPUStartTime) * 1000.0;
      auto *pixels = static_cast<const std::array<float, 4> *>(output.contents);
      FloatImage ss(n), ww(n);
      for (std::size_t i = 0; i < n; ++i) {
        ss[i] = pixels[i * 2];
        ww[i] = pixels[i * 2 + 1];
      }
      result->samples.push_back(
          summarize_sample(size, sample, request, bounds, ss, ww));
      transient_bytes = std::max(transient_bytes, n * 64);
    }
    result->sampling_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - sample_start)
                              .count();
    result->retained_bytes = input.source->byte_count() * 2 +
                             work.value()->projected_bytes +
                             result_bytes(*result);
    const auto prior_result_bytes =
        last_result_ ? additional_result_bytes(*last_result_, *result) : 0;
    // Explicit working-set estimate: old UI result survives until replacement.
    // Driver allocation granularity/drawables and external result owners are
    // not part of this per-session CPU/GPU payload accounting.
    result->peak_bytes = std::max(
        work_reused ? std::size_t{} : work.value()->peak + prior_result_bytes,
        result->retained_bytes + transient_bytes + prior_result_bytes);
    last_source_ = input.source;
    last_request_ = statistics_request;
    last_result_ = result;
    return Out::success(std::move(result));
  }
} catch (const std::bad_alloc &) {
  return Result<ResultRef, Error>::failure(
      native_error("analysis_statistics_host_allocation_failed"));
}
} // namespace
MacAnalysisPresentationStatus
macos_analysis_presentation_status(const AnalysisPresenterPort &presenter) {
  const auto *native = dynamic_cast<const MacAnalysisPresenter *>(&presenter);
  return native ? native->status() : MacAnalysisPresentationStatus{};
}
void set_macos_analysis_presenter_error_callback(
    AnalysisPresenterPort &presenter, std::function<void(Error)> callback) {
  if (auto *native = dynamic_cast<MacAnalysisPresenter *>(&presenter))
    native->set_error_callback(std::move(callback));
}
Result<MacAnalysisBackend, Error> make_macos_analysis_backend() try {
  auto state = std::make_shared<Shared>();
  if (state->gpu->failure)
    return Result<MacAnalysisBackend, Error>::failure(*state->gpu->failure);
  return Result<MacAnalysisBackend, Error>::success(
      {std::make_shared<MacAnalysisPort>(state),
       std::make_shared<MacAnalysisPresenter>(state)});
} catch (const std::bad_alloc &) {
  return Result<MacAnalysisBackend, Error>::failure(
      native_error("analysis_session_allocation_failed"));
}
Result<LinearSourceRef, Error>
macos_analysis_render_offscreen(const analysis::Input &input,
                                const analysis::SourceView &view) try {
  Shared state;
  return render_source(state, input, view, true);
} catch (const std::bad_alloc &) {
  return Result<LinearSourceRef, Error>::failure(
      native_error("analysis_offscreen_allocation_failed"));
}
} // namespace hdrshot
