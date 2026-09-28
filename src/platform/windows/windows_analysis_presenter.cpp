#include "platform/windows/windows_analysis_backend.hpp"
#include "platform/windows/windows_analysis_shader_math.hpp"
#include "platform/windows/windows_capture.hpp"
#include "platform/windows/windows_d3d.hpp"
#include "platform/windows/windows_display_transform.hpp"
#include "ports/diagnostics_port.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <dcomp.h>
#include <mutex>
#include <optional>
#include <thread>

namespace hdrshot {
namespace {
using namespace analysis;
constexpr const char *kernels = R"HLSL(
Texture2D<float4> source:register(t0);
Texture2D<float4> work:register(t1);
Texture2D<float4> marks:register(t2);
RWTexture2D<float4> destination:register(u0);
cbuffer Params:register(b0) {
 uint sw,sh,tw,th;
 float view_scale,ox,oy,white;
 uint space,false_color,mask_kind,has_marks;
 float mx,my,mw,mh;
 float mask_dim,white_scale,output_scrgb,padding;
 uint radius,vertical,pad2,pad3;
 float4 weights[13];
};
float weight(uint i){return weights[i/4][i%4];}
A3 as_a3(float3 v){return a3(v.x,v.y,v.z);}
float3 as_v3(A3 v){return float3(v.x,v.y,v.z);}
[numthreads(8,8,1)] void blur(uint3 q:SV_DispatchThreadID){
 if(q.x>=sw||q.y>=sh)return;
 float4 sum=0;
 for(int d=-int(radius);d<=int(radius);++d){
  int x=int(q.x)+d;if(x<0||x>=int(sw))continue;
  float3 v=source.Load(int3(x,q.y,0)).rgb;
  if(finite3(as_a3(v)))sum+=float4(v,1)*weight(uint(d+int(radius)));
 }
 destination[q.xy]=sum;
}
[numthreads(8,8,1)] void prepare_work(uint3 q:SV_DispatchThreadID){
 if(q.x>=sw||q.y>=sh)return;
 A3 color=as_a3(source.Load(int3(q.xy,0)).rgb);
 if(!finite3(color)){destination[q.xy]=0;return;}
 if(radius){float4 sum=0;
  for(int d=-int(radius);d<=int(radius);++d){int y=int(q.y)+d;
   if(y>=0&&y<int(sh))sum+=work.Load(int3(q.x,y,0))*weight(uint(d+int(radius)));
  }
  if(!(sum.a>0)){destination[q.xy]=0;return;}color=as_a3(sum.rgb/sum.a);
 }
 A3 value=working_rgb(color,space,white);
 destination[q.xy]=float4(as_v3(value),finite3(value)?1:0);
}
struct Vertex{float4 pos:SV_Position;};
Vertex vs(uint id:SV_VertexID){Vertex v;float2 uv=float2((id<<1)&2,id&2);
 v.pos=float4(uv*float2(2,-2)+float2(-1,1),0,1);return v;}
float3 source_pixel(int2 p){
 p=clamp(p,int2(0,0),int2(sw-1,sh-1));A3 v=as_a3(source.Load(int3(p,0)).rgb);
 if(!finite3(v))return float3(.35,0,.35);if(space<2)v=clip3(v,1);return as_v3(v);
}
float4 ps(Vertex v):SV_Target{
 float2 pos=(v.pos.xy-float2(ox,oy))/view_scale;
 float3 color=float3(.004,.006,.008);
 if(all(pos>=0)&&pos.x<sw&&pos.y<sh){
  if(false_color){float4 w=work.Load(int3(int2(pos),0));
   color=w.a>0?as_v3(false_color_rgb(dot3(y_coefficients(working_gamut(space)),as_a3(w.rgb)))):float3(.35,0,.35);
  }else{float2 f=pos-.5;int2 low=int2(floor(f));float2 t=f-float2(low);
   color=lerp(lerp(source_pixel(low),source_pixel(low+int2(1,0)),t.x),
              lerp(source_pixel(low+int2(0,1)),source_pixel(low+int2(1,1)),t.x),t.y);
  }
  if(!mask_contains(mask_kind,pos.x,pos.y,mx,my,mw,mh))color*=mask_dim;
 }
 if(has_marks){float4 mark=marks.Load(int3(int2(v.pos.xy),0));
  uint3 code=uint3(round(mark.rgb*255));
  color=lerp(color,as_v3(ui_rgb_to_linear_p3((code.r<<16)|(code.g<<8)|code.b)),mark.a);
 }
  if(output_scrgb!=0)color=windows_present_p3(color);
 return float4(color,1);
}
)HLSL";

Error presentation_error(const char *reason) {
  return windows_d3d_error("WindowsAnalysisPresenter", reason);
}

// Explicit immutable CPU readback boundary for tests; on-screen rendering never
// reads this output back. The native draw and fixture use identical shaders.
class ReadbackSource final : public LinearSource {
public:
  ReadbackSource(PixelSize size, LinearFloatPixels pixels)
      : size_(size), pixels_(std::move(pixels)) {}
  PixelSize size_px() const override { return size_; }
  std::size_t byte_count() const override {
    return pixels_.size() * sizeof(float);
  }
  Result<bool, Error> wait_until_ready() const override {
    return Result<bool, Error>::success(true);
  }
  Result<LinearFloatPixels, Error> read_region(PixelRect r) const override {
    using Out = Result<LinearFloatPixels, Error>;
    if (r.x < 0 || r.y < 0 || r.width <= 0 || r.height <= 0 ||
        r.width > size_.width - r.x || r.height > size_.height - r.y)
      return Out::failure(presentation_error("invalid_readback_region"));
    LinearFloatPixels output(std::size_t(r.width) * r.height * 4);
    for (int y = 0; y < r.height; ++y)
      std::copy_n(pixels_.data() +
                      (std::size_t(y + r.y) * size_.width + r.x) * 4,
                  std::size_t(r.width) * 4,
                  output.data() + std::size_t(y) * r.width * 4);
    return Out::success(std::move(output));
  }

private:
  PixelSize size_;
  LinearFloatPixels pixels_;
};

struct Constants {
  UINT sw{}, sh{}, tw{}, th{};
  float scale{}, ox{}, oy{}, white{};
  UINT space{}, false_color{}, mask_kind{}, has_marks{};
  float mx{}, my{}, mw{}, mh{}, mask_dim{}, white_scale{1}, output_scrgb{},
      padding{};
  UINT radius{}, vertical{}, pad2{}, pad3{};
  std::array<float, 52> weights{};
};
static_assert(sizeof(Constants) % 16 == 0);

class Renderer {
public:
  explicit Renderer(std::shared_ptr<DiagnosticsPort> diagnostics = {})
      : diagnostics_(std::move(diagnostics)) {
    const auto text = windows_analysis_math_hlsl() +
        std::string(WindowsIccGpuTransform::hlsl_source()) +
        std::string(WindowsDisplayTransform::hlsl_source()) + kernels;
    auto v = gpu_.compile(text.c_str(), "vs", "vs_5_0");
    auto p = gpu_.compile(text.c_str(), "ps", "ps_5_0");
    auto b = gpu_.compile(text.c_str(), "blur", "cs_5_0");
    auto w = gpu_.compile(text.c_str(), "prepare_work", "cs_5_0");
    winrt::check_hresult(gpu_.device->CreateVertexShader(
        v->GetBufferPointer(), v->GetBufferSize(), nullptr, vertex_.put()));
    winrt::check_hresult(gpu_.device->CreatePixelShader(
        p->GetBufferPointer(), p->GetBufferSize(), nullptr, pixel_.put()));
    winrt::check_hresult(gpu_.device->CreateComputeShader(
        b->GetBufferPointer(), b->GetBufferSize(), nullptr, blur_.put()));
    winrt::check_hresult(gpu_.device->CreateComputeShader(
        w->GetBufferPointer(), w->GetBufferSize(), nullptr,
        work_shader_.put()));
  }

  Result<bool, Error> draw(const Input &input, const SourceView &view,
                           HWND hwnd) {
    using Out = Result<bool, Error>;
    if (!input.source || !valid_source_view(view))
      return Out::failure(presentation_error("invalid_source_view"));
    const auto size = input.source->size_px();
    if (size.width <= 0 || size.height <= 0 || size.width > 16384 ||
        size.height > 16384)
      return Out::failure(presentation_error("invalid_source_size"));
    if (hwnd && !IsWindow(hwnd))
      return Out::failure(presentation_error("surface_unavailable"));
    try {
      Constants c;
      c.sw = UINT(size.width);
      c.sh = UINT(size.height);
      c.tw = UINT(view.target_size.width);
      c.th = UINT(view.target_size.height);
      c.scale = float(view.scale);
      c.ox = float(view.offset_x);
      c.oy = float(view.offset_y);
      c.white = float(view.settings.reference_white_nits);
      c.space = UINT(view.settings.working_space);
      c.false_color = view.false_color;
      c.has_marks = bool(view.operation_overlay);
      c.mask_kind = view.mask.enabled
                        ? (view.mask.shape == MaskShape::rectangle ? 1u : 2u)
                        : 0u;
      c.mx = float(view.mask.bounds.x);
      c.my = float(view.mask.bounds.y);
      c.mw = float(view.mask.bounds.width);
      c.mh = float(view.mask.bounds.height);
      c.mask_dim = float(view.mask_outside_factor);
      const bool changed = source_owner_ != input.source;
      if (changed) {
        auto pixels =
            input.source->read_region({0, 0, size.width, size.height});
        if (!pixels)
          return Out::failure(pixels.error());
        if (pixels.value().size() != std::size_t(size.width) * size.height * 4)
          return Out::failure(presentation_error("invalid_source_length"));
        source_ = texture(size, DXGI_FORMAT_R32G32B32A32_FLOAT,
                          D3D11_BIND_SHADER_RESOURCE, pixels.value().data(),
                          UINT(size.width) * 16);
        source_view_ = gpu_.srv(source_);
        source_owner_ = input.source;
        work_settings_.reset();
        work_ = nullptr;
        work_view_ = nullptr;
        horizontal_ = nullptr;
        horizontal_view_ = nullptr;
      }
      if (view.false_color &&
          (!work_settings_ || *work_settings_ != view.settings)) {
        auto weights = gaussian_weights(view.settings.blur_sigma_px);
        if (weights.size() > c.weights.size())
          return Out::failure(presentation_error("invalid_blur_radius"));
        std::copy(weights.begin(), weights.end(), c.weights.begin());
        c.radius = UINT(weights.size() / 2);
        if (!work_ || changed) {
          work_ =
              texture(size, DXGI_FORMAT_R32G32B32A32_FLOAT,
                      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
          work_view_ = gpu_.srv(work_);
        }
        if (c.radius) {
          if (!horizontal_ || changed) {
            horizontal_ = texture(size, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                  D3D11_BIND_SHADER_RESOURCE |
                                      D3D11_BIND_UNORDERED_ACCESS);
            horizontal_view_ = gpu_.srv(horizontal_);
          }
          dispatch(blur_.get(), horizontal_, nullptr, c);
        }
        dispatch(work_shader_.get(), work_,
                 c.radius ? horizontal_view_.get() : nullptr, c);
        work_settings_ = view.settings;
      }
      if (marks_owner_ != view.operation_overlay) {
        marks_ = nullptr;
        marks_view_ = nullptr;
        marks_owner_ = view.operation_overlay;
        if (marks_owner_) {
          marks_ = texture(view.target_size, DXGI_FORMAT_R8G8B8A8_UNORM,
                           D3D11_BIND_SHADER_RESOURCE,
                           marks_owner_->rgba.data(), c.tw * 4);
          marks_view_ = gpu_.srv(marks_);
        }
      }
      prepare_target(view.target_size, hwnd);
      c.output_scrgb = hwnd ? 1.0F : 0.0F;
      if (hwnd) {
        const auto monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (monitor != monitor_) {
          // A failed target query must never reuse the previous monitor's ICC.
          display_transform_.reset();
          monitor_ = nullptr;
          auto displays = windows_enumerate_displays(diagnostics_.get());
          if (!displays) {
            record_diagnostic_stage(diagnostics_.get(),{}, {},"windows.color",
                "analysis.target_query","failure",{},&displays.error());
            return Out::failure(displays.error());
          }
          const auto found = std::find_if(
              displays.value().begin(), displays.value().end(),
              [&](const auto &d) {
                return d.monitor == reinterpret_cast<std::uintptr_t>(monitor);
              });
          if (found == displays.value().end()) {
            record_diagnostic_stage(diagnostics_.get(),{}, {},"windows.color",
                "analysis.target_query","failure",{{"reason","monitor_unavailable"}});
            return Out::failure(presentation_error("monitor_unavailable"));
          }
          record_diagnostic_stage(diagnostics_.get(),{}, {},"windows.color",
              "analysis.target_query","success",
              {{"displayId",std::to_string(found->snapshot.id.value)},
               {"returnedSpace",std::to_string(found->advanced_color_mode)}});
          auto transform = WindowsDisplayTransform::create(gpu_.device.get(), *found);
          if (!transform) {
            record_diagnostic_stage(diagnostics_.get(),{}, {},"windows.color",
                "analysis.display_transform","failure",
                {{"displayId",std::to_string(found->snapshot.id.value)}},
                &transform.error());
            return Out::failure(transform.error());
          }
          display_transform_ = std::move(transform.value());
          record_diagnostic_stage(diagnostics_.get(),{}, {},"windows.color",
              "analysis.display_transform","success",
              {{"displayId",std::to_string(found->snapshot.id.value)},
               {"returnedSpace",std::to_string(display_transform_->effective_mode())},
               {"scale",std::to_string(display_transform_->white_scale())},
               {"source",found->icc_profile?found->icc_profile->sha256:"none"}});
          monitor_ = monitor;
        }
        if (!display_transform_)
          return Out::failure(presentation_error("display_transform_unavailable"));
        c.white_scale = display_transform_->white_scale();
      }
      auto cb = gpu_.buffer(sizeof(c), D3D11_BIND_CONSTANT_BUFFER, 0, &c);
      ID3D11Buffer *buffers[]{cb.get()};
      ID3D11ShaderResourceView *views[]{source_view_.get(), work_view_.get(),
                                        marks_view_.get()};
      ID3D11RenderTargetView *targets[]{target_.get()};
      gpu_.context->IASetPrimitiveTopology(
          D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      gpu_.context->VSSetShader(vertex_.get(), nullptr, 0);
      gpu_.context->PSSetShader(pixel_.get(), nullptr, 0);
      gpu_.context->PSSetConstantBuffers(0, 1, buffers);
      if (hwnd)
        display_transform_->bind_ps(gpu_.context.get());
      gpu_.context->PSSetShaderResources(0, 3, views);
      gpu_.context->OMSetRenderTargets(1, targets, nullptr);
      const D3D11_VIEWPORT viewport{0, 0, float(c.tw), float(c.th), 0, 1};
      gpu_.context->RSSetViewports(1, &viewport);
      gpu_.context->Draw(3, 0);
      ID3D11ShaderResourceView *empty[3]{};
      gpu_.context->PSSetShaderResources(0, 3, empty);
      gpu_.context->OMSetRenderTargets(0, nullptr, nullptr);
      if (swapchain_) {
        // No unbounded GUI wait: the sole worker owns all immediate-context and
        // DXGI operations. The worker retains a busy frame until it succeeds,
        // is superseded, or is canceled; it never blocks GUI event delivery.
        winrt::check_hresult(swapchain_->Present(0, DXGI_PRESENT_DO_NOT_WAIT));
        winrt::check_hresult(composition_->Commit());
      }
      return Out::success(true);
    } catch (const winrt::hresult_error &e) {
      return Out::failure(
          windows_d3d_error("WindowsAnalysisPresenter", "draw", e.code()));
    }
  }

  Result<LinearSourceRef, Error> readback(PixelSize size) {
    using Out = Result<LinearSourceRef, Error>;
    try {
      D3D11_TEXTURE2D_DESC desc{};
      output_->GetDesc(&desc);
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      winrt::com_ptr<ID3D11Texture2D> staging;
      winrt::check_hresult(
          gpu_.device->CreateTexture2D(&desc, nullptr, staging.put()));
      gpu_.context->CopyResource(staging.get(), output_.get());
      LinearFloatPixels values(std::size_t(size.width) * size.height * 4);
      D3D11_MAPPED_SUBRESOURCE mapped{};
      winrt::check_hresult(
          gpu_.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
      for (int y = 0; y < size.height; ++y)
        std::memcpy(values.data() + std::size_t(y) * size.width * 4,
                    static_cast<const std::byte *>(mapped.pData) +
                        std::size_t(y) * mapped.RowPitch,
                    std::size_t(size.width) * 16);
      gpu_.context->Unmap(staging.get(), 0);
      return Out::success(
          std::make_shared<ReadbackSource>(size, std::move(values)));
    } catch (const winrt::hresult_error &e) {
      return Out::failure(
          windows_d3d_error("WindowsAnalysisPresenter", "readback", e.code()));
    }
  }

private:
  winrt::com_ptr<ID3D11Texture2D> texture(PixelSize size, DXGI_FORMAT format,
                                          UINT bind,
                                          const void *pixels = nullptr,
                                          UINT pitch = 0) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = UINT(size.width);
    d.Height = UINT(size.height);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.BindFlags = bind;
    d.Usage = pixels ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA data{pixels, pitch, 0};
    winrt::com_ptr<ID3D11Texture2D> result;
    winrt::check_hresult(gpu_.device->CreateTexture2D(
        &d, pixels ? &data : nullptr, result.put()));
    return result;
  }
  void dispatch(ID3D11ComputeShader *shader,
                const winrt::com_ptr<ID3D11Texture2D> &destination,
                ID3D11ShaderResourceView *second, const Constants &c) {
    auto cb = gpu_.buffer(sizeof(c), D3D11_BIND_CONSTANT_BUFFER, 0, &c);
    ID3D11Buffer *buffers[]{cb.get()};
    winrt::com_ptr<ID3D11UnorderedAccessView> uav;
    winrt::check_hresult(gpu_.device->CreateUnorderedAccessView(
        destination.get(), nullptr, uav.put()));
    ID3D11UnorderedAccessView *targets[]{uav.get()};
    ID3D11ShaderResourceView *views[]{source_view_.get(), second};
    gpu_.context->CSSetShader(shader, nullptr, 0);
    gpu_.context->CSSetConstantBuffers(0, 1, buffers);
    gpu_.context->CSSetShaderResources(0, 2, views);
    gpu_.context->CSSetUnorderedAccessViews(0, 1, targets, nullptr);
    gpu_.context->Dispatch((c.sw + 7) / 8, (c.sh + 7) / 8, 1);
    ID3D11UnorderedAccessView *no_target[]{nullptr};
    ID3D11ShaderResourceView *empty[2]{};
    gpu_.context->CSSetUnorderedAccessViews(0, 1, no_target, nullptr);
    gpu_.context->CSSetShaderResources(0, 2, empty);
  }
  void prepare_target(PixelSize size, HWND hwnd) {
    if (size == target_size_ && hwnd == hwnd_ && output_)
      return;
    target_ = nullptr;
    output_ = nullptr;
    if (hwnd != hwnd_) {
      visual_ = nullptr;
      composition_target_ = nullptr;
      composition_ = nullptr;
      swapchain_ = nullptr;
      monitor_ = nullptr;
    }
    if (hwnd) {
      if (swapchain_)
        winrt::check_hresult(
            swapchain_->ResizeBuffers(2, UINT(size.width), UINT(size.height),
                                      DXGI_FORMAT_R16G16B16A16_FLOAT, 0));
      else {
        winrt::com_ptr<IDXGIAdapter> adapter;
        winrt::check_hresult(
            gpu_.device.as<IDXGIDevice>()->GetAdapter(adapter.put()));
        winrt::com_ptr<IDXGIFactory2> factory;
        winrt::check_hresult(adapter->GetParent(winrt::guid_of<IDXGIFactory2>(),
                                                factory.put_void()));
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = UINT(size.width);
        d.Height = UINT(size.height);
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        winrt::check_hresult(factory->CreateSwapChainForComposition(
            gpu_.device.get(), &d, nullptr, swapchain_.put()));
        auto color = swapchain_.as<IDXGISwapChain3>();
        UINT flags{};
        const auto check_hr=color->CheckColorSpaceSupport(
            DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &flags);
        record_diagnostic_stage(diagnostics_.get(),{}, {},"windows.color","analysis.surface_check",
            SUCCEEDED(check_hr)?"success":"failure",{{"api","CheckColorSpaceSupport"},
            {"nativeCode",std::to_string(static_cast<std::uint32_t>(check_hr))}});
        winrt::check_hresult(check_hr);
        record_diagnostic_stage(diagnostics_.get(),{}, {},"windows.color","analysis.surface_support_flags","success",
            {{"nativeCode",std::to_string(flags)}});
        // SetColorSpace1 may succeed even when the pre-set support bit is
        // absent for the currently associated output.
        const auto set_hr=color->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
        record_diagnostic_stage(diagnostics_.get(),{}, {},"windows.color","analysis.surface_set",
            SUCCEEDED(set_hr)?"success":"failure",{{"api","SetColorSpace1"},
            {"requestedSpace","RGB_FULL_G10_NONE_P709"},
            {"nativeCode",std::to_string(static_cast<std::uint32_t>(set_hr))}});
        winrt::check_hresult(set_hr);
        DXGI_SWAP_CHAIN_DESC1 actual{};
        winrt::check_hresult(swapchain_->GetDesc1(&actual));
        if (actual.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
          winrt::throw_hresult(DXGI_ERROR_UNSUPPORTED);
        record_diagnostic_stage(diagnostics_.get(),{}, {},"windows.color",
            "analysis.surface_actual","success",
            {{"requestedFormat","R16G16B16A16_FLOAT"},
             {"requestedSpace","RGB_FULL_G10_NONE_P709"},
             {"format",std::to_string(actual.Format)},
             {"nativeCode","0"}});
        winrt::check_hresult(DCompositionCreateDevice(
            gpu_.device.as<IDXGIDevice>().get(),
            winrt::guid_of<IDCompositionDevice>(), composition_.put_void()));
        winrt::check_hresult(composition_->CreateTargetForHwnd(
            hwnd, TRUE, composition_target_.put()));
        winrt::check_hresult(composition_->CreateVisual(visual_.put()));
        winrt::check_hresult(visual_->SetContent(swapchain_.get()));
        winrt::check_hresult(composition_target_->SetRoot(visual_.get()));
      }
      winrt::check_hresult(swapchain_->GetBuffer(
          0, winrt::guid_of<ID3D11Texture2D>(), output_.put_void()));
    } else
      output_ = texture(size, DXGI_FORMAT_R32G32B32A32_FLOAT,
                        D3D11_BIND_RENDER_TARGET);
    winrt::check_hresult(gpu_.device->CreateRenderTargetView(
        output_.get(), nullptr, target_.put()));
    target_size_ = size;
    hwnd_ = hwnd;
  }
  WindowsD3DDevice gpu_;
  LinearSourceRef source_owner_;
  std::shared_ptr<const UiImage> marks_owner_;
  std::optional<Settings> work_settings_;
  winrt::com_ptr<ID3D11Texture2D> source_, work_, horizontal_, marks_, output_;
  winrt::com_ptr<ID3D11ShaderResourceView> source_view_, work_view_,
      horizontal_view_, marks_view_;
  winrt::com_ptr<ID3D11VertexShader> vertex_;
  winrt::com_ptr<ID3D11PixelShader> pixel_;
  winrt::com_ptr<ID3D11ComputeShader> blur_, work_shader_;
  winrt::com_ptr<ID3D11RenderTargetView> target_;
  winrt::com_ptr<IDXGISwapChain1> swapchain_;
  winrt::com_ptr<IDCompositionDevice> composition_;
  winrt::com_ptr<IDCompositionTarget> composition_target_;
  winrt::com_ptr<IDCompositionVisual> visual_;
  PixelSize target_size_{};
  HWND hwnd_{};
  HMONITOR monitor_{};
  std::shared_ptr<const WindowsDisplayTransform> display_transform_;
  std::shared_ptr<DiagnosticsPort> diagnostics_;
};

struct Delivery {
  struct Frame {
    Input input;
    SourceView view;
    HWND hwnd{};
    std::uint64_t generation{};
  };
  std::mutex mutex;
  std::condition_variable ready;
  // Error handlers only dispatch work; serialize revocation with delivery.
  // Recursive locking permits a handler to clear itself or release its owner.
  std::recursive_mutex callback_mutex;
  std::optional<Frame> pending;
  WindowsAnalysisPresentationStatus status;
  std::function<void(Error)> error;
  bool stopping{};
};
class Presenter final : public AnalysisPresenterPort {
public:
  explicit Presenter(std::shared_ptr<DiagnosticsPort> diagnostics)
      : state_(std::make_shared<Delivery>()) {
    // Worker owns its lifetime. Destruction only revokes delivery and wakes it;
    // neither GPU completion nor an OS Present blocks the GUI destructor.
    std::thread([state = state_, log = std::move(diagnostics)] {
      std::unique_ptr<Renderer> renderer;
      for (;;) {
        Delivery::Frame frame;
        {
          std::unique_lock lock(state->mutex);
          state->ready.wait(lock, [&] {
            return state->stopping || state->pending.has_value();
          });
          if (state->stopping)
            return;
          frame = std::move(*state->pending);
          state->pending.reset();
          state->status.pending = false;
          state->status.worker_active = true;
          ++state->status.submitted;
        }
        Result<bool, Error> result = Result<bool, Error>::failure(
            presentation_error("renderer_unavailable"));
        bool superseded = false;
        try {
          if (!renderer)
            renderer = std::make_unique<Renderer>(log);
          for (;;) {
            result = renderer->draw(frame.input, frame.view, frame.hwnd);
            if (result)
              break;
            const auto code = result.error().safe_context.find("hresult");
            if (code == result.error().safe_context.end() ||
                code->second != std::to_string(static_cast<std::uint32_t>(
                                    DXGI_ERROR_WAS_STILL_DRAWING)))
              break;
            {
              std::unique_lock lock(state->mutex);
              if (state->stopping)
                return;
              if (state->pending) {
                superseded = true;
                break;
              }
              state->ready.wait_for(lock, std::chrono::milliseconds(16), [&] {
                return state->stopping || state->pending.has_value();
              });
              if (state->stopping)
                return;
              if (state->pending) {
                superseded = true;
                break;
              }
            }
          }
        } catch (const winrt::hresult_error &e) {
          result = Result<bool, Error>::failure(windows_d3d_error(
              "WindowsAnalysisPresenter", "initialize", e.code()));
        } catch (const std::exception &) {
          result = Result<bool, Error>::failure(
              presentation_error("allocation_or_worker_failure"));
        }
        {
          std::lock_guard lock(state->mutex);
          state->status.worker_active = false;
          if (state->stopping)
            return;
          if (superseded) {
            ++state->status.dropped;
          } else if (result) {
            ++state->status.completed;
            state->status.completed_generation = frame.generation;
          } else {
            ++state->status.failed;
          }
        }
        if (!result && !superseded) {
          std::lock_guard delivery_lock(state->callback_mutex);
          std::function<void(Error)> callback;
          {
            std::lock_guard lock(state->mutex);
            if (!state->stopping)
              callback = state->error;
          }
          if (callback)
            callback(result.error());
        }
      }
    }).detach();
  }
  ~Presenter() override {
    std::lock_guard delivery_lock(state_->callback_mutex);
    {
      std::lock_guard lock(state_->mutex);
      state_->stopping = true;
      state_->pending.reset();
      state_->error = {};
      state_->status.pending = false;
    }
    state_->ready.notify_one();
  }
  Result<bool, Error> present(const Input &input, const SourceView &view,
                              std::uintptr_t native_surface) override {
    if (!native_surface || !input.source || !valid_source_view(view))
      return Result<bool, Error>::failure(
          presentation_error("invalid_source_view_or_surface"));
    {
      std::lock_guard lock(state_->mutex);
      if (state_->stopping)
        return Result<bool, Error>::failure(
            presentation_error("presenter_stopped"));
      if (state_->pending)
        ++state_->status.dropped;
      auto generation = ++state_->status.requested;
      state_->pending = Delivery::Frame{
          input, view, reinterpret_cast<HWND>(native_surface), generation};
      state_->status.pending = true;
    }
    state_->ready.notify_one();
    return Result<bool, Error>::success(true);
  }
  std::shared_ptr<Delivery> state_;
};
} // namespace

Result<std::shared_ptr<AnalysisPresenterPort>, Error>
make_windows_analysis_presenter(std::shared_ptr<DiagnosticsPort> diagnostics) {
  return Result<std::shared_ptr<AnalysisPresenterPort>, Error>::success(
      std::make_shared<Presenter>(std::move(diagnostics)));
}
WindowsAnalysisPresentationStatus
windows_analysis_presentation_status(const AnalysisPresenterPort &presenter) {
  const auto *native = dynamic_cast<const Presenter *>(&presenter);
  if (!native)
    return {};
  std::lock_guard lock(native->state_->mutex);
  return native->state_->status;
}
void set_windows_analysis_presenter_error_callback(
    AnalysisPresenterPort &presenter, std::function<void(Error)> callback) {
  if (auto *native = dynamic_cast<Presenter *>(&presenter)) {
    std::lock_guard delivery_lock(native->state_->callback_mutex);
    std::lock_guard lock(native->state_->mutex);
    native->state_->error = std::move(callback);
  }
}
Result<LinearSourceRef, Error>
windows_analysis_render_offscreen(const Input &input, const SourceView &view) {
  using Out = Result<LinearSourceRef, Error>;
  try {
    Renderer renderer;
    auto drawn = renderer.draw(input, view, nullptr);
    if (!drawn)
      return Out::failure(drawn.error());
    return renderer.readback(view.target_size);
  } catch (const winrt::hresult_error &e) {
    return Out::failure(windows_d3d_error("WindowsAnalysisPresenter",
                                          "offscreen_initialize", e.code()));
  }
}
} // namespace hdrshot
