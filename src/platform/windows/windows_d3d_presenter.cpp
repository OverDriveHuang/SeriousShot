#include "platform/windows/windows_d3d_presenter.hpp"
#include "platform/windows/windows_d3d.hpp"
#include <algorithm>
#include <cmath>
#include <dcomp.h>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace hdrshot {
namespace {
constexpr const char* source=R"HLSL(
Texture2D<float4> pixels:register(t0);
cbuffer Params:register(b0) {
  uint width; uint height; float white_scale; float outside_factor;
  int4 selection;
  float ui_white; int border_width; int2 padding;
};
struct Vertex { float4 pos:SV_Position; float2 uv:TEXCOORD; };
Vertex vs(uint id:SV_VertexID) {
  Vertex v; v.uv=float2((id<<1)&2,id&2);
  v.pos=float4(v.uv*float2(2,-2)+float2(-1,1),0,1); return v;
}
float4 ps(Vertex v):SV_Target {
  uint2 xy=min(uint2(v.uv*float2(width,height)),uint2(width-1,height-1));
  float3 p3=pixels.Load(int3(xy,0)).rgb;
  bool inside=selection.z>0 && selection.w>0 && int(xy.x)>=selection.x && int(xy.y)>=selection.y &&
      int(xy.x)<selection.x+selection.z && int(xy.y)<selection.y+selection.w;
  p3*=inside?1.0:outside_factor;
  int2 p=int2(xy);int2 end=selection.xy+selection.zw;
  int border=selection.z>0 && selection.w>0 ? border_width:0;
  bool outer=border>0 && all(p>=selection.xy-border) && all(p<end+border);
  bool inner=all(p>=selection.xy+border) && all(p<end-border);
  if(outer && !inner) p3=ui_white.xxx;
  float3 scrgb=float3(1.22494018*p3.r-0.22494018*p3.g,
      -0.04205695*p3.r+1.04205695*p3.g,
      -0.01963755*p3.r-0.07863605*p3.g+1.09827360*p3.b);
  return float4(scrgb*white_scale,1);
}
)HLSL";
Error cancelled() { return {ErrorCode::operation_cancelled,"WindowsD3DPreview",Retryability::never,{}}; }
}
struct WindowsD3DPreviewPresenter::Impl {
  WindowsD3DDevice gpu;
  winrt::com_ptr<IDXGISwapChain1> swapchain;
  winrt::com_ptr<IDCompositionDevice> composition;
  winrt::com_ptr<IDCompositionTarget> composition_target;
  winrt::com_ptr<IDCompositionVisual> visual;
  bool first_present=true;
  winrt::com_ptr<ID3D11RenderTargetView> render_target;
  winrt::com_ptr<ID3D11Texture2D> output;
  std::mutex drawing;
  winrt::com_ptr<ID3D11VertexShader> vertex;
  winrt::com_ptr<ID3D11PixelShader> pixel;
  winrt::com_ptr<ID3D11Texture2D> texture;
  winrt::com_ptr<ID3D11ShaderResourceView> texture_view;
  FrameId loaded_frame{};
  WindowsDisplayInfo display;
  std::mutex mutex;
  std::condition_variable ready;
  struct Work { PresentPreviewRequest request; Completion completion; };
  std::optional<Work> latest;
  std::set<std::pair<std::uint64_t,std::uint64_t>> cancellations;
  bool stopping{};
  std::thread worker;
  Impl(HWND hwnd,WindowsDisplayInfo info):display(std::move(info)) {
    if(display.snapshot.capture_size_px.width<=0 || display.snapshot.capture_size_px.height<=0 ||
        !WindowsColor::scrgb_to_edr_scale(display.source_white)) winrt::throw_hresult(E_INVALIDARG);
    auto vs=gpu.compile(source,"vs","vs_5_0"), ps=gpu.compile(source,"ps","ps_5_0");
    winrt::check_hresult(gpu.device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,vertex.put()));
    winrt::check_hresult(gpu.device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,pixel.put()));
    winrt::com_ptr<IDXGIAdapter> adapter;
    winrt::check_hresult(gpu.device.as<IDXGIDevice>()->GetAdapter(adapter.put()));
    winrt::com_ptr<IDXGIFactory2> factory;
    winrt::check_hresult(adapter->GetParent(winrt::guid_of<IDXGIFactory2>(),factory.put_void()));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width=display.snapshot.capture_size_px.width; desc.Height=display.snapshot.capture_size_px.height;
    desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; desc.SampleDesc.Count=1;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=2;
    desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
    if(hwnd) {
    // Compose the first FP16 frame while hidden; exposing the HWND must not
    // expose an empty GDI redirection bitmap or wait for an unoccluded Present.
    winrt::check_hresult(factory->CreateSwapChainForComposition(gpu.device.get(),&desc,nullptr,swapchain.put()));
    winrt::check_hresult(DCompositionCreateDevice(gpu.device.as<IDXGIDevice>().get(),
        winrt::guid_of<IDCompositionDevice>(),composition.put_void()));
    winrt::check_hresult(composition->CreateTargetForHwnd(hwnd,TRUE,composition_target.put()));
    winrt::check_hresult(composition->CreateVisual(visual.put()));
    winrt::check_hresult(visual->SetContent(swapchain.get()));
    winrt::check_hresult(composition_target->SetRoot(visual.get()));
    UINT supported=0;
    auto color=swapchain.as<IDXGISwapChain3>();
    winrt::check_hresult(color->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,&supported));
    if((supported&DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)==0) winrt::throw_hresult(DXGI_ERROR_UNSUPPORTED);
    winrt::check_hresult(color->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709));
    winrt::check_hresult(swapchain->GetBuffer(0,winrt::guid_of<ID3D11Texture2D>(),output.put_void()));
    } else {
      D3D11_TEXTURE2D_DESC texture_desc{};
      texture_desc.Width=desc.Width;texture_desc.Height=desc.Height;
      texture_desc.MipLevels=1;texture_desc.ArraySize=1;texture_desc.SampleDesc.Count=1;
      texture_desc.Format=desc.Format;texture_desc.BindFlags=D3D11_BIND_RENDER_TARGET;
      winrt::check_hresult(gpu.device->CreateTexture2D(&texture_desc,nullptr,output.put()));
    }
    winrt::check_hresult(gpu.device->CreateRenderTargetView(output.get(),nullptr,render_target.put()));
    worker=std::thread([this] { loop(); });
  }
  ~Impl() {
    std::optional<Work> abandoned;
    { const std::scoped_lock lock(mutex); stopping=true; abandoned=std::move(latest); latest.reset(); }
    if(abandoned) abandoned->completion(Result<PresentReceipt,Error>::failure(cancelled()));
    ready.notify_one(); if(worker.joinable()) worker.join();
  }
  void loop() {
    while(true) {
      std::optional<Work> work;
      { std::unique_lock lock(mutex); ready.wait(lock,[this] { return stopping || latest.has_value(); });
        if(stopping) return; work=std::move(latest); latest.reset(); }
      auto result=[&] { const std::scoped_lock lock(drawing);return draw(work->request); }();
      { const std::scoped_lock lock(mutex);
        if(stopping || cancellations.contains({work->request.model.session_id.value,work->request.operation_id.value}))
          result=Result<PresentReceipt,Error>::failure(cancelled()); }
      work->completion(std::move(result));
    }
  }
  Result<PresentReceipt,Error> draw(const PresentPreviewRequest& request) {
    const auto& model=request.model;
    if(!model.frozen_desktop || request.target_display_id!=display.snapshot.id ||
        model.display_generation!=model.frozen_desktop->display_generation ||
        !std::isfinite(model.overlay_style.outside_linear_dim_factor) ||
        model.overlay_style.outside_linear_dim_factor<0 || model.overlay_style.outside_linear_dim_factor>1 ||
        model.overlay_style.selection_border_width_px<0 || !std::isfinite(model.overlay_style.ui_white_edr) ||
        model.overlay_style.ui_white_edr<0)
      return Result<PresentReceipt,Error>::failure(windows_d3d_error("WindowsD3DPreview","invalid_frame_or_display"));
    const auto& segments=model.frozen_desktop->canonical_segments;
    const auto segment=std::find_if(segments.begin(),segments.end(),[this](const auto& s) { return s.display_id==display.snapshot.id; });
    if(segment==segments.end() || segment->encoding!=WindowsColor::linear_p3_encoding() || segment->size_px!=display.snapshot.capture_size_px ||
        segment->rgba_half.size()!=static_cast<std::size_t>(segment->size_px.width)*segment->size_px.height*4U)
      return Result<PresentReceipt,Error>::failure(windows_d3d_error("WindowsD3DPreview","invalid_linear_p3_source"));
    try {
      if(loaded_frame!=model.frozen_desktop->frame_id) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=segment->size_px.width;desc.Height=segment->size_px.height;desc.MipLevels=1;desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.SampleDesc.Count=1;
        desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{segment->rgba_half.data(),static_cast<UINT>(segment->size_px.width*8),0};
        texture_view=nullptr;texture=nullptr;
        winrt::check_hresult(gpu.device->CreateTexture2D(&desc,&data,texture.put()));
        texture_view=gpu.srv(texture);loaded_frame=model.frozen_desktop->frame_id;
      }
      const auto rect=preview_selection_rect(model);
      struct Params { UINT width,height;float white_scale,outside;int selection[4];float ui_white;int border;int padding[2]; };
      const Params params{static_cast<UINT>(segment->size_px.width),static_cast<UINT>(segment->size_px.height),
          1.0F/WindowsColor::scrgb_to_edr_scale(display.source_white).value(),model.overlay_style.outside_linear_dim_factor,
          {rect.x,rect.y,rect.width,rect.height},display.source_white.hdr_active?model.overlay_style.ui_white_edr:1.0F,
          model.overlay_style.selection_border_width_px,{}};
      const auto constants=gpu.buffer(sizeof(params),D3D11_BIND_CONSTANT_BUFFER,0,&params);
      ID3D11Buffer* cbs[]{constants.get()};
      ID3D11ShaderResourceView* views[]{texture_view.get()};
      ID3D11RenderTargetView* targets[]{render_target.get()};
      gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      gpu.context->VSSetShader(vertex.get(),nullptr,0);
      gpu.context->PSSetShader(pixel.get(),nullptr,0);
      gpu.context->PSSetConstantBuffers(0,1,cbs);
      gpu.context->PSSetShaderResources(0,1,views);
      gpu.context->OMSetRenderTargets(1,targets,nullptr);
      const D3D11_VIEWPORT viewport{0,0,static_cast<float>(params.width),static_cast<float>(params.height),0,1};
      gpu.context->RSSetViewports(1,&viewport);
      gpu.context->Draw(3,0);
      if(swapchain) winrt::check_hresult(swapchain->Present(1,0));
      if(composition && first_present) {
        winrt::check_hresult(composition->Commit());
        winrt::check_hresult(composition->WaitForCommitCompletion());
        first_present=false;
      }
      return Result<PresentReceipt,Error>::success({model.session_id,request.operation_id,model.frozen_desktop->frame_id,
          model.display_generation,model.selection.revision,model.annotation_document.revision,
          model.initial_highlight.revision});
    } catch(const winrt::hresult_error& e) {
      return Result<PresentReceipt,Error>::failure(windows_d3d_error("WindowsD3DPreview","present",e.code()));
    }
  }
};
WindowsD3DPreviewPresenter::WindowsD3DPreviewPresenter(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
WindowsD3DPreviewPresenter::~WindowsD3DPreviewPresenter()=default;
Result<std::vector<std::uint16_t>,Error> WindowsD3DPreviewPresenter::render_offscreen(const PresentPreviewRequest& request) {
  const std::scoped_lock lock(impl_->drawing);
  if(impl_->swapchain) return Result<std::vector<std::uint16_t>,Error>::failure(
      windows_d3d_error("WindowsD3DPreview","offscreen_surface_required"));
  const auto result=impl_->draw(request);
  if(!result) return Result<std::vector<std::uint16_t>,Error>::failure(result.error());
  try {
    D3D11_TEXTURE2D_DESC desc{};impl_->output->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> staging;
    winrt::check_hresult(impl_->gpu.device->CreateTexture2D(&desc,nullptr,staging.put()));
    impl_->gpu.context->CopyResource(staging.get(),impl_->output.get());
    std::vector<std::uint16_t> data(static_cast<std::size_t>(desc.Width)*desc.Height*4);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(impl_->gpu.context->Map(staging.get(),0,D3D11_MAP_READ,0,&mapped));
    for(UINT y=0;y<desc.Height;++y) std::memcpy(data.data()+static_cast<std::size_t>(y)*desc.Width*4,
        static_cast<const char*>(mapped.pData)+static_cast<std::size_t>(y)*mapped.RowPitch,desc.Width*8U);
    impl_->gpu.context->Unmap(staging.get(),0);
    return Result<std::vector<std::uint16_t>,Error>::success(std::move(data));
  } catch(const winrt::hresult_error& e) {
    return Result<std::vector<std::uint16_t>,Error>::failure(windows_d3d_error("WindowsD3DPreview","readback",e.code()));
  }
}
Result<std::shared_ptr<WindowsD3DPreviewPresenter>,Error> WindowsD3DPreviewPresenter::create(void* hwnd,WindowsDisplayInfo display) {
  try { return Result<std::shared_ptr<WindowsD3DPreviewPresenter>,Error>::success(
      std::shared_ptr<WindowsD3DPreviewPresenter>(new WindowsD3DPreviewPresenter(std::make_unique<Impl>(static_cast<HWND>(hwnd),std::move(display))))); }
  catch(const winrt::hresult_error& e) {
    return Result<std::shared_ptr<WindowsD3DPreviewPresenter>,Error>::failure(windows_d3d_error("WindowsD3DPreview","create",e.code()));
  }
}
void WindowsD3DPreviewPresenter::present(const PresentPreviewRequest& request,Completion completion) {
  std::optional<Impl::Work> displaced; bool reject=false;
  { const std::scoped_lock lock(impl_->mutex);
    reject=impl_->stopping || impl_->cancellations.contains({request.model.session_id.value,request.operation_id.value});
    if(!reject) { displaced=std::move(impl_->latest); impl_->latest=Impl::Work{request,std::move(completion)}; } }
  if(reject) completion(Result<PresentReceipt,Error>::failure(cancelled()));
  if(displaced) displaced->completion(Result<PresentReceipt,Error>::failure(cancelled()));
  impl_->ready.notify_one();
}
void WindowsD3DPreviewPresenter::cancel(SessionId session,OperationId operation) {
  const std::scoped_lock lock(impl_->mutex); impl_->cancellations.emplace(session.value,operation.value);
}
}  // namespace hdrshot
