#include "platform/windows/windows_gpu_source.hpp"
#include "platform/windows/windows_d3d.hpp"
#include "platform/windows/windows_icc_transform.hpp"
#include "ports/export_ports.hpp"
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

namespace hdrshot {
namespace {
constexpr const char *shader = R"HLSL(
Texture2D<float4> input : register(t0);
RWTexture2D<float4> output : register(u0);
cbuffer Params : register(b0) { uint width; uint height; float white_scale; float gain; uint use_icc; };
float srgb_encode_channel(float x) {
  float a=abs(x);
  return (a<=0.0031308?12.92*a:1.055*pow(a,1.0/2.4)-0.055)*(x<0.0?-1.0:1.0);
}
[numthreads(16,16,1)]
void main(uint3 position : SV_DispatchThreadID) {
  if (position.x >= width || position.y >= height) return;
  float3 rgb = input.Load(int3(position.xy, 0)).rgb;
  float3 p3;
  if (use_icc != 0) {
    float3 corrected=rgb*gain;
    p3=windows_icc_device_to_p3(float3(srgb_encode_channel(corrected.r),
      srgb_encode_channel(corrected.g),srgb_encode_channel(corrected.b)));
  } else {
    p3 = float3(
        0.82246197 * rgb.r + 0.17753803 * rgb.g,
        0.03319420 * rgb.r + 0.96680580 * rgb.g,
        0.01708263 * rgb.r + 0.07239744 * rgb.g + 0.91051993 * rgb.b);
    p3 *= white_scale*gain;
  }
  output[position.xy] = float4(p3, 1.0);
}
)HLSL";
Error source_error(const char *reason, HRESULT hr = E_FAIL) {
  return {ErrorCode::invalid_color_contract,
          "WindowsGpuSource",
          Retryability::after_recreate,
          {{"reason", reason},
           {"hresult", std::to_string(static_cast<std::uint32_t>(hr))}}};
}
class WindowsGpuSource final : public LinearSource {
public:
  WindowsGpuSource(PixelSize size, winrt::com_ptr<ID3D11Device> device,
                   winrt::com_ptr<ID3D11DeviceContext> context,
                   winrt::com_ptr<ID3D11Texture2D> texture,
                   winrt::com_ptr<ID3D11Query> completion)
      : size_(size), device_(std::move(device)), context_(std::move(context)),
        texture_(std::move(texture)), completion_(std::move(completion)) {}
  PixelSize size_px() const override { return size_; }
  std::size_t byte_count() const override {
    return static_cast<std::size_t>(size_.width) * size_.height * 4U *
           sizeof(float);
  }
  Result<bool, Error> wait_until_ready() const override {
    const std::scoped_lock lock(mutex_);
    return wait_locked();
  }
  Result<LinearFloatPixels, Error> read_region(PixelRect rect) const override {
    if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0 ||
        rect.x >= size_.width || rect.y >= size_.height ||
        rect.width > size_.width - rect.x ||
        rect.height > size_.height - rect.y)
      return Result<LinearFloatPixels, Error>::failure(
          source_error("invalid_region"));
    const std::scoped_lock lock(mutex_);
    const auto ready = wait_locked();
    if (!ready)
      return Result<LinearFloatPixels, Error>::failure(ready.error());
    try {
      D3D11_TEXTURE2D_DESC desc{};
      desc.Width = static_cast<UINT>(rect.width);
      desc.Height = static_cast<UINT>(rect.height);
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_STAGING;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      winrt::com_ptr<ID3D11Texture2D> staging;
      winrt::check_hresult(
          device_->CreateTexture2D(&desc, nullptr, staging.put()));
      const D3D11_BOX box{static_cast<UINT>(rect.x),
                          static_cast<UINT>(rect.y),
                          0,
                          static_cast<UINT>(rect.x + rect.width),
                          static_cast<UINT>(rect.y + rect.height),
                          1};
      context_->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, texture_.get(),
                                      0, &box);
      D3D11_MAPPED_SUBRESOURCE mapped{};
      winrt::check_hresult(
          context_->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
      LinearFloatPixels out;
      out.resize(static_cast<std::size_t>(rect.width) * rect.height * 4U);
      for (int y = 0; y < rect.height; ++y)
        std::memcpy(out.data() + static_cast<std::size_t>(y) * rect.width * 4U,
                    static_cast<const std::uint8_t *>(mapped.pData) +
                        static_cast<std::size_t>(y) * mapped.RowPitch,
                    static_cast<std::size_t>(rect.width) * 4U * sizeof(float));
      context_->Unmap(staging.get(), 0);
      return Result<LinearFloatPixels, Error>::success(std::move(out));
    } catch (const winrt::hresult_error &e) {
      return Result<LinearFloatPixels, Error>::failure(
          source_error("readback", e.code()));
    } catch (const std::bad_alloc &) {
      return Result<LinearFloatPixels, Error>::failure(
          source_error("allocation", E_OUTOFMEMORY));
    }
  }

private:
  Result<bool, Error> wait_locked() const {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      const HRESULT status =
          context_->GetData(completion_.get(), nullptr, 0, 0);
      if (status == S_OK)
        return Result<bool, Error>::success(true);
      if (FAILED(status))
        return Result<bool, Error>::failure(
            source_error("gpu_completion", status));
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return Result<bool, Error>::failure(source_error("gpu_timeout"));
  }
  PixelSize size_;
  winrt::com_ptr<ID3D11Device> device_;
  winrt::com_ptr<ID3D11DeviceContext> context_;
  winrt::com_ptr<ID3D11Texture2D> texture_;
  winrt::com_ptr<ID3D11Query> completion_;
  mutable std::mutex mutex_;
};
} // namespace

Result<LinearSourceRef, Error>
windows_normalize_scrgb_source(PixelSize size,
                               std::vector<std::uint16_t> rgba_scrgb,
                               float scrgb_to_edr_scale, double gain,
                               std::shared_ptr<const WindowsIccProfile> legacy_profile) {
  if (size.width <= 0 || size.height <= 0 || size.width > 16384 ||
      size.height > 16384 || !valid_windows_scrgb_gain(gain) ||
      !std::isfinite(scrgb_to_edr_scale) || scrgb_to_edr_scale <= 0 ||
      rgba_scrgb.size() !=
          static_cast<std::size_t>(size.width) * size.height * 4U ||
      (legacy_profile && scrgb_to_edr_scale != 1.0F))
    return Result<LinearSourceRef, Error>::failure(
        source_error("invalid_input"));
  // Half exponent 31 denotes Inf/NaN. Alpha is intentionally ignored.
  for (std::size_t i = 0; i < rgba_scrgb.size(); i += 4U)
    for (std::size_t c = 0; c < 3U; ++c)
      if ((rgba_scrgb[i + c] & 0x7c00U) == 0x7c00U)
        return Result<LinearSourceRef, Error>::failure(
            source_error("non_finite_input"));
  try {
    WindowsD3DDevice gpu;
    const std::string full_shader=std::string(WindowsIccGpuTransform::hlsl_source())+shader;
    const auto blob = gpu.compile(full_shader.c_str(), "main", "cs_5_0");
    std::shared_ptr<const WindowsIccGpuTransform> icc;
    if(legacy_profile) {
      auto created=WindowsIccGpuTransform::create(gpu.device.get(),std::move(legacy_profile));
      if(!created) return Result<LinearSourceRef,Error>::failure(created.error());
      icc=std::move(created.value());
    }
    winrt::com_ptr<ID3D11ComputeShader> compute;
    winrt::check_hresult(gpu.device->CreateComputeShader(
        blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
        compute.put()));
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(size.width);
    desc.Height = static_cast<UINT>(size.height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA input_data{rgba_scrgb.data(), desc.Width * 8U,
                                            0};
    winrt::com_ptr<ID3D11Texture2D> input;
    winrt::check_hresult(
        gpu.device->CreateTexture2D(&desc, &input_data, input.put()));
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    winrt::com_ptr<ID3D11Texture2D> output;
    winrt::check_hresult(
        gpu.device->CreateTexture2D(&desc, nullptr, output.put()));
    winrt::com_ptr<ID3D11UnorderedAccessView> output_view;
    winrt::check_hresult(gpu.device->CreateUnorderedAccessView(
        output.get(), nullptr, output_view.put()));
    const auto input_view = gpu.srv(input);
    struct Params {
      UINT width, height;
      float scale, gain;
      UINT use_icc;
      UINT pad[3];
    };
    const Params params{desc.Width, desc.Height, scrgb_to_edr_scale,
                        static_cast<float>(gain),icc?1U:0U,{0,0,0}};
    const auto constants =
        gpu.buffer(sizeof(params), D3D11_BIND_CONSTANT_BUFFER, 0, &params);
    ID3D11ShaderResourceView *srvs[]{input_view.get()};
    ID3D11UnorderedAccessView *uavs[]{output_view.get()};
    ID3D11Buffer *cbuffers[]{constants.get()};
    gpu.context->CSSetShader(compute.get(), nullptr, 0);
    gpu.context->CSSetShaderResources(0, 1, srvs);
    gpu.context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    gpu.context->CSSetConstantBuffers(0, 1, cbuffers);
    if(icc) icc->bind_cs(gpu.context.get());
    gpu.context->Dispatch((desc.Width + 15U) / 16U, (desc.Height + 15U) / 16U,
                          1);
    ID3D11ShaderResourceView *clear_srv[]{nullptr};
    ID3D11UnorderedAccessView *clear_uav[]{nullptr};
    gpu.context->CSSetShaderResources(0, 1, clear_srv);
    gpu.context->CSSetUnorderedAccessViews(0, 1, clear_uav, nullptr);
    D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT, 0};
    winrt::com_ptr<ID3D11Query> completion;
    winrt::check_hresult(
        gpu.device->CreateQuery(&query_desc, completion.put()));
    gpu.context->End(completion.get());
    gpu.context->Flush();
    return Result<LinearSourceRef, Error>::success(
        std::make_shared<WindowsGpuSource>(
            size, std::move(gpu.device), std::move(gpu.context),
            std::move(output), std::move(completion)));
  } catch (const winrt::hresult_error &e) {
    return Result<LinearSourceRef, Error>::failure(
        source_error("normalization", e.code()));
  } catch (const std::bad_alloc &) {
    return Result<LinearSourceRef, Error>::failure(
        source_error("allocation", E_OUTOFMEMORY));
  }
}
} // namespace hdrshot
