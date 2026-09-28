#include "platform/windows/windows_analysis_backend.hpp"
#include "domain/analysis/engine.hpp"
#include "domain/annotation/annotation_compositing.hpp"
#include "platform/windows/windows_analysis_shader_math.hpp"
#include "platform/windows/windows_d3d.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace hdrshot {
namespace {
using namespace analysis;
using Clock = std::chrono::steady_clock;

Error native_error(const char *reason, HRESULT hr = E_FAIL) {
  return windows_d3d_error("WindowsAnalysis", reason, hr);
}

// All buffers are indexed by source pixel, never by viewport sampling density.
// FP32 S and W remain textures; only aggregate grids and 256-pixel partials
// cross the readback boundary.
constexpr const char *kCompute = R"HLSL(
Texture2D<float4> source_pixels : register(t0);
Texture2D<float4> work_pixels : register(t1);
Texture2D<uint4> underlay_pixels : register(t2);
Texture2D<uint4> overlay_pixels : register(t3);
Texture2D<float4> projected_wave_pixels : register(t4);
Texture2D<float2> projected_vector_pixels : register(t5);
RWTexture2D<float4> output_pixels : register(u0);
RWStructuredBuffer<uint> count_output : register(u1);
RWStructuredBuffer<uint> color_output : register(u2);
struct Partial {float4 source_sum; float4 work_sum; uint4 counts; float4 x_sum;};
RWStructuredBuffer<Partial> partial_output : register(u3);
RWStructuredBuffer<float4> sample_output : register(u4);
RWStructuredBuffer<float4> bounds_output : register(u5);
RWTexture2D<float4> projected_wave_output : register(u6);
RWTexture2D<float2> projected_vector_output : register(u7);
cbuffer Params : register(b0) {
 uint width,height,space,mask_kind;
 float white,mx,my,mw,mh;
 uint wave_width,wave_height,vector_width,vector_height;
 uint bins,wave_flags,hist_flags,hist_mode;
 uint vector_mode,vector_enabled,mode,group_columns;
 float amplitude_zoom,amplitude_pan,vector_scale_x,vector_scale_y;
 float vector_center_x,vector_center_y,blur_radius;uint group_count;
 uint sample_x,sample_y,sample_width,sample_height;
 float4 weight_vectors[13];
};
cbuffer ReportParams : register(b1) {
 uint report_width,report_height,report_space,report_false_color;
 float report_scale,report_offset_x,report_offset_y,report_white;
 uint report_mask_kind;float report_mask_x,report_mask_y,report_mask_width;
 float report_mask_height,report_mask_dim,report_ui_white,report_pad;
 int report_source_x,report_source_y,report_source_width,report_source_height;
};
float weight_at(int i){float4 v=weight_vectors[i/4];return v[i%4];}
float3 rgb3(A3 v) { return float3(v.x,v.y,v.z); }
A3 a3v(float3 v) { return a3(v.x,v.y,v.z); }
uint2 xy_of(uint i) {return uint2(i%width,i/width);}
void add_color(uint cell,float3 color) {
 [unroll] for(uint c=0;c<3;c++) {
   uint value=(uint)floor(saturate(color[c])*65535.0f+0.5f);
   uint offset=(cell*3+c)*2;
   uint old;
   InterlockedAdd(color_output[offset],value,old);
   if(old>0xffffffffu-value) InterlockedAdd(color_output[offset+1],1u);
 }
}
[numthreads(8,8,1)]
void horizontal(uint3 q:SV_DispatchThreadID) {
 if(q.x>=width||q.y>=height)return;
 float4 sum=0; int radius=(int)blur_radius;
 [loop] for(int d=-radius;d<=radius;d++) {
   int x=(int)q.x+d;if(x<0||x>=(int)width)continue;
   float3 v=source_pixels.Load(int3(x,q.y,0)).rgb;
   if(!finite3(a3v(v)))continue;
   float weight=weight_at(d+radius);sum+=float4(v,1)*weight;
 }
 output_pixels[q.xy]=sum;
}
[numthreads(8,8,1)]
void build_work(uint3 q:SV_DispatchThreadID) {
 if(q.x>=width||q.y>=height)return;
 float3 src=source_pixels.Load(int3(q.xy,0)).rgb;
 if(!finite3(a3v(src))) {output_pixels[q.xy]=0;return;}
 if(blur_radius>0) {
   float4 sum=0;int radius=(int)blur_radius;
   [loop] for(int d=-radius;d<=radius;d++) {
     int y=(int)q.y+d;if(y<0||y>=(int)height)continue;
     sum+=work_pixels.Load(int3(q.x,y,0))*weight_at(d+radius);
   }
   if(!(sum.a>0)){output_pixels[q.xy]=0;return;}
   src=sum.rgb/sum.a;
 }
 A3 value=working_rgb(a3v(src),space,white);
 output_pixels[q.xy]=float4(rgb3(value),finite3(value)?1.0f:0.0f);
}
[numthreads(8,8,1)]
void project_signals(uint3 q:SV_DispatchThreadID) {
 if(q.x>=width||q.y>=height)return;
 float4 w=work_pixels.Load(int3(q.xy,0));
 Signals s=signals_from_work(a3v(w.rgb),space,white);
 if(mode&1u)projected_wave_output[q.xy]=float4(rgb3(s.encoded),s.intensity);
 if(mode&2u)projected_vector_output[q.xy]=float2(s.perceptual.y,s.perceptual.z);
}
groupshared float4 group_source[256];
groupshared float4 group_work[256];
groupshared uint4 group_counts[256];
groupshared float group_x[256];
[numthreads(256,1,1)]
void statistics(uint3 id:SV_DispatchThreadID,uint lane:SV_GroupIndex,uint3 group:SV_GroupID) {
 uint group_index=group.y*group_columns+group.x;
 uint i=group_index*256+lane;float4 ss=0,ww=0;uint4 ns=0;float xx=0;
 if(i<width*height) {
   uint2 q=xy_of(i);
   if(mask_contains(mask_kind,float(q.x)+.5f,float(q.y)+.5f,mx,my,mw,mh)) {
     float4 s=0;
     if(mode==0)s=source_pixels.Load(int3(q,0));
     float4 w=work_pixels.Load(int3(q,0));
     Signals signal=(Signals)0;
     if(mode==0)signal=signals_from_work(a3v(w.rgb),space,white);
     if((mode==0&&!finite3(a3v(s.rgb)))||w.a==0||(mode==0&&!signal.valid)) ns.y=1;
     else {
       ss=float4(s.rgb,0);ww=float4(w.rgb,0);ns.x=1;
       ns.z=mode==0&&signal.hue_valid?1u:0u;xx=(float(q.x)+.5f)/float(width);
       float3 color=rgb3(display_srgb_from_work(a3v(w.rgb),space));
       uint wave_size=wave_width*wave_height,vector_size=vector_width*vector_height;
       float4 cached=(mode==1&&(wave_flags!=0||(vector_enabled&&vector_mode==0)))?
           projected_wave_pixels.Load(int3(q,0)):float4(0,0,0,0);
       float4 values=mode==1?float4(cached.a,cached.rgb):float4(signal.intensity,rgb3(signal.encoded));
       [unroll] for(uint c=0;c<4;c++) if(wave_flags&(1u<<c)) {
         float value=mode==1?(values[c]-amplitude_pan)*amplitude_zoom:values[c];
         if(mode==0||(value>=0&&value<=1)) {
           uint k=signal_bin(value,wave_height)*wave_width+source_column_bin(q.x,width,wave_width);
           InterlockedAdd(count_output[c*wave_size+k],1u);
           if(c==0)add_color(k,color);
         }
       }
       if(vector_enabled) {
         float2 plane;
         if(mode==1) {
           if(vector_mode==0){A3 ncl=ncl_from_signal(a3v(cached.rgb),working_gamut(space));plane=float2(ncl.y,ncl.z);}
           else plane=projected_vector_pixels.Load(int3(q,0));
         }else plane=vector_mode==0?float2(signal.ncl.y,signal.ncl.z):float2(signal.perceptual.y,signal.perceptual.z);
         float2 v=.5f+(plane-float2(vector_center_x,vector_center_y))*float2(vector_scale_x,vector_scale_y);
         if(mode==0||all(v>=0)&&all(v<=1)) {
           uint k=signal_bin(v.y,vector_height)*vector_width+signal_bin(v.x,vector_width);
           InterlockedAdd(count_output[4*wave_size+k],1u);
           add_color(wave_size+k,color);
         }
       }
       if(hist_mode!=5||signal.hue_valid) {
         float3 h=hist_mode==0?float3(signal.intensity,signal.intensity,signal.intensity):(hist_mode==5?float3(signal.hue/360.0f,0,0):
           ((hist_mode==2||hist_mode==4)?rgb3(signal.adobe):rgb3(signal.encoded)));
         [unroll] for(uint c=0;c<4;c++) if(hist_flags&(1u<<c)) {
           uint b=signal_bin(h[c==0?0:c-1],bins);
           InterlockedAdd(count_output[4*wave_size+vector_size+c*bins+b],1u);
           if(c==0)add_color(wave_size+vector_size+b,color);
         }
       }
     }
   }
 }
 // Fine grids replace the corresponding base region; their numerical readouts
 // continue to use the retained full-statistics result. No partial reduction.
 if(mode==1)return;
 group_source[lane]=ss;group_work[lane]=ww;group_counts[lane]=ns;group_x[lane]=xx;
 GroupMemoryBarrierWithGroupSync();
 [unroll] for(uint stride=128;stride>0;stride/=2) {
   if(lane<stride) {group_source[lane]+=group_source[lane+stride];group_work[lane]+=group_work[lane+stride];
     group_counts[lane]+=group_counts[lane+stride];group_x[lane]+=group_x[lane+stride];}
   GroupMemoryBarrierWithGroupSync();
 }
 if(lane==0&&group_index<group_count) {Partial p;p.source_sum=group_source[0];p.work_sum=group_work[0];
   p.counts=group_counts[0];p.x_sum=float4(group_x[0],0,0,0);partial_output[group_index]=p;}
}
[numthreads(256,1,1)]
void vector_bounds(uint3 id:SV_DispatchThreadID,uint lane:SV_GroupIndex,uint3 group:SV_GroupID) {
 uint group_index=group.y*group_columns+group.x;
 uint i=group_index*256+lane;
 float4 v=0;if(i<width*height){float4 w=work_pixels.Load(int3(xy_of(i),0));
   if(w.a>0){Signals s=signals_from_work(a3v(w.rgb),space,white);
     if(s.valid)v=abs(float4(s.ncl.y,s.ncl.z,s.perceptual.y,s.perceptual.z));}}
 group_source[lane]=v;GroupMemoryBarrierWithGroupSync();
 [unroll] for(uint stride=128;stride>0;stride/=2) {if(lane<stride)group_source[lane]=max(group_source[lane],group_source[lane+stride]);GroupMemoryBarrierWithGroupSync();}
 if(lane==0&&group_index<group_count)bounds_output[group_index]=group_source[0];
}
[numthreads(8,8,1)]
void sample_region(uint3 q:SV_DispatchThreadID) {
 if(q.x>=sample_width||q.y>=sample_height)return;
 uint i=q.y*sample_width+q.x;uint2 p=q.xy+uint2(sample_x,sample_y);
 sample_output[i*2]=source_pixels.Load(int3(p,0));
 sample_output[i*2+1]=work_pixels.Load(int3(p,0));
}
float3 source_pixel(int2 q) {
 q=clamp(q,int2(0,0),int2(width-1,height-1));
 A3 v=a3v(source_pixels.Load(int3(q,0)).rgb);
 if(!finite3(v))return float3(.35f,0,.35f);
 if(report_space<2)v=clip3(v,1.0f);
 return rgb3(v);
}
float3 report_source(float2 target) {
 float2 pos=(target-float2(report_offset_x,report_offset_y))/report_scale;
 if(any(pos<0)||pos.x>=width||pos.y>=height)return float3(.004f,.006f,.008f);
 float3 color;
 if(report_false_color) {
   float4 w=work_pixels.Load(int3(uint2(pos),0));
   color=w.a>0?rgb3(false_color_rgb(dot3(y_coefficients(working_gamut(report_space)),a3v(w.rgb)))):float3(.35f,0,.35f);
 }else {
   float2 f=pos-.5f;int2 low=int2(floor(f));float2 t=f-float2(low);
   color=lerp(lerp(source_pixel(low),source_pixel(low+int2(1,0)),t.x),
     lerp(source_pixel(low+int2(0,1)),source_pixel(low+int2(1,1)),t.x),t.y);
 }
 if(!mask_contains(report_mask_kind,pos.x,pos.y,report_mask_x,report_mask_y,report_mask_width,report_mask_height))
   color*=report_mask_dim;
 return color;
}
float4 ui_pixel(uint4 pixel) {
 A3 color=ui_rgb_to_linear_p3((pixel.r<<16)|(pixel.g<<8)|pixel.b);
 return float4(rgb3(color)*report_ui_white,float(pixel.a)/255.0f);
}
[numthreads(8,8,1)]
void render_report(uint3 q:SV_DispatchThreadID) {
 if(q.x>=report_width||q.y>=report_height)return;
 float4 base=ui_pixel(underlay_pixels.Load(int3(q.xy,0)));
 float3 color=base.rgb*base.a;
 int2 local=int2(q.xy)-int2(report_source_x,report_source_y);
 if(all(local>=0)&&local.x<report_source_width&&local.y<report_source_height)
   color=report_source(float2(local)+.5f);
 float4 top=ui_pixel(overlay_pixels.Load(int3(q.xy,0)));
 output_pixels[q.xy]=float4(lerp(color,top.rgb,top.a),1);
}
)HLSL";

struct alignas(16) Params {
  std::uint32_t width{}, height{}, space{}, mask_kind{};
  float white{}, mx{}, my{}, mw{}, mh{};
  std::uint32_t wave_width{}, wave_height{}, vector_width{}, vector_height{};
  std::uint32_t bins{}, wave_flags{}, hist_flags{}, hist_mode{};
  std::uint32_t vector_mode{}, vector_enabled{}, mode{}, group_columns{};
  float amplitude_zoom{}, amplitude_pan{}, vector_scale_x{}, vector_scale_y{};
  float vector_center_x{}, vector_center_y{}, blur_radius{};
  std::uint32_t group_count{};
  std::uint32_t sample_x{}, sample_y{}, sample_width{}, sample_height{};
  float before_weights[3]{};
  float weights[52]{};
};
static_assert(sizeof(Params) % 16 == 0);
struct Partial {
  std::array<float, 4> source_sum, work_sum;
  std::array<std::uint32_t, 4> counts;
  std::array<float, 4> x_sum;
};
static_assert(sizeof(Partial) == 64);
struct ReportParams {
  std::uint32_t width, height, space, false_color;
  float scale, offset_x, offset_y, white;
  std::uint32_t mask_kind;
  float mask_x, mask_y, mask_width;
  float mask_height, mask_dim, ui_white, pad;
  std::int32_t source_x, source_y, source_width, source_height;
};

struct Runtime {
  WindowsD3DDevice gpu;
  std::string shader = windows_analysis_math_hlsl() + kCompute;
  std::mutex mutex;
  std::map<std::string, winrt::com_ptr<ID3D11ComputeShader>> kernels;
  Runtime() {
    for (const char *entry :
         {"horizontal", "build_work", "project_signals", "statistics",
          "vector_bounds", "sample_region", "render_report"}) {
      auto bytecode = gpu.compile(shader.c_str(), entry, "cs_5_0");
      winrt::com_ptr<ID3D11ComputeShader> kernel;
      winrt::check_hresult(gpu.device->CreateComputeShader(
          bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr,
          kernel.put()));
      kernels.emplace(entry, std::move(kernel));
    }
  }
};

winrt::com_ptr<ID3D11Texture2D>
create_texture(Runtime &r, PixelSize size, const float *data = nullptr,
               DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_FLOAT) {
  D3D11_TEXTURE2D_DESC d{};
  d.Width = size.width;
  d.Height = size.height;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = format;
  d.SampleDesc.Count = 1;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  if (data) {
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  }
  D3D11_SUBRESOURCE_DATA init{data, UINT(size.width * 16), 0};
  winrt::com_ptr<ID3D11Texture2D> result;
  winrt::check_hresult(
      r.gpu.device->CreateTexture2D(&d, data ? &init : nullptr, result.put()));
  return result;
}
winrt::com_ptr<ID3D11UnorderedAccessView>
texture_uav(Runtime &r, ID3D11Texture2D *texture) {
  winrt::com_ptr<ID3D11UnorderedAccessView> view;
  winrt::check_hresult(
      r.gpu.device->CreateUnorderedAccessView(texture, nullptr, view.put()));
  return view;
}
winrt::com_ptr<ID3D11ShaderResourceView> texture_srv(Runtime &r,
                                                     ID3D11Texture2D *texture) {
  winrt::com_ptr<ID3D11ShaderResourceView> view;
  winrt::check_hresult(
      r.gpu.device->CreateShaderResourceView(texture, nullptr, view.put()));
  return view;
}
winrt::com_ptr<ID3D11Buffer> structured(Runtime &r, std::size_t n,
                                        std::size_t stride,
                                        bool initial_zero = true) {
  D3D11_BUFFER_DESC d{};
  d.ByteWidth = UINT(std::max<std::size_t>(1, n) * stride);
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
  d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  d.StructureByteStride = UINT(stride);
  winrt::com_ptr<ID3D11Buffer> b;
  winrt::check_hresult(r.gpu.device->CreateBuffer(&d, nullptr, b.put()));
  if (initial_zero) {
    auto uav = r.gpu.uav(b);
    UINT zeros[4]{};
    r.gpu.context->ClearUnorderedAccessViewUint(uav.get(), zeros);
  }
  return b;
}
template <class T>
std::vector<T> read_buffer(Runtime &r, ID3D11Buffer *input, std::size_t n) {
  D3D11_BUFFER_DESC d{};
  input->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.MiscFlags = 0;
  d.StructureByteStride = 0;
  winrt::com_ptr<ID3D11Buffer> staging;
  winrt::check_hresult(r.gpu.device->CreateBuffer(&d, nullptr, staging.put()));
  r.gpu.context->CopyResource(staging.get(), input);
  D3D11_MAPPED_SUBRESOURCE m{};
  winrt::check_hresult(
      r.gpu.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m));
  std::vector<T> out(n);
  std::memcpy(out.data(), m.pData, n * sizeof(T));
  r.gpu.context->Unmap(staging.get(), 0);
  return out;
}
void dispatch(Runtime &r, const char *entry, Params params,
              ID3D11Texture2D *source, ID3D11Texture2D *work,
              ID3D11Texture2D *output, ID3D11Buffer *counts = nullptr,
              ID3D11Buffer *colors = nullptr, ID3D11Buffer *partials = nullptr,
              ID3D11Buffer *samples = nullptr, ID3D11Buffer *bounds = nullptr,
              UINT x = 0, UINT y = 1, ID3D11Texture2D *projected_wave = nullptr,
              ID3D11Texture2D *projected_vector = nullptr) {
  auto constants =
      r.gpu.buffer(sizeof(params), D3D11_BIND_CONSTANT_BUFFER, 0, &params);
  auto src = source ? texture_srv(r, source)
                    : winrt::com_ptr<ID3D11ShaderResourceView>{};
  auto wrk =
      work ? texture_srv(r, work) : winrt::com_ptr<ID3D11ShaderResourceView>{};
  auto out = output ? texture_uav(r, output)
                    : winrt::com_ptr<ID3D11UnorderedAccessView>{};
  auto buffer_uav = [&](ID3D11Buffer *b) {
    winrt::com_ptr<ID3D11UnorderedAccessView> v;
    if (b)
      winrt::check_hresult(
          r.gpu.device->CreateUnorderedAccessView(b, nullptr, v.put()));
    return v;
  };
  auto cu = buffer_uav(counts), co = buffer_uav(colors),
       pa = buffer_uav(partials), sa = buffer_uav(samples),
       bo = buffer_uav(bounds);
  auto pw = projected_wave ? texture_srv(r, projected_wave)
                           : winrt::com_ptr<ID3D11ShaderResourceView>{};
  auto pv = projected_vector ? texture_srv(r, projected_vector)
                             : winrt::com_ptr<ID3D11ShaderResourceView>{};
  ID3D11ShaderResourceView *srv[]{src.get(), wrk.get(), nullptr,
                                  nullptr,   pw.get(),  pv.get()};
  ID3D11UnorderedAccessView *uav[]{out.get(), cu.get(), co.get(),
                                   pa.get(),  sa.get(), bo.get()};
  ID3D11Buffer *cb[]{constants.get()};
  auto *c = r.gpu.context.get();
  c->CSSetShader(r.kernels.at(entry).get(), nullptr, 0);
  c->CSSetConstantBuffers(0, 1, cb);
  c->CSSetShaderResources(0, 6, srv);
  c->CSSetUnorderedAccessViews(0, 6, uav, nullptr);
  c->Dispatch(x ? x : (params.width + 7) / 8, y, 1);
  ID3D11ShaderResourceView *null_srv[6]{};
  ID3D11UnorderedAccessView *null_uav[6]{};
  c->CSSetShaderResources(0, 6, null_srv);
  c->CSSetUnorderedAccessViews(0, 6, null_uav, nullptr);
  c->Flush();
}

class GpuSource final : public LinearSource {
public:
  GpuSource(std::shared_ptr<Runtime> runtime, PixelSize size,
            winrt::com_ptr<ID3D11Texture2D> texture)
      : runtime_(std::move(runtime)), size_(size),
        texture_(std::move(texture)) {}
  PixelSize size_px() const override { return size_; }
  std::size_t byte_count() const override {
    return std::size_t(size_.width) * size_.height * 16;
  }
  Result<bool, Error> wait_until_ready() const override {
    return Result<bool, Error>::success(true);
  }
  Result<LinearFloatPixels, Error> read_region(PixelRect rect) const override {
    using Out = Result<LinearFloatPixels, Error>;
    if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0 ||
        std::int64_t(rect.x) + rect.width > size_.width ||
        std::int64_t(rect.y) + rect.height > size_.height)
      return Out::failure(analysis_error("source_region_out_of_bounds"));
    try {
      std::lock_guard lock(runtime_->mutex);
      D3D11_TEXTURE2D_DESC d{};
      texture_->GetDesc(&d);
      d.Usage = D3D11_USAGE_STAGING;
      d.BindFlags = 0;
      d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      d.Width = UINT(rect.width);
      d.Height = UINT(rect.height);
      winrt::com_ptr<ID3D11Texture2D> staging;
      winrt::check_hresult(
          runtime_->gpu.device->CreateTexture2D(&d, nullptr, staging.put()));
      D3D11_BOX box{UINT(rect.x),
                    UINT(rect.y),
                    0,
                    UINT(rect.x + rect.width),
                    UINT(rect.y + rect.height),
                    1};
      runtime_->gpu.context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0,
                                                   texture_.get(), 0, &box);
      D3D11_MAPPED_SUBRESOURCE mapped{};
      winrt::check_hresult(runtime_->gpu.context->Map(
          staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
      LinearFloatPixels output(std::size_t(rect.width) * rect.height * 4);
      for (int y = 0; y < rect.height; y++)
        std::memcpy(output.data() + std::size_t(y) * rect.width * 4,
                    static_cast<const std::byte *>(mapped.pData) +
                        std::size_t(y) * mapped.RowPitch,
                    std::size_t(rect.width) * 16);
      runtime_->gpu.context->Unmap(staging.get(), 0);
      return Out::success(std::move(output));
    } catch (const winrt::hresult_error &e) {
      return Out::failure(native_error("source_readback_failed", e.code()));
    }
  }
  Runtime *runtime() const { return runtime_.get(); }
  ID3D11Texture2D *texture() const { return texture_.get(); }

private:
  std::shared_ptr<Runtime> runtime_;
  PixelSize size_;
  winrt::com_ptr<ID3D11Texture2D> texture_;
};

struct WorkCache {
  LinearSourceRef source;
  Settings settings;
  winrt::com_ptr<ID3D11Texture2D> work;
  winrt::com_ptr<ID3D11Texture2D> projected_wave;
  winrt::com_ptr<ID3D11Texture2D> projected_vector;
  std::size_t projected_bytes{};
  std::array<float, 4> vector_bounds{};
  double prepare_ms{};
  std::size_t peak_bytes{};
};

class WindowsAnalysisPort final : public AnalysisPort {
public:
  explicit WindowsAnalysisPort(std::shared_ptr<Runtime> runtime)
      : runtime_(std::move(runtime)) {}
  Result<LinearSourceRef, Error>
  prepare(const SelectionRoiView &source,
          const AnnotationPixelPlan &annotations) override {
    using Out = Result<LinearSourceRef, Error>;
    if (!source.valid_storage() || annotations.output_size_px != source.size_px)
      return Out::failure(analysis_error("invalid_clean_analysis_input"));
    auto plan = materialize_annotation_plan(annotations);
    if (!plan)
      return Out::failure(plan.error());
    auto frame = FrameCropper::read_cpu_region(source);
    if (!frame)
      return Out::failure(frame.error());
    auto roi = FrameCropper::view(frame.value());
    FloatImage pixels(std::size_t(roi.size_px.width) * roi.size_px.height);
    for (int y = 0; y < roi.size_px.height; y++)
      for (int x = 0; x < roi.size_px.width; x++) {
        auto &p = pixels[std::size_t(y) * roi.size_px.width + x];
        p[3] = 1;
        for (std::size_t c = 0; c < 3; c++) {
          auto offset = roi.first_sample_offset +
                        std::size_t(y) * roi.row_stride_samples +
                        std::size_t(x) * 4 + c;
          if (!roi.rgba_float.empty())
            p[c] = roi.rgba_float[offset];
          else {
            auto v = roi.sample(offset);
            if (!v)
              return Out::failure(v.error());
            p[c] = ExtendedP3Mapper::source_linear(v.value(),
                                                   roi.encoding.transfer);
          }
        }
      }
    for (const auto &span : plan.value().annotation_owned_spans)
      for (int x = 0; x < span.length; x++) {
        auto i = std::size_t(span.y) * roi.size_px.width + span.x + x;
        auto ink = annotation_linear_sample(span, std::size_t(x));
        for (std::size_t c = 0; c < 3; c++)
          pixels[i][c] =
              ink[c] +
              (ink[3] > 0 ? ink[3] * std::max(0.f, pixels[i][c]) : 0.f);
      }
    try {
      std::lock_guard lock(runtime_->mutex);
      auto texture =
          create_texture(*runtime_, roi.size_px, pixels.front().data());
      return Out::success(std::make_shared<GpuSource>(runtime_, roi.size_px,
                                                      std::move(texture)));
    } catch (const winrt::hresult_error &e) {
      return Out::failure(native_error("clean_roi_upload_failed", e.code()));
    }
  }
  Result<ResultRef, Error> analyze(const Input &input,
                                   const Request &request) override;
  Result<LinearSourceRef, Error>
  compose_report(const Input &input, const ReportPlan &plan) override;

private:
  Result<winrt::com_ptr<ID3D11Texture2D>, Error>
  source_texture(const LinearSourceRef &source) {
    using Out = Result<winrt::com_ptr<ID3D11Texture2D>, Error>;
    if (auto gpu = std::dynamic_pointer_cast<const GpuSource>(source);
        gpu && gpu->runtime() == runtime_.get()) {
      winrt::com_ptr<ID3D11Texture2D> texture;
      texture.copy_from(gpu->texture());
      return Out::success(std::move(texture));
    }
    if (uploaded_source_ == source && uploaded_texture_)
      return Out::success(uploaded_texture_);
    auto size = source->size_px();
    auto data = source->read_region({0, 0, size.width, size.height});
    if (!data)
      return Out::failure(data.error());
    if (data.value().size() != std::size_t(size.width) * size.height * 4)
      return Out::failure(analysis_error("invalid_native_source_readback"));
    try {
      uploaded_texture_ = create_texture(*runtime_, size, data.value().data());
      uploaded_source_ = source;
      return Out::success(uploaded_texture_);
    } catch (const winrt::hresult_error &e) {
      return Out::failure(native_error("source_upload_failed", e.code()));
    }
  }
  Result<WorkCache, Error> ensure_work(const Input &input,
                                       const Settings &settings,
                                       ID3D11Texture2D *source);
  Result<WorkCache, Error> ensure_projected(WorkCache work, bool wave,
                                            bool vector);
  std::shared_ptr<Runtime> runtime_;
  LinearSourceRef uploaded_source_, last_source_;
  winrt::com_ptr<ID3D11Texture2D> uploaded_texture_;
  std::optional<WorkCache> work_;
  std::optional<Request> last_request_;
  ResultRef last_result_;
};

Result<WorkCache, Error>
WindowsAnalysisPort::ensure_work(const Input &input, const Settings &settings,
                                 ID3D11Texture2D *source) {
  using Out = Result<WorkCache, Error>;
  if (work_ && work_->source == input.source && work_->settings == settings)
    return Out::success(*work_);
  if (!valid_settings(settings))
    return Out::failure(analysis_error("invalid_native_analysis_input"));
  auto start = Clock::now();
  auto size = input.source->size_px();
  auto weights = gaussian_weights(settings.blur_sigma_px);
  Params p{};
  p.width = size.width;
  p.height = size.height;
  p.space = unsigned(settings.working_space);
  p.white = float(settings.reference_white_nits);
  p.blur_radius = float(weights.size() / 2);
  std::copy(weights.begin(), weights.end(), p.weights);
  try {
    auto target = create_texture(*runtime_, size);
    winrt::com_ptr<ID3D11Texture2D> horizontal;
    if (p.blur_radius > 0) {
      horizontal = create_texture(*runtime_, size);
      dispatch(*runtime_, "horizontal", p, source, nullptr, horizontal.get(),
               nullptr, nullptr, nullptr, nullptr, nullptr,
               (size.width + 7) / 8, (size.height + 7) / 8);
    }
    dispatch(*runtime_, "build_work", p, source,
             horizontal ? horizontal.get() : source, target.get(), nullptr,
             nullptr, nullptr, nullptr, nullptr, (size.width + 7) / 8,
             (size.height + 7) / 8);
    const auto groups = (std::size_t(size.width) * size.height + 255) / 256;
    auto bounds =
        structured(*runtime_, groups, sizeof(std::array<float, 4>), false);
    const auto rows = (groups + 65534) / 65535;
    const auto columns = (groups + rows - 1) / rows;
    p.group_columns = UINT(columns);
    p.group_count = UINT(groups);
    dispatch(*runtime_, "vector_bounds", p, nullptr, target.get(), nullptr,
             nullptr, nullptr, nullptr, nullptr, bounds.get(), UINT(columns),
             UINT((groups + columns - 1) / columns));
    WorkCache next;
    next.source = input.source;
    next.settings = settings;
    next.work = std::move(target);
    auto reduced =
        read_buffer<std::array<float, 4>>(*runtime_, bounds.get(), groups);
    for (auto &item : reduced)
      for (std::size_t c = 0; c < 4; c++)
        next.vector_bounds[c] = std::max(next.vector_bounds[c], item[c]);
    next.prepare_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    const auto uploaded_copy =
        uploaded_source_ == input.source ? input.source->byte_count() : 0;
    next.peak_bytes = input.source->byte_count() * (p.blur_radius > 0 ? 3 : 2) +
                      uploaded_copy + groups * 16;
    work_ = next;
    return Out::success(next);
  } catch (const winrt::hresult_error &e) {
    return Out::failure(native_error("work_gpu_failed", e.code()));
  }
}

Result<WorkCache, Error>
WindowsAnalysisPort::ensure_projected(WorkCache work, bool wave, bool vector) {
  using Out = Result<WorkCache, Error>;
  const bool add_wave = wave && !work.projected_wave;
  const bool add_vector = vector && !work.projected_vector;
  if (!add_wave && !add_vector)
    return Out::success(std::move(work));
  auto size = work.source->size_px();
  try {
    if (add_wave)
      work.projected_wave = create_texture(*runtime_, size);
    if (add_vector)
      work.projected_vector =
          create_texture(*runtime_, size, nullptr, DXGI_FORMAT_R32G32_FLOAT);
    Params p{};
    p.width = size.width;
    p.height = size.height;
    p.space = unsigned(work.settings.working_space);
    p.white = float(work.settings.reference_white_nits);
    p.mode = (add_wave ? 1u : 0u) | (add_vector ? 2u : 0u);
    auto constants =
        runtime_->gpu.buffer(sizeof(p), D3D11_BIND_CONSTANT_BUFFER, 0, &p);
    auto source = texture_srv(*runtime_, work.work.get());
    auto wave_uav = add_wave ? texture_uav(*runtime_, work.projected_wave.get())
                             : winrt::com_ptr<ID3D11UnorderedAccessView>{};
    auto vector_uav = add_vector
                          ? texture_uav(*runtime_, work.projected_vector.get())
                          : winrt::com_ptr<ID3D11UnorderedAccessView>{};
    ID3D11Buffer *cb[]{constants.get()};
    ID3D11ShaderResourceView *srv[]{source.get()};
    ID3D11UnorderedAccessView *uav[]{wave_uav.get(), vector_uav.get()};
    auto *c = runtime_->gpu.context.get();
    c->CSSetShader(runtime_->kernels.at("project_signals").get(), nullptr, 0);
    c->CSSetConstantBuffers(0, 1, cb);
    c->CSSetShaderResources(1, 1, srv);
    c->CSSetUnorderedAccessViews(6, 2, uav, nullptr);
    c->Dispatch((size.width + 7) / 8, (size.height + 7) / 8, 1);
    ID3D11ShaderResourceView *null_srv[1]{};
    ID3D11UnorderedAccessView *null_uav[2]{};
    c->CSSetShaderResources(1, 1, null_srv);
    c->CSSetUnorderedAccessViews(6, 2, null_uav, nullptr);
    c->Flush();
    work.projected_bytes =
        std::size_t(size.width) * size.height *
        ((work.projected_wave ? 16u : 0u) + (work.projected_vector ? 8u : 0u));
    work.peak_bytes = std::max(
        work.peak_bytes,
        work.source->byte_count() * 2 +
            (uploaded_source_ == work.source ? work.source->byte_count() : 0) +
            work.projected_bytes);
    work_ = work;
    return Out::success(std::move(work));
  } catch (const winrt::hresult_error &e) {
    return Out::failure(native_error("projected_signals_gpu_failed", e.code()));
  }
}

Result<ResultRef, Error> WindowsAnalysisPort::analyze(const Input &input,
                                                      const Request &request) {
  using Out = Result<ResultRef, Error>;
  if (!input.source || !valid_request(request))
    return Out::failure(analysis_error("invalid_analysis_request"));
  try {
    std::lock_guard gpu_lock(runtime_->mutex);
    auto source = source_texture(input.source);
    if (!source)
      return Out::failure(source.error());
    const bool work_reused = work_ && work_->source == input.source &&
                             work_->settings == request.settings;
    auto work = ensure_work(input, request.settings, source.value().get());
    if (!work)
      return Out::failure(work.error());
    auto size = input.source->size_px();
    Request identity = request;
    identity.scopes.wave_width =
        std::min(identity.scopes.wave_width, std::uint32_t(size.width));
    auto &o = identity.scopes;
    const bool same_source =
        last_result_ && last_source_ == input.source && last_request_;
    const bool reuse =
        same_source && same_statistics_request(*last_request_, identity);
    const bool reuse_wave =
        same_source && same_wave_statistics_request(*last_request_, identity);
    const bool reuse_vector =
        same_source && same_vector_statistics_request(*last_request_, identity);
    const bool reuse_histogram =
        same_source &&
        same_histogram_statistics_request(*last_request_, identity);
    const bool reuse_wave_detail =
        same_source && same_wave_detail_request(*last_request_, identity);
    const bool reuse_vector_detail =
        same_source && same_vector_detail_request(*last_request_, identity);
    auto result = std::make_shared<ResultData>();
    auto stats_start = Clock::now();
    std::size_t transient_bytes{};
    if (reuse) {
      *result = *last_result_;
      result->samples.clear();
      refresh_result_projection(*result, request);
      result->prepare_ms = result->statistics_ms = result->prepare_gpu_ms =
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
      auto base = vector_calibration(request.settings, o.vector_mode, 1, 1, 1);
      const unsigned axis = o.vector_mode == VectorMode::ycbcr ? 0u : 2u;
      result->vector_grid_x_extent =
          std::max(base.x_extent, double(work.value().vector_bounds[axis]));
      result->vector_grid_y_extent =
          std::max(base.y_extent, double(work.value().vector_bounds[axis + 1]));
      const bool do_wave = o.waveform_visible && !reuse_wave;
      const bool do_vector = o.vector_visible && !reuse_vector;
      const bool do_hist = o.histogram_visible && !reuse_histogram;
      const std::size_t wave_size =
          do_wave ? std::size_t(o.wave_width) * o.wave_height : 0;
      const std::size_t vector_size =
          do_vector ? std::size_t(o.vector_width) * o.vector_height : 0;
      const std::size_t hist_size = do_hist ? o.histogram_bins : 0;
      const std::size_t count_size =
          std::max<std::size_t>(1, wave_size * 4 + vector_size + hist_size * 4);
      const std::size_t color_size =
          std::max<std::size_t>(1, (wave_size + vector_size + hist_size) * 6);
      const std::size_t groups =
          (std::size_t(size.width) * size.height + 255) / 256;
      auto counts = structured(*runtime_, count_size, 4);
      auto colors = structured(*runtime_, color_size, 4);
      auto partials = structured(*runtime_, groups, sizeof(Partial), false);
      transient_bytes =
          count_size * 4 + color_size * 4 + groups * sizeof(Partial);
      Params p{};
      p.width = size.width;
      p.height = size.height;
      p.space = unsigned(request.settings.working_space);
      p.mask_kind = request.mask.enabled
                        ? (request.mask.shape == MaskShape::rectangle ? 1u : 2u)
                        : 0u;
      p.white = float(request.settings.reference_white_nits);
      p.mx = float(request.mask.bounds.x);
      p.my = float(request.mask.bounds.y);
      p.mw = float(request.mask.bounds.width);
      p.mh = float(request.mask.bounds.height);
      p.wave_width = do_wave ? o.wave_width : 0;
      p.wave_height = do_wave ? o.wave_height : 0;
      p.vector_width = do_vector ? o.vector_width : 0;
      p.vector_height = do_vector ? o.vector_height : 0;
      p.bins = do_hist ? o.histogram_bins : 0;
      p.wave_flags = !do_wave                                        ? 0u
                     : o.wave_mode == WaveMode::intensity            ? 1u
                     : o.wave_mode == WaveMode::parade_intensity_rgb ? 15u
                                                                     : 14u;
      p.hist_flags = !do_hist ? 0u
                     : (o.histogram_mode == HistogramMode::intensity ||
                        o.histogram_mode == HistogramMode::hue)
                         ? 1u
                         : 14u;
      p.hist_mode = unsigned(o.histogram_mode);
      p.vector_mode = unsigned(o.vector_mode);
      p.vector_enabled = do_vector ? 1u : 0u;
      p.vector_scale_x = float(1 / (2 * result->vector_grid_x_extent));
      p.vector_scale_y = float(1 / (2 * result->vector_grid_y_extent));
      const auto rows = (groups + 65534) / 65535;
      const auto columns = (groups + rows - 1) / rows;
      p.group_columns = UINT(columns);
      p.group_count = UINT(groups);
      dispatch(*runtime_, "statistics", p, source.value().get(),
               work.value().work.get(), nullptr, counts.get(), colors.get(),
               partials.get(), nullptr, nullptr, UINT(columns),
               UINT((groups + columns - 1) / columns));
      auto integers =
          read_buffer<std::uint32_t>(*runtime_, counts.get(), count_size);
      auto color_values =
          read_buffer<std::uint32_t>(*runtime_, colors.get(), color_size);
      auto reduced = read_buffer<Partial>(*runtime_, partials.get(), groups);
      auto color_sum = [&](std::size_t cell, std::size_t channel) {
        std::size_t offset = (cell * 3 + channel) * 2;
        std::uint64_t value = std::uint64_t(color_values[offset]) +
                              (std::uint64_t(color_values[offset + 1]) << 32);
        return float(double(value) / 65535.);
      };
      auto fill = [&](DensityGrid &grid, unsigned width, unsigned height,
                      std::size_t offset,
                      std::optional<std::size_t> color_offset) {
        grid.width = width;
        grid.height = height;
        std::size_t n = std::size_t(width) * height;
        grid.counts.assign(integers.begin() + offset,
                           integers.begin() + offset + n);
        grid.maximum =
            *std::max_element(grid.counts.begin(), grid.counts.end());
        if (color_offset) {
          grid.color_sums.resize(n);
          for (std::size_t i = 0; i < n; i++)
            for (std::size_t c = 0; c < 3; c++)
              grid.color_sums[i][c] = color_sum(*color_offset + i, c);
        }
      };
      for (std::size_t c = 0; c < 4; c++)
        if (p.wave_flags & (1u << c)) {
          fill(result->waveform[c], o.wave_width, o.wave_height, c * wave_size,
               c == 0 ? std::optional<std::size_t>{0} : std::nullopt);
          set_waveform_column_coverage(result->waveform[c],
                                       unsigned(size.width));
        }
      if (do_vector)
        fill(result->vectorscope, o.vector_width, o.vector_height,
             4 * wave_size, wave_size);
      for (std::size_t c = 0; c < 4; c++)
        if (p.hist_flags & (1u << c)) {
          auto &h = result->histograms[c];
          h.counts.resize(o.histogram_bins);
          for (std::size_t b = 0; b < o.histogram_bins; b++)
            h.counts[b] = integers[4 * wave_size + vector_size +
                                   c * o.histogram_bins + b];
          h.maximum = *std::max_element(h.counts.begin(), h.counts.end());
          if (c == 0) {
            h.color_sums.resize(o.histogram_bins);
            for (std::size_t i = 0; i < o.histogram_bins; i++)
              for (std::size_t j = 0; j < 3; j++)
                h.color_sums[i][j] = color_sum(wave_size + vector_size + i, j);
          }
        }
      Rgb source_mean{}, work_mean{};
      double source_x{};
      for (const auto &group : reduced) {
        result->valid_count += group.counts[0];
        result->invalid_count += group.counts[1];
        result->hue_count += group.counts[2];
        source_x += group.x_sum[0];
        for (std::size_t c = 0; c < 3; c++) {
          source_mean[c] += group.source_sum[c];
          work_mean[c] += group.work_sum[c];
        }
      }
      if (result->valid_count) {
        for (std::size_t c = 0; c < 3; c++) {
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
      result->prepare_ms = work_reused ? 0 : work.value().prepare_ms;
      result->statistics_ms =
          std::chrono::duration<double, std::milli>(Clock::now() - stats_start)
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
    const bool detail_wave =
        !reuse_wave_detail && o.waveform_visible && o.wave_detail_width;
    const bool detail_vector =
        !reuse_vector_detail && o.vector_visible && o.vector_detail_width;
    if (detail_wave || detail_vector) {
      auto start = Clock::now();
      const std::size_t wave_size =
          detail_wave ? std::size_t(std::min(o.wave_detail_width,
                                             unsigned(size.width))) *
                            o.wave_detail_height
                      : 0;
      const std::size_t vector_size =
          detail_vector
              ? std::size_t(o.vector_detail_width) * o.vector_detail_height
              : 0;
      work = ensure_projected(
          work.value(),
          detail_wave || (detail_vector && o.vector_mode == VectorMode::ycbcr),
          detail_vector && o.vector_mode == VectorMode::perceptual);
      if (!work)
        return Out::failure(work.error());
      auto counts = structured(
          *runtime_, std::max<std::size_t>(1, wave_size * 4 + vector_size), 4);
      auto colors = structured(
          *runtime_, std::max<std::size_t>(1, (wave_size + vector_size) * 6),
          4);
      const auto groups = (std::size_t(size.width) * size.height + 255) / 256;
      Params p{};
      p.width = size.width;
      p.height = size.height;
      p.space = unsigned(request.settings.working_space);
      p.white = float(request.settings.reference_white_nits);
      p.mode = 1;
      p.mask_kind = request.mask.enabled
                        ? (request.mask.shape == MaskShape::rectangle ? 1u : 2u)
                        : 0u;
      p.mx = float(request.mask.bounds.x);
      p.my = float(request.mask.bounds.y);
      p.mw = float(request.mask.bounds.width);
      p.mh = float(request.mask.bounds.height);
      p.wave_width =
          detail_wave ? std::min(o.wave_detail_width, unsigned(size.width)) : 0;
      p.wave_height = detail_wave ? o.wave_detail_height : 0;
      p.vector_width = detail_vector ? o.vector_detail_width : 0;
      p.vector_height = detail_vector ? o.vector_detail_height : 0;
      p.wave_flags = !detail_wave                                    ? 0u
                     : o.wave_mode == WaveMode::intensity            ? 1u
                     : o.wave_mode == WaveMode::parade_intensity_rgb ? 15u
                                                                     : 14u;
      p.vector_enabled = detail_vector ? 1u : 0u;
      p.vector_mode = unsigned(o.vector_mode);
      p.amplitude_zoom = float(o.amplitude_view.zoom);
      p.amplitude_pan = float(o.amplitude_view.pan);
      if (detail_wave)
        result->waveform_detail_view = o.amplitude_view;
      if (detail_vector) {
        auto extents = vector_detail_extents(request);
        result->vector_detail_x_extent = extents[0];
        result->vector_detail_y_extent = extents[1];
        result->vector_detail_zoom = o.vector_zoom;
        result->vector_detail_center = o.vector_pan;
        p.vector_scale_x = float(1 / (2 * extents[0]));
        p.vector_scale_y = float(1 / (2 * extents[1]));
        p.vector_center_x = float(o.vector_pan[0]);
        p.vector_center_y = float(o.vector_pan[1]);
      }
      const auto rows = (groups + 65534) / 65535;
      const auto columns = (groups + rows - 1) / rows;
      p.group_columns = UINT(columns);
      p.group_count = UINT(groups);
      dispatch(*runtime_, "statistics", p, nullptr, work.value().work.get(),
               nullptr, counts.get(), colors.get(), nullptr, nullptr, nullptr,
               UINT(columns), UINT((groups + columns - 1) / columns),
               work.value().projected_wave.get(),
               work.value().projected_vector.get());
      auto integers = read_buffer<std::uint32_t>(
          *runtime_, counts.get(),
          std::max<std::size_t>(1, wave_size * 4 + vector_size));
      auto sums = read_buffer<std::uint32_t>(
          *runtime_, colors.get(),
          std::max<std::size_t>(1, (wave_size + vector_size) * 6));
      auto fill = [&](DensityGrid &grid, unsigned width, unsigned height,
                      std::size_t offset,
                      std::optional<std::size_t> color_offset) {
        grid.width = width;
        grid.height = height;
        std::size_t n = std::size_t(width) * height;
        grid.counts.assign(integers.begin() + offset,
                           integers.begin() + offset + n);
        grid.maximum =
            *std::max_element(grid.counts.begin(), grid.counts.end());
        if (color_offset) {
          grid.color_sums.resize(n);
          for (std::size_t i = 0; i < n; i++)
            for (std::size_t c = 0; c < 3; c++) {
              std::size_t k = ((*color_offset + i) * 3 + c) * 2;
              grid.color_sums[i][c] =
                  float(double(std::uint64_t(sums[k]) +
                               (std::uint64_t(sums[k + 1]) << 32)) /
                        65535.);
            }
        }
      };
      for (std::size_t c = 0; c < 4; c++)
        if (p.wave_flags & (1u << c)) {
          fill(result->waveform_detail[c], p.wave_width, p.wave_height,
               c * wave_size,
               c == 0 ? std::optional<std::size_t>{0} : std::nullopt);
          set_waveform_column_coverage(result->waveform_detail[c],
                                       unsigned(size.width));
        }
      if (detail_vector)
        fill(result->vectorscope_detail, p.vector_width, p.vector_height,
             4 * wave_size, wave_size);
      result->detail_ms =
          std::chrono::duration<double, std::milli>(Clock::now() - start)
              .count();
      transient_bytes =
          std::max(transient_bytes, (wave_size * 4 + vector_size) * 4 +
                                        (wave_size + vector_size) * 24);
    }
    auto sample_start = Clock::now();
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
      auto rect = sample_bounds(size, sample);
      std::size_t n = std::size_t(rect.width) * std::size_t(rect.height);
      if (!n) {
        SampleResult empty;
        empty.request = sample;
        empty.clipped_bounds = rect;
        result->samples.push_back(std::move(empty));
        continue;
      }
      auto data =
          structured(*runtime_, n * 2, sizeof(std::array<float, 4>), false);
      Params p{};
      p.width = size.width;
      p.height = size.height;
      p.sample_x = std::uint32_t(rect.x);
      p.sample_y = std::uint32_t(rect.y);
      p.sample_width = std::uint32_t(rect.width);
      p.sample_height = std::uint32_t(rect.height);
      dispatch(*runtime_, "sample_region", p, source.value().get(),
               work.value().work.get(), nullptr, nullptr, nullptr, nullptr,
               data.get(), nullptr, (UINT(rect.width) + 7) / 8,
               (UINT(rect.height) + 7) / 8);
      auto values =
          read_buffer<std::array<float, 4>>(*runtime_, data.get(), n * 2);
      FloatImage ss(n), ww(n);
      for (std::size_t i = 0; i < n; i++) {
        ss[i] = values[i * 2];
        ww[i] = values[i * 2 + 1];
      }
      result->samples.push_back(
          summarize_sample(size, sample, request, rect, ss, ww));
      transient_bytes = std::max(transient_bytes, n * 64);
    }
    result->sampling_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - sample_start)
            .count();
    // Source and work are persistent FP32 textures. An external LinearSource
    // also requires one cached analysis-device upload; it is counted here.
    const auto uploaded_copy =
        uploaded_source_ == input.source ? input.source->byte_count() : 0;
    result->retained_bytes = input.source->byte_count() * 2 + uploaded_copy +
                             work.value().projected_bytes +
                             result_bytes(*result);
    const auto prior =
        last_result_ ? additional_result_bytes(*last_result_, *result) : 0;
    result->peak_bytes =
        std::max(work_reused ? std::size_t{} : work.value().peak_bytes + prior,
                 result->retained_bytes + transient_bytes + prior);
    last_source_ = input.source;
    last_request_ = identity;
    last_result_ = result;
    return Out::success(std::move(result));
  } catch (const winrt::hresult_error &e) {
    return Out::failure(native_error("analysis_gpu_failed", e.code()));
  } catch (const std::bad_alloc &) {
    return Out::failure(native_error("analysis_host_allocation_failed"));
  }
}

Result<LinearSourceRef, Error>
WindowsAnalysisPort::compose_report(const Input &input,
                                    const ReportPlan &plan) {
  using Out = Result<LinearSourceRef, Error>;
  if (!input.source || !valid_report_plan(plan))
    return Out::failure(analysis_error("invalid_report_plan"));
  try {
    std::lock_guard gpu_lock(runtime_->mutex);
    auto source = source_texture(input.source);
    if (!source)
      return Out::failure(source.error());
    auto work = plan.source_view.false_color
                    ? ensure_work(input, plan.source_view.settings,
                                  source.value().get())
                    : Result<WorkCache, Error>::success(WorkCache{});
    if (!work)
      return Out::failure(work.error());
    auto size = plan.underlay.size;
    auto ui_texture = [&](const UiImage &image) {
      D3D11_TEXTURE2D_DESC d{};
      d.Width = size.width;
      d.Height = size.height;
      d.MipLevels = 1;
      d.ArraySize = 1;
      d.Format = DXGI_FORMAT_R8G8B8A8_UINT;
      d.SampleDesc.Count = 1;
      d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      d.Usage = D3D11_USAGE_IMMUTABLE;
      D3D11_SUBRESOURCE_DATA data{image.rgba.data(), UINT(size.width * 4), 0};
      winrt::com_ptr<ID3D11Texture2D> texture;
      winrt::check_hresult(
          runtime_->gpu.device->CreateTexture2D(&d, &data, texture.put()));
      return texture;
    };
    auto underlay = ui_texture(plan.underlay),
         overlay = ui_texture(plan.overlay);
    auto output = create_texture(*runtime_, size);
    ReportParams p{};
    const auto &v = plan.source_view;
    p.width = size.width;
    p.height = size.height;
    p.space = unsigned(v.settings.working_space);
    p.false_color = v.false_color ? 1u : 0u;
    p.scale = float(v.scale);
    p.offset_x = float(v.offset_x);
    p.offset_y = float(v.offset_y);
    p.white = float(v.settings.reference_white_nits);
    p.mask_kind =
        v.mask.enabled ? (v.mask.shape == MaskShape::rectangle ? 1u : 2u) : 0u;
    p.mask_x = float(v.mask.bounds.x);
    p.mask_y = float(v.mask.bounds.y);
    p.mask_width = float(v.mask.bounds.width);
    p.mask_height = float(v.mask.bounds.height);
    p.mask_dim = float(v.mask_outside_factor);
    p.ui_white = float(plan.ui_white_edr);
    p.source_x = plan.source_rect.x;
    p.source_y = plan.source_rect.y;
    p.source_width = plan.source_rect.width;
    p.source_height = plan.source_rect.height;
    Params base{};
    auto source_size = input.source->size_px();
    base.width = source_size.width;
    base.height = source_size.height;
    auto constants = runtime_->gpu.buffer(sizeof(base),
                                          D3D11_BIND_CONSTANT_BUFFER, 0, &base);
    auto report_constants =
        runtime_->gpu.buffer(sizeof(p), D3D11_BIND_CONSTANT_BUFFER, 0, &p);
    auto src = texture_srv(*runtime_, source.value().get());
    auto wrk =
        texture_srv(*runtime_, work.value().work ? work.value().work.get()
                                                 : source.value().get());
    auto ui0 = texture_srv(*runtime_, underlay.get()),
         ui1 = texture_srv(*runtime_, overlay.get());
    auto dst = texture_uav(*runtime_, output.get());
    ID3D11Buffer *cb[]{constants.get(), report_constants.get()};
    ID3D11ShaderResourceView *srv[]{src.get(), wrk.get(), ui0.get(), ui1.get()};
    ID3D11UnorderedAccessView *uav[]{dst.get()};
    auto *c = runtime_->gpu.context.get();
    c->CSSetShader(runtime_->kernels.at("render_report").get(), nullptr, 0);
    c->CSSetConstantBuffers(0, 2, cb);
    c->CSSetShaderResources(0, 4, srv);
    c->CSSetUnorderedAccessViews(0, 1, uav, nullptr);
    c->Dispatch((size.width + 7) / 8, (size.height + 7) / 8, 1);
    ID3D11ShaderResourceView *null_srv[4]{};
    ID3D11UnorderedAccessView *null_uav[1]{};
    c->CSSetShaderResources(0, 4, null_srv);
    c->CSSetUnorderedAccessViews(0, 1, null_uav, nullptr);
    c->Flush();
    return Out::success(
        std::make_shared<GpuSource>(runtime_, size, std::move(output)));
  } catch (const winrt::hresult_error &e) {
    return Out::failure(native_error("report_gpu_failed", e.code()));
  }
}
} // namespace

Result<WindowsAnalysisBackend, Error> make_windows_analysis_backend(
    std::shared_ptr<DiagnosticsPort> diagnostics) {
  try {
    auto runtime = std::make_shared<Runtime>();
    auto presenter = make_windows_analysis_presenter(std::move(diagnostics));
    if (!presenter)
      return Result<WindowsAnalysisBackend, Error>::failure(presenter.error());
    return Result<WindowsAnalysisBackend, Error>::success(
        {std::make_shared<WindowsAnalysisPort>(std::move(runtime)),
         presenter.value()});
  } catch (const winrt::hresult_error &e) {
    return Result<WindowsAnalysisBackend, Error>::failure(
        native_error("analysis_device_unavailable", e.code()));
  } catch (const std::bad_alloc &) {
    return Result<WindowsAnalysisBackend, Error>::failure(
        native_error("analysis_session_allocation_failed"));
  }
}
} // namespace hdrshot
