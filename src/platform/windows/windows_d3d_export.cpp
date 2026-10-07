#include "platform/windows/windows_d3d_export.hpp"
#include "platform/windows/windows_d3d.hpp"
#include "platform/windows/windows_color.hpp"
#include "domain/annotation/annotation_compositing.hpp"
#include <bit>
#include <limits>
#include <mutex>

namespace hdrshot {
namespace {
class WindowsCleanSamples final : public LinearSampleStorage {
 public:
  explicit WindowsCleanSamples(std::vector<std::array<float, 4>> samples)
      : samples_(std::move(samples)) {}
  std::size_t sample_count() const override { return samples_.size(); }
  Result<bool, Error> wait_until_ready() const override {
    return Result<bool, Error>::success(true);
  }
  Result<std::vector<std::array<float, 4>>, Error> read_samples(
      std::size_t offset, std::size_t count) const override {
    if (offset > samples_.size() || count > samples_.size() - offset)
      return Result<std::vector<std::array<float, 4>>, Error>::failure(
          {ErrorCode::invalid_input, "WindowsCleanSamples", Retryability::never, {}});
    return Result<std::vector<std::array<float, 4>>, Error>::success(
        std::vector<std::array<float, 4>>(
            samples_.begin() + static_cast<std::ptrdiff_t>(offset),
            samples_.begin() + static_cast<std::ptrdiff_t>(offset + count)));
  }
 private:
  std::vector<std::array<float, 4>> samples_;
};
class WindowsCleanComposition final : public CleanCompositionPort {
 public:
  Result<std::vector<std::array<float, 4>>, Error> compose(
      std::span<const CleanCompositionSample> samples) override {
    return cpu_.compose(samples);
  }
  Result<LinearSampleRef, Error> compose_native(
      LinearSourceRef source, std::span<const PositionedCleanSample> samples) override {
    if (!source) return Result<LinearSampleRef, Error>::failure(
        {ErrorCode::invalid_input, "WindowsCleanComposition", Retryability::never, {}});
    int left = source->size_px().width, top = source->size_px().height, right = 0, bottom = 0;
    for (const auto& sample : samples) {
      if (sample.x >= static_cast<unsigned>(source->size_px().width) ||
          sample.y >= static_cast<unsigned>(source->size_px().height))
        return Result<LinearSampleRef, Error>::failure(
            {ErrorCode::invalid_input, "WindowsCleanComposition", Retryability::never, {}});
      if (sample.annotation[3] > 0) {
        left = std::min(left, static_cast<int>(sample.x));
        top = std::min(top, static_cast<int>(sample.y));
        right = std::max(right, static_cast<int>(sample.x) + 1);
        bottom = std::max(bottom, static_cast<int>(sample.y) + 1);
      }
    }
    LinearFloatPixels source_region;
    if (right > left && bottom > top) {
      auto region = source->read_region({left, top, right - left, bottom - top});
      if (!region) return Result<LinearSampleRef, Error>::failure(region.error());
      source_region = std::move(region.value());
    }
    std::vector<CleanCompositionSample> work;
    work.reserve(samples.size());
    for (const auto& sample : samples) {
      CleanCompositionSample item{sample.annotation, {}};
      if (sample.annotation[3] > 0) {
        const auto p = (static_cast<std::size_t>(sample.y - static_cast<unsigned>(top)) *
            static_cast<std::size_t>(right - left) +
            static_cast<std::size_t>(sample.x - static_cast<unsigned>(left))) * 4U;
        for (std::size_t c = 0; c < 3U; ++c) item.source[c] = source_region[p + c];
      }
      work.push_back(item);
    }
    auto composed = cpu_.compose(work);
    if (!composed) return Result<LinearSampleRef, Error>::failure(composed.error());
    return Result<LinearSampleRef, Error>::success(
        std::make_shared<WindowsCleanSamples>(std::move(composed.value())));
  }
 private:
  CpuCleanComposition cpu_;
};
// The source texture is already Linear Extended Display P3 in EDR units.
// These kernels never perform a platform/source gamma conversion.
constexpr const char* shader=R"HLSL(
Texture2D<float4> source : register(t0);
StructuredBuffer<uint> coverage : register(t1);
StructuredBuffer<float4> colors : register(t2);
RWStructuredBuffer<uint4> output : register(u0);
RWStructuredBuffer<uint> stats : register(u1);
cbuffer Params : register(b0) {
  uint width; uint height; uint mode; float white_nits;
  uint max_code; uint3 padding;
};
groupshared uint light_max[256];
groupshared uint2 light_sum[256];
groupshared uint clipped_pixels[256];
groupshared uint clipped_channels[256];
float srgb_encode(float v) {
  return v<=0.0031308 ? 12.92*v : 1.055*pow(v,1.0/2.4)-0.055;
}
float pq_encode(float nits) {
  float p=pow(clamp(nits,0.0,10000.0)/10000.0,2610.0/16384.0);
  return pow((3424.0/4096.0+(2413.0/128.0)*p)/(1.0+(2392.0/128.0)*p),2523.0/32.0);
}
[numthreads(256,1,1)]
void main(uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
  uint gid=(group.y*65535+group.x)*256+lane;
  uint total=width*height;
  light_max[lane]=0; light_sum[lane]=uint2(0,0);
  clipped_pixels[lane]=0; clipped_channels[lane]=0;
  if(gid<total) {
    uint index=coverage[gid];
    float4 sample=index==0 ? float4(0,0,0,1) : colors[index-1];
    float3 p3=sample.rgb;
    if(sample.a>0) {
      float3 underlying=source.Load(int3(gid%width,gid/width,0)).rgb;
      if(!all(isfinite(underlying))) { InterlockedAdd(stats[0],1); underlying=0; }
      p3+=sample.a*max(underlying,0.0);
    }
    uint4 result=uint4(0,0,0,0x3c00);
    if(mode==2 || mode==3) {
      // Mode 3 prepares the temporary JPEG image captured on an SDR display.
      // Reduce the actual output samples after clipping, in this same dispatch.
      p3=mode==3 ? clamp(p3,0.0,1.0) : min(p3,10000.0/203.0);
      result=uint4(f32tof16(p3.r),f32tof16(p3.g),f32tof16(p3.b),0x3c00);
      float3 measured=mode==3 ? float3(f16tof32(result.r),f16tof32(result.g),f16tof32(result.b)) : p3;
      float maximum=max(measured.r,max(measured.g,measured.b));
      InterlockedMax(stats[6],asuint(maximum));
      if(index==0) InterlockedMax(stats[7],asuint(maximum));
    } else {
      [unroll] for(uint c=0;c<3;++c) {
        float value;
        if(mode==1) {
          float nits=p3[c]*white_nits;
          if(nits>10000) { clipped_channels[lane]++; clipped_pixels[lane]=1; }
          value=pq_encode(nits);
        } else value=srgb_encode(max(p3[c],0.0));
        uint effective=uint(floor(saturate(value)*float(max_code)+0.5));
        result[c]=uint(floor(float(effective)*65535.0/float(max_code)+0.5));
      }
      if(mode==1) {
        uint units=uint(floor(min(max(p3.r,max(p3.g,p3.b))*white_nits,10000.0)*10000.0+0.5));
        light_max[lane]=units; light_sum[lane]=uint2(units,0);
      }
    }
    output[gid]=result;
  }
  GroupMemoryBarrierWithGroupSync();
  for(uint stride=128;stride>0;stride/=2) {
    if(lane<stride) {
      uint previous=light_sum[lane].x;
      light_sum[lane].x+=light_sum[lane+stride].x;
      light_sum[lane].y+=light_sum[lane+stride].y+(light_sum[lane].x<previous ? 1:0);
      light_max[lane]=max(light_max[lane],light_max[lane+stride]);
      clipped_channels[lane]+=clipped_channels[lane+stride];
      clipped_pixels[lane]+=clipped_pixels[lane+stride];
    }
    GroupMemoryBarrierWithGroupSync();
  }
  if(lane==0 && mode==1) {
    InterlockedAdd(stats[1],clipped_channels[0]);
    InterlockedAdd(stats[2],clipped_pixels[0]);
    InterlockedMax(stats[3],light_max[0]);
    uint previous;
    InterlockedAdd(stats[4],light_sum[0].x,previous);
    InterlockedAdd(stats[5],light_sum[0].y+(previous+light_sum[0].x<previous ? 1:0));
  }
}
)HLSL";
Error invalid(const char* reason) {
  return {ErrorCode::invalid_color_contract,"WindowsD3DExport",Retryability::never,{{"reason",reason}}};
}
struct Output {
  std::vector<std::array<std::uint32_t,4>> pixels;
  std::array<std::uint32_t,8> stats{};
};
}
std::shared_ptr<CleanCompositionPort> windows_clean_composition() {
  return std::make_shared<WindowsCleanComposition>();
}
struct WindowsD3DExportPixelProcessor::Impl {
  WindowsD3DDevice gpu;
  winrt::com_ptr<ID3D11ComputeShader> compute;
  std::mutex mutex;
  Impl() {
    auto blob=gpu.compile(shader,"main","cs_5_0");
    winrt::check_hresult(gpu.device->CreateComputeShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,compute.put()));
  }
  Result<Output,Error> run(const SelectionRoiView& source,const AnnotationPixelPlan& plan,
                          UINT mode,float white,UINT max_code) {
    const std::scoped_lock lock(mutex);
    CanonicalFrameView cpu_source;
    SelectionRoiView source_view = source;
    if (source.linear_source) {
      auto read = FrameCropper::read_cpu_region(source);
      if (!read) return Result<Output,Error>::failure(read.error());
      cpu_source = std::move(read.value());
      source_view = FrameCropper::view(cpu_source);
    }
    auto cpu_plan = materialize_annotation_plan(plan);
    if (!cpu_plan) return Result<Output,Error>::failure(cpu_plan.error());
    if(!WindowsColor::valid_linear_p3_roi(source_view) ||
        source_view.size_px!=plan.output_size_px ||
        source.size_px.width>16384 || source.size_px.height>16384) return Result<Output,Error>::failure(invalid("invalid_linear_p3_roi"));
    const auto count=static_cast<std::size_t>(source.size_px.width)*source.size_px.height;
    if(count>std::numeric_limits<UINT>::max()/16U || source.row_stride_samples>std::numeric_limits<UINT>::max()/2U)
      return Result<Output,Error>::failure(invalid("gpu_resource_size_overflow"));
    const auto upload=prepare_annotation_gpu_upload(cpu_plan.value());
    if(!upload) return Result<Output,Error>::failure(upload.error());
    if(upload.value().samples.size()>std::numeric_limits<UINT>::max()/16U) return Result<Output,Error>::failure(invalid("annotation_upload_overflow"));
    try {
      D3D11_TEXTURE2D_DESC desc{};
      desc.Width=source_view.size_px.width; desc.Height=source_view.size_px.height;
      desc.MipLevels=1; desc.ArraySize=1;
      desc.Format=source_view.rgba_float.empty() ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
      desc.SampleDesc.Count=1; desc.Usage=D3D11_USAGE_IMMUTABLE; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
      const D3D11_SUBRESOURCE_DATA input{source_view.sample_data(source_view.first_sample_offset),
          static_cast<UINT>(source_view.row_stride_samples*source_view.sample_bytes()),0};
      winrt::com_ptr<ID3D11Texture2D> texture;
      winrt::check_hresult(gpu.device->CreateTexture2D(&desc,&input,texture.put()));
      const auto indices=gpu.buffer(static_cast<UINT>(count*4U),D3D11_BIND_SHADER_RESOURCE,4,upload.value().indices.data());
      const auto samples=gpu.buffer(static_cast<UINT>(upload.value().samples.size()*16U),D3D11_BIND_SHADER_RESOURCE,16,upload.value().samples.data());
      const auto result=gpu.buffer(static_cast<UINT>(count*16U),D3D11_BIND_UNORDERED_ACCESS,16);
      const std::array<UINT,8> zeros{};
      const auto stats=gpu.buffer(32,D3D11_BIND_UNORDERED_ACCESS,4,zeros.data());
      struct Params { UINT width,height,mode; float white; UINT max_code; UINT padding[3]; };
      const Params params{desc.Width,desc.Height,mode,white,max_code,{}};
      const auto constants=gpu.buffer(sizeof(params),D3D11_BIND_CONSTANT_BUFFER,0,&params);
      const auto source_srv=gpu.srv(texture), index_view=gpu.srv(indices), color_view=gpu.srv(samples);
      const auto output_view=gpu.uav(result), stats_view=gpu.uav(stats);
      ID3D11ShaderResourceView* resources[]{source_srv.get(),index_view.get(),color_view.get()};
      ID3D11UnorderedAccessView* outputs[]{output_view.get(),stats_view.get()};
      ID3D11Buffer* constant_buffers[]{constants.get()};
      gpu.context->CSSetShader(compute.get(),nullptr,0);
      gpu.context->CSSetShaderResources(0,3,resources);
      gpu.context->CSSetUnorderedAccessViews(0,2,outputs,nullptr);
      gpu.context->CSSetConstantBuffers(0,1,constant_buffers);
      const auto groups=static_cast<UINT>((count+255U)/256U);
      gpu.context->Dispatch(std::min(groups,65535U),(groups+65534U)/65535U,1);
      ID3D11ShaderResourceView* clear_resources[3]{};
      ID3D11UnorderedAccessView* clear_outputs[2]{};
      gpu.context->CSSetShaderResources(0,3,clear_resources);
      gpu.context->CSSetUnorderedAccessViews(0,2,clear_outputs,nullptr);
      Output out;
      gpu.read_buffer(stats,out.stats.data(),32);
      if(out.stats[0]) return Result<Output,Error>::failure(invalid("non_finite_visible_source"));
      out.pixels.resize(count);
      gpu.read_buffer(result,out.pixels.data(),static_cast<UINT>(count*16U));
      return Result<Output,Error>::success(std::move(out));
    } catch(const winrt::hresult_error& e) {
      return Result<Output,Error>::failure(windows_d3d_error("WindowsD3DExport","compute_or_readback",e.code()));
    } catch(const std::bad_alloc&) {
      return Result<Output,Error>::failure(windows_d3d_error("WindowsD3DExport","allocation_failed",E_OUTOFMEMORY));
    }
  }
};
WindowsD3DExportPixelProcessor::WindowsD3DExportPixelProcessor(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
WindowsD3DExportPixelProcessor::~WindowsD3DExportPixelProcessor()=default;
Result<std::unique_ptr<WindowsD3DExportPixelProcessor>,Error> WindowsD3DExportPixelProcessor::create() {
  try { return Result<std::unique_ptr<WindowsD3DExportPixelProcessor>,Error>::success(
      std::unique_ptr<WindowsD3DExportPixelProcessor>(new WindowsD3DExportPixelProcessor(std::make_unique<Impl>()))); }
  catch(const winrt::hresult_error& e) {
    return Result<std::unique_ptr<WindowsD3DExportPixelProcessor>,Error>::failure(windows_d3d_error("WindowsD3DExport","create_device_or_shader",e.code()));
  }
}
Result<ExportPixelProcessResult,Error> WindowsD3DExportPixelProcessor::process(const ExportPixelProcessRequest& request) {
  if(!request.source || !request.pixel_plan || !valid_hdr_pq_precision(request.hdr_pq_precision) ||
      (request.pq_diffuse_white!=PqDiffuseWhite::nits_100 && request.pq_diffuse_white!=PqDiffuseWhite::nits_203))
    return Result<ExportPixelProcessResult,Error>::failure(invalid("invalid_request"));
  const bool hdr=request.output_plan.encoding_intent==EncodingIntent::hdr_pq;
  const auto bits=static_cast<UINT>(request.hdr_pq_precision);
  const auto max_code=hdr ? ((1U<<bits)-1U):65535U;
  const auto pixels=impl_->run(*request.source,*request.pixel_plan,hdr?1U:0U,
      static_cast<float>(pq_diffuse_white_nits(request.pq_diffuse_white)),max_code);
  if(!pixels) return Result<ExportPixelProcessResult,Error>::failure(pixels.error());
  ExportPixelProcessResult out;
  out.size_px=request.source->size_px;
  out.output_encoding={ColorPrimaries::display_p3,hdr?TransferFunction::pq:TransferFunction::srgb,
      AlphaMode::opaque,hdr?pq_diffuse_white_nits(request.pq_diffuse_white):0.0};
  out.rgb_u16.reserve(pixels.value().pixels.size()*3U);
  for(const auto& p:pixels.value().pixels) for(std::size_t c=0;c<3;++c) out.rgb_u16.push_back(static_cast<std::uint16_t>(p[c]));
  const auto& s=pixels.value().stats;
  out.luminance_clip={s[2],s[1]};
  if(hdr) out.content_light=ContentLightStatistics{s[3],static_cast<std::uint64_t>(s[4])+(static_cast<std::uint64_t>(s[5])<<32U),pixels.value().pixels.size()};
  return Result<ExportPixelProcessResult,Error>::success(std::move(out));
}
Result<LinearDisplayP3HalfImage,Error> WindowsD3DExportPixelProcessor::render(const UltraHdrInputRenderRequest& request) {
  if(!request.source || !request.pixel_plan || request.reference_white_nits!=kUltraHdrReferenceWhiteNits)
    return Result<LinearDisplayP3HalfImage,Error>::failure(invalid("invalid_uhdr_request"));
  const auto mode=request.source->display_dynamic_range==DisplayDynamicRange::sdr ? 3U : 2U;
  const auto pixels=impl_->run(*request.source,*request.pixel_plan,mode,203.0F,65535U);
  if(!pixels) return Result<LinearDisplayP3HalfImage,Error>::failure(pixels.error());
  LinearDisplayP3HalfImage out;
  out.size_px=request.source->size_px;
  out.rgba_half.reserve(pixels.value().pixels.size()*4U);
  for(const auto& p:pixels.value().pixels) for(auto c:p) out.rgba_half.push_back(static_cast<std::uint16_t>(c));
  out.maximum_linear_component=std::bit_cast<float>(pixels.value().stats[6]);
  out.source_visible_maximum_linear_component=std::bit_cast<float>(pixels.value().stats[7]);
  out.capture_sdr_tolerance=request.source->capture_sdr_tolerance;
  return Result<LinearDisplayP3HalfImage,Error>::success(std::move(out));
}
}  // namespace hdrshot
