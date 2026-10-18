#include "platform/windows/windows_d3d_presenter.hpp"
#include "platform/windows/windows_d3d.hpp"
#include "platform/windows/windows_display_transform.hpp"
#include "platform/windows/windows_analysis_shader_math.hpp"
#include <atomic>
#include "application/clean_content_cache.hpp"
#include "domain/annotation/annotation_compositing.hpp"
#include "ports/diagnostics_port.hpp"
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
StructuredBuffer<uint> clean_indices:register(t1);
StructuredBuffer<float4> clean_colors:register(t2);
Texture2D<float4> card_ui:register(t3);
cbuffer Params:register(b0) {
  uint width; uint height; float white_scale; float outside_factor;
  int4 selection;
  float ui_white; int border_width; int2 padding;
  int4 magnifier_rect; int4 magnifier_source;
};
struct Vertex { float4 pos:SV_Position; float2 uv:TEXCOORD; };
Vertex vs(uint id:SV_VertexID) {
  Vertex v; v.uv=float2((id<<1)&2,id&2);
  v.pos=float4(v.uv*float2(2,-2)+float2(-1,1),0,1); return v;
}
float4 ps(Vertex v):SV_Target {
  uint2 xy=min(uint2(v.uv*float2(width,height)),uint2(width-1,height-1));
  float3 p3=pixels.Load(int3(xy,0)).rgb;
  uint clean_index=clean_indices[xy.y*width+xy.x];
  if(clean_index>0) p3=clean_colors[clean_index-1].rgb;
  bool inside=selection.z>0 && selection.w>0 && int(xy.x)>=selection.x && int(xy.y)>=selection.y &&
      int(xy.x)<selection.x+selection.z && int(xy.y)<selection.y+selection.w;
  p3*=inside?1.0:outside_factor;
  int2 p=int2(xy);int2 end=selection.xy+selection.zw;
  int border=selection.z>0 && selection.w>0 ? border_width:0;
  bool outer=border>0 && all(p>=selection.xy-border) && all(p<end+border);
  bool inner=all(p>=selection.xy+border) && all(p<end-border);
  if(outer && !inner) p3=ui_white.xxx;
  int2 mp=int2(xy)-magnifier_rect.xy;
  if(magnifier_source.w!=0 && all(mp>=0) && all(mp<magnifier_rect.zw)) {
    int z=magnifier_source.z;int2 cell=mp/z;int2 center=magnifier_rect.zw/z/2;
    int2 q=magnifier_source.xy+cell-center;p3=0.025.xxx;
    if(all(q>=0) && q.x<int(width) && q.y<int(height)) p3=pixels.Load(int3(q,0)).rgb;
    int2 within=mp%z;
    bool box=all(cell==center) && (within.x==0 || within.y==0 || within.x==z-1 || within.y==z-1);
    bool cross=(mp.x==magnifier_rect.z/2 && abs(mp.y-magnifier_rect.w/2)>z) || (mp.y==magnifier_rect.w/2 && abs(mp.x-magnifier_rect.z/2)>z);
    // Match Metal: white stays at its original coordinates, with symmetric
    // black cross rails and a box outline entirely outside the source cell.
    int2 box_min=center*z-1,box_max=(center+1)*z;
    bool box_outline=all(mp>=box_min) && all(mp<=box_max) &&
        (mp.x==box_min.x || mp.x==box_max.x || mp.y==box_min.y || mp.y==box_max.y);
    bool cross_outline=(abs(mp.x-magnifier_rect.z/2)==1 && abs(mp.y-magnifier_rect.w/2)>z) ||
        (abs(mp.y-magnifier_rect.w/2)==1 && abs(mp.x-magnifier_rect.z/2)>z);
    if(box || cross)p3=ui_white.xxx;
    else if(box_outline || cross_outline)p3=0.0.xxx;
  }
  return float4(windows_present_p3(p3),1);
}

float4 ps_card(Vertex v):SV_Target {
  int2 xy=int2(v.pos.xy);
  int2 mp=xy-magnifier_rect.xy;
  float3 p3=0.025.xxx;
  if(all(mp>=0) && all(mp<magnifier_rect.zw)) {
    int z=magnifier_source.z;int2 cell=mp/z;int2 center=magnifier_rect.zw/z/2;
    int2 q=magnifier_source.xy+cell-center;p3=0.025.xxx;
    if(all(q>=0) && q.x<int(width) && q.y<int(height)) p3=pixels.Load(int3(q,0)).rgb;
    int2 within=mp%z;
    bool box=all(cell==center) && (within.x==0 || within.y==0 || within.x==z-1 || within.y==z-1);
    bool cross=(mp.x==magnifier_rect.z/2 && abs(mp.y-magnifier_rect.w/2)>z) || (mp.y==magnifier_rect.w/2 && abs(mp.x-magnifier_rect.z/2)>z);
    // Match Metal: white stays at its original coordinates, with symmetric
    // black cross rails and a box outline entirely outside the source cell.
    int2 box_min=center*z-1,box_max=(center+1)*z;
    bool box_outline=all(mp>=box_min) && all(mp<=box_max) &&
        (mp.x==box_min.x || mp.x==box_max.x || mp.y==box_min.y || mp.y==box_max.y);
    bool cross_outline=(abs(mp.x-magnifier_rect.z/2)==1 && abs(mp.y-magnifier_rect.w/2)>z) ||
        (abs(mp.y-magnifier_rect.w/2)==1 && abs(mp.x-magnifier_rect.z/2)>z);
    if(box || cross)p3=ui_white.xxx;
    else if(box_outline || cross_outline)p3=0.0.xxx;
  }
  float4 ui=card_ui.Load(int3(xy,0));
  uint3 code=uint3(round(ui.rgb*255));
  A3 linear_ui=ui_rgb_to_linear_p3((code.r<<16)|(code.g<<8)|code.b);
  p3=lerp(p3,float3(linear_ui.x,linear_ui.y,linear_ui.z),ui.a);
  return float4(windows_present_p3(p3),1);
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
  winrt::com_ptr<ID3D11Buffer> clean_index_buffer, clean_color_buffer;
  winrt::com_ptr<ID3D11ShaderResourceView> clean_index_view, clean_color_view;
  std::shared_ptr<const CleanContentSnapshot> loaded_clean_content;
  FrameId loaded_frame{};
  LinearSourceRef loaded_source;
  FrozenDesktopRef loaded_desktop;
  struct Counters {
    std::atomic<std::uint64_t> source_texture_uploads{}, clean_content_uploads{},
        background_draws{}, background_presents{}, card_draws{}, card_presents{}, card_ui_uploads{};
  } resource_counters;
  HWND card_hwnd{};
  struct CardSurface {
    PixelSize size{};
    winrt::com_ptr<IDXGISwapChain1> chain;
    winrt::com_ptr<IDCompositionTarget> target;
    winrt::com_ptr<IDCompositionVisual> visual;
    winrt::com_ptr<ID3D11Texture2D> output, ui;
    winrt::com_ptr<ID3D11RenderTargetView> render;
    winrt::com_ptr<ID3D11ShaderResourceView> ui_view;
    HANDLE available{};
    bool committed{}, capacity{};
    ~CardSurface(){if(available) CloseHandle(available);}
  };
  std::unique_ptr<CardSurface> card;
  winrt::com_ptr<ID3D11PixelShader> card_pixel;
  winrt::handle stopped{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
  winrt::handle changed{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
  winrt::handle background_available;
  bool background_capacity{};
  WindowsDisplayInfo display;
  std::shared_ptr<DiagnosticsPort> diagnostics;
  std::shared_ptr<const WindowsDisplayTransform> display_transform;
  std::optional<Error> initialization_error;
  std::mutex mutex;
  std::condition_variable ready;
  struct Work { PresentPreviewRequest request; Completion completion; };
  std::optional<Work> latest;
  struct CardWork { MagnifierCardFrame frame; MagnifierCardPresenterPort::Completion completion; };
  std::optional<CardWork> latest_card;
  std::uint64_t card_generation{};
  std::set<std::pair<std::uint64_t,std::uint64_t>> cancellations;
  bool stopping{};
  std::thread worker;
  Impl(HWND hwnd,WindowsDisplayInfo info,std::shared_ptr<DiagnosticsPort> log,HWND card_window)
      :card_hwnd(card_window),display(std::move(info)),diagnostics(std::move(log)) {
    if(!stopped || !changed) throw WindowsSyntheticFailure{windows_d3d_error("WindowsD3DPreview","CreateEventW",HRESULT_FROM_WIN32(GetLastError()))};
    if(display.snapshot.capture_size_px.width<=0 || display.snapshot.capture_size_px.height<=0)
      throw WindowsSyntheticFailure{windows_d3d_error("WindowsD3DPreview", "invalid_capture_size")};
    auto transform=WindowsDisplayTransform::create(gpu.device.get(),display);
    if(!transform) {
      initialization_error=transform.error();
      record_diagnostic_stage(diagnostics.get(),{}, {},"windows.color","overlay.display_transform","failure",
          {{"displayId",std::to_string(display.snapshot.id.value)}},&*initialization_error);
      return;
    }
    display_transform=std::move(transform.value());
    record_diagnostic_stage(diagnostics.get(),{}, {},"windows.color","overlay.display_transform","success",
        {{"displayId",std::to_string(display.snapshot.id.value)},
         {"returnedSpace",std::to_string(display_transform->effective_mode())},
         {"scale",std::to_string(display_transform->white_scale())},
         {"source",display.icc_profile?display.icc_profile->sha256:"none"}});
    const std::string shader=windows_analysis_math_hlsl()+std::string(WindowsIccGpuTransform::hlsl_source())+
        std::string(WindowsDisplayTransform::hlsl_source())+source;
    auto vs=gpu.compile(shader.c_str(),"vs","vs_5_0"), ps=gpu.compile(shader.c_str(),"ps","ps_5_0");
    winrt::check_hresult(gpu.device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,vertex.put()));
    winrt::check_hresult(gpu.device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,pixel.put()));
    auto cs=gpu.compile(shader.c_str(),"ps_card","ps_5_0");
    winrt::check_hresult(gpu.device->CreatePixelShader(cs->GetBufferPointer(),cs->GetBufferSize(),nullptr,card_pixel.put()));
    winrt::com_ptr<IDXGIAdapter> adapter;
    winrt::check_hresult(gpu.device.as<IDXGIDevice>()->GetAdapter(adapter.put()));
    winrt::com_ptr<IDXGIFactory2> factory;
    winrt::check_hresult(adapter->GetParent(winrt::guid_of<IDXGIFactory2>(),factory.put_void()));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width=display.snapshot.capture_size_px.width; desc.Height=display.snapshot.capture_size_px.height;
    desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; desc.SampleDesc.Count=1;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=2;
    desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
    desc.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if(hwnd) {
    // Compose the first FP16 frame while hidden; exposing the HWND must not
    // expose an empty GDI redirection bitmap or wait for an unoccluded Present.
    winrt::check_hresult(factory->CreateSwapChainForComposition(gpu.device.get(),&desc,nullptr,swapchain.put()));
    auto bounded=swapchain.as<IDXGISwapChain2>();
    winrt::check_hresult(bounded->SetMaximumFrameLatency(1));
    background_available.attach(bounded->GetFrameLatencyWaitableObject());
    if(!background_available) throw WindowsSyntheticFailure{windows_d3d_error("WindowsD3DPreview","missing_frame_latency_handle")};
    winrt::check_hresult(DCompositionCreateDevice(gpu.device.as<IDXGIDevice>().get(),
        winrt::guid_of<IDCompositionDevice>(),composition.put_void()));
    winrt::check_hresult(composition->CreateTargetForHwnd(hwnd,TRUE,composition_target.put()));
    winrt::check_hresult(composition->CreateVisual(visual.put()));
    winrt::check_hresult(visual->SetContent(swapchain.get()));
    winrt::check_hresult(composition_target->SetRoot(visual.get()));
    UINT supported=0;
    auto color=swapchain.as<IDXGISwapChain3>();
    const auto check_hr=color->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,&supported);
    record_diagnostic_stage(diagnostics.get(),{}, {},"windows.color","overlay.surface_check",
        SUCCEEDED(check_hr)?"success":"failure",{{"api","CheckColorSpaceSupport"},
        {"nativeCode",std::to_string(static_cast<std::uint32_t>(check_hr))}});
    winrt::check_hresult(check_hr);
    record_diagnostic_stage(diagnostics.get(),{}, {},"windows.color","overlay.surface_support_flags","success",
        {{"nativeCode",std::to_string(supported)}});
    // The pre-set PRESENT flag is advisory for a different current output;
    // SetColorSpace1 is authoritative for this FP16/G10 composition surface.
    const auto set_hr=color->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
    record_diagnostic_stage(diagnostics.get(),{}, {},"windows.color","overlay.surface_set",
        SUCCEEDED(set_hr)?"success":"failure",{{"api","SetColorSpace1"},
        {"requestedSpace","RGB_FULL_G10_NONE_P709"},
        {"nativeCode",std::to_string(static_cast<std::uint32_t>(set_hr))}});
    winrt::check_hresult(set_hr);
    DXGI_SWAP_CHAIN_DESC1 actual{};
    winrt::check_hresult(swapchain->GetDesc1(&actual));
    if(actual.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT)
      throw WindowsSyntheticFailure{windows_d3d_error("WindowsD3DPreview", "unsupported_surface_format")};
    record_diagnostic_stage(diagnostics.get(),{}, {},"windows.color","overlay.surface_actual","success",
        {{"requestedFormat","R16G16B16A16_FLOAT"},{"requestedSpace","RGB_FULL_G10_NONE_P709"},
         {"format",std::to_string(actual.Format)},
         {"nativeCode","0"}});
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
    std::optional<CardWork> abandoned_card;
    { const std::scoped_lock lock(mutex); stopping=true;
      abandoned=std::move(latest); latest.reset();
      abandoned_card=std::move(latest_card); latest_card.reset(); }
    SetEvent(stopped.get()); ready.notify_one();
    if(abandoned) abandoned->completion(Result<PresentReceipt,Error>::failure(cancelled()));
    if(abandoned_card) abandoned_card->completion(Result<std::uint64_t,Error>::failure(cancelled()));
    if(worker.joinable()) worker.join();
  }
  // 0=closed, 1=capacity acquired, 2=busy/new work. Busy work is
  // requeued and the other surface gets a turn, including while occluded.
  int await_surface(HANDLE available) {
    if(!available) return WaitForSingleObject(stopped.get(),0)==WAIT_OBJECT_0?0:1;
    HANDLE objects[]{stopped.get(),available,changed.get()};
    const auto wait=WaitForMultipleObjects(3,objects,FALSE,20);
    if(wait==WAIT_OBJECT_0) return 0;
    if(wait==WAIT_OBJECT_0+1) return 1;
    if(wait==WAIT_TIMEOUT || wait==WAIT_OBJECT_0+2) return 2;
    throw WindowsSyntheticFailure{windows_d3d_error("WindowsD3DPreview","WaitForMultipleObjects",HRESULT_FROM_WIN32(GetLastError()))};
  }
  void loop() {
    bool prefer_card=false;
    while(true) {
      std::optional<Work> work;
      std::optional<CardWork> card_work;
      { std::unique_lock lock(mutex);
        ready.wait(lock,[this] { return stopping || latest || latest_card; });
        if(stopping) return;
        if(latest && (!latest_card || !prefer_card)) {
          work=std::move(latest);latest.reset();prefer_card=true;
        } else {card_work=std::move(latest_card);latest_card.reset();prefer_card=false;}
      }
      if(work) {
        bool requeued=false;
        auto result=[&] {
          try {
            const int capacity=background_capacity?1:await_surface(background_available.get());
            if(capacity==1) background_capacity=true;
            if(capacity==0) return Result<PresentReceipt,Error>::failure(cancelled());
            if(capacity==2) {
              std::scoped_lock lock(mutex);
              if(!stopping && !latest){latest=std::move(work);work.reset();requeued=true;}
              return Result<PresentReceipt,Error>::failure(cancelled());
            }
            std::optional<Work> old;
            {std::scoped_lock lock(mutex);if(latest){old=std::move(work);work=std::move(latest);latest.reset();}}
            if(old) old->completion(Result<PresentReceipt,Error>::failure(cancelled()));
            const std::scoped_lock lock(drawing);return draw(work->request);
          } catch(const WindowsSyntheticFailure& e){return Result<PresentReceipt,Error>::failure(e.error);}
        }();
        if(requeued) continue;
        { const std::scoped_lock lock(mutex);
          if(stopping || cancellations.contains({work->request.model.session_id.value,work->request.operation_id.value}))
            result=Result<PresentReceipt,Error>::failure(cancelled()); }
        work->completion(std::move(result));
      } else {
        bool requeued=false;
        auto result=[&] {
          try {
            {std::scoped_lock lock(mutex);if(stopping || card_work->frame.generation!=card_generation)
                return Result<std::uint64_t,Error>::failure(cancelled());}
            {const std::scoped_lock lock(drawing);ensure_card(card_work->frame.size_px);}
            const int capacity=card->capacity?1:await_surface(card->available);
            if(capacity==1) card->capacity=true;
            if(capacity==0) return Result<std::uint64_t,Error>::failure(cancelled());
            if(capacity==2) {
              std::scoped_lock lock(mutex);
              if(!stopping && !latest_card && card_work->frame.generation==card_generation) {
                latest_card=std::move(card_work);card_work.reset();requeued=true;
              }
              return Result<std::uint64_t,Error>::failure(cancelled());
            }
            std::optional<CardWork> old;bool invalid;
            {std::scoped_lock lock(mutex);
              if(latest_card && latest_card->frame.size_px==card_work->frame.size_px) {
                old=std::move(card_work);card_work=std::move(latest_card);latest_card.reset();
              }
              invalid=stopping || card_work->frame.generation!=card_generation;
            }
            if(old) old->completion(Result<std::uint64_t,Error>::failure(cancelled()));
            if(invalid) return Result<std::uint64_t,Error>::failure(cancelled());
            const std::scoped_lock lock(drawing);return draw_card(card_work->frame);
          } catch(const WindowsSyntheticFailure& e){return Result<std::uint64_t,Error>::failure(e.error);}
            catch(const winrt::hresult_error& e){return Result<std::uint64_t,Error>::failure(windows_d3d_error("WindowsD3DPreview","card",e.code()));}
        }();
        if(requeued) continue;
        {std::scoped_lock lock(mutex);if(stopping || card_work->frame.generation!=card_generation)
            result=Result<std::uint64_t,Error>::failure(cancelled());}
        card_work->completion(std::move(result));
      }
    }
  }
  void ensure_card(PixelSize size) {
    if(size.width<=0 || size.height<=0 || size.width>16384 || size.height>16384)
      throw WindowsSyntheticFailure{windows_d3d_error("WindowsD3DPreview","invalid_card_size")};
    if(card && card->size==size) return;
    if(!card) card=std::make_unique<CardSurface>();
    card->capacity=false;
    ID3D11RenderTargetView* empty[]{nullptr};gpu.context->OMSetRenderTargets(1,empty,nullptr);
    ID3D11ShaderResourceView* no_ui[]{nullptr};gpu.context->PSSetShaderResources(3,1,no_ui);
    card->render=nullptr;card->output=nullptr;card->ui_view=nullptr;card->ui=nullptr;
    if(card_hwnd) {
      if(card->chain) {
        winrt::check_hresult(card->chain->ResizeBuffers(2,size.width,size.height,
            DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT));
      } else {
        winrt::com_ptr<IDXGIAdapter> adapter;
        winrt::check_hresult(gpu.device.as<IDXGIDevice>()->GetAdapter(adapter.put()));
        winrt::com_ptr<IDXGIFactory2> factory;
        winrt::check_hresult(adapter->GetParent(winrt::guid_of<IDXGIFactory2>(),factory.put_void()));
        DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=size.width;desc.Height=size.height;
        desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.SampleDesc.Count=1;
        desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;
        desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
        desc.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        winrt::check_hresult(factory->CreateSwapChainForComposition(gpu.device.get(),&desc,nullptr,card->chain.put()));
        auto bounded=card->chain.as<IDXGISwapChain2>();
        winrt::check_hresult(bounded->SetMaximumFrameLatency(1));
        card->available=bounded->GetFrameLatencyWaitableObject();
        if(!card->available) throw WindowsSyntheticFailure{windows_d3d_error("WindowsD3DPreview","missing_card_latency_handle")};
        winrt::check_hresult(card->chain.as<IDXGISwapChain3>()->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709));
        winrt::check_hresult(composition->CreateTargetForHwnd(card_hwnd,TRUE,card->target.put()));
        winrt::check_hresult(composition->CreateVisual(card->visual.put()));
        winrt::check_hresult(card->visual->SetContent(card->chain.get()));
        winrt::check_hresult(card->target->SetRoot(card->visual.get()));
      }
      winrt::check_hresult(card->chain->GetBuffer(0,winrt::guid_of<ID3D11Texture2D>(),card->output.put_void()));
    } else {
      D3D11_TEXTURE2D_DESC d{};d.Width=size.width;d.Height=size.height;
      d.MipLevels=1;d.ArraySize=1;d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
      d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_RENDER_TARGET;
      winrt::check_hresult(gpu.device->CreateTexture2D(&d,nullptr,card->output.put()));
    }
    winrt::check_hresult(gpu.device->CreateRenderTargetView(card->output.get(),nullptr,card->render.put()));
    D3D11_TEXTURE2D_DESC ui{};ui.Width=size.width;ui.Height=size.height;
    ui.MipLevels=1;ui.ArraySize=1;ui.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    ui.SampleDesc.Count=1;ui.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    winrt::check_hresult(gpu.device->CreateTexture2D(&ui,nullptr,card->ui.put()));
    card->ui_view=gpu.srv(card->ui);card->size=size;
  }
  Result<std::uint64_t,Error> draw_card(const MagnifierCardFrame& frame) {
    using Out=Result<std::uint64_t,Error>;
    auto local_image=frame.image;local_image.center={0,0};
    if(!valid_magnifier_preview(local_image,frame.size_px) || !frame.source || frame.display_id!=display.snapshot.id ||
        frame.display_generation!=frame.source->display_generation ||
        frame.ui_rgba.size()!=std::size_t(frame.size_px.width)*frame.size_px.height*4 ||
        !frame.image.visible || frame.image.zoom<1 || frame.image.viewport.empty() ||
        frame.image.viewport.x<0 || frame.image.viewport.y<0 ||
        frame.image.viewport.right()>frame.size_px.width || frame.image.viewport.bottom()>frame.size_px.height)
      return Out::failure(windows_d3d_error("WindowsD3DPreview","invalid_card_frame"));
    const auto found=std::find_if(frame.source->canonical_segments.begin(),frame.source->canonical_segments.end(),
        [&](const auto& s){return s.display_id==frame.display_id;});
    if(found==frame.source->canonical_segments.end() || found->encoding!=WindowsColor::linear_p3_encoding() ||
        found->size_px!=display.snapshot.capture_size_px || frame.image.center.x<0 || frame.image.center.y<0 ||
        frame.image.center.x>=found->size_px.width || frame.image.center.y>=found->size_px.height ||
        (found->linear_source ? found->linear_source->size_px()!=found->size_px :
          found->rgba_half.size()!=std::size_t(found->size_px.width)*found->size_px.height*4))
      return Out::failure(windows_d3d_error("WindowsD3DPreview","invalid_card_source"));
    try {
      ensure_card(frame.size_px);
      auto prepared=prepare_source(frame.source,*found);if(!prepared)return Out::failure(prepared.error());
      gpu.context->UpdateSubresource(card->ui.get(),0,nullptr,frame.ui_rgba.data(),frame.size_px.width*4,0);
      ++resource_counters.card_ui_uploads;
      struct Params {UINT width,height;float white_scale,outside;int selection[4];float ui_white;int border;int padding[2];int magnifier_rect[4];int magnifier_source[4];};
      const auto& image=frame.image;
      const Params params{UINT(found->size_px.width),UINT(found->size_px.height),display_transform->white_scale(),1,{},
          display.advanced_color_mode==2?2.03F:1.F,0,{},
          {image.viewport.x,image.viewport.y,image.viewport.width,image.viewport.height},
          {image.center.x,image.center.y,image.zoom,1}};
      auto constants=gpu.buffer(sizeof(params),D3D11_BIND_CONSTANT_BUFFER,0,&params);
      ID3D11Buffer* cbs[]{constants.get()};
      ID3D11ShaderResourceView* views[]{texture_view.get(),nullptr,nullptr,card->ui_view.get()};
      ID3D11RenderTargetView* target[]{card->render.get()};
      gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      gpu.context->VSSetShader(vertex.get(),nullptr,0);gpu.context->PSSetShader(card_pixel.get(),nullptr,0);
      gpu.context->PSSetConstantBuffers(0,1,cbs);display_transform->bind_ps(gpu.context.get());
      gpu.context->PSSetShaderResources(0,4,views);gpu.context->OMSetRenderTargets(1,target,nullptr);
      const D3D11_VIEWPORT viewport{0,0,float(frame.size_px.width),float(frame.size_px.height),0,1};
      gpu.context->RSSetViewports(1,&viewport);gpu.context->Draw(3,0);++resource_counters.card_draws;
      if(card->chain){card->capacity=false;winrt::check_hresult(card->chain->Present(1,0));++resource_counters.card_presents;}
      if(card_hwnd && !card->committed) {
        winrt::check_hresult(composition->Commit());
        winrt::check_hresult(composition->WaitForCommitCompletion());card->committed=true;
      }
      return Out::success(frame.revision);
    }catch(const WindowsSyntheticFailure& e){return Out::failure(e.error);}
     catch(const winrt::hresult_error& e){return Out::failure(windows_d3d_error("WindowsD3DPreview","draw_card",e.code()));}
  }
  Result<bool,Error> prepare_source(FrozenDesktopRef frozen,const CanonicalFrameSegment& segment) {
      if(loaded_desktop!=frozen || loaded_frame!=frozen->frame_id || loaded_source!=segment.linear_source) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=segment.size_px.width;desc.Height=segment.size_px.height;desc.MipLevels=1;desc.ArraySize=1;
        desc.Format=segment.linear_source ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count=1;
        desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        LinearFloatPixels native_pixels;
        if (segment.linear_source) {
          auto read = segment.linear_source->read_region({0,0,segment.size_px.width,segment.size_px.height});
          if (!read) return Result<bool,Error>::failure(read.error());
          native_pixels = std::move(read.value());
          if(native_pixels.size()!=std::size_t(segment.size_px.width)*segment.size_px.height*4)
            return Result<bool,Error>::failure(windows_d3d_error("WindowsD3DPreview","invalid_source_readback_length"));
        }
        const D3D11_SUBRESOURCE_DATA data{segment.linear_source ?
            static_cast<const void*>(native_pixels.data()) : static_cast<const void*>(segment.rgba_half.data()),
            static_cast<UINT>(segment.size_px.width*(segment.linear_source ? 16 : 8)),0};
        texture_view=nullptr;texture=nullptr;
        winrt::check_hresult(gpu.device->CreateTexture2D(&desc,&data,texture.put()));
        texture_view=gpu.srv(texture);loaded_frame=frozen->frame_id;
        loaded_source=segment.linear_source;loaded_desktop=frozen;
        ++resource_counters.source_texture_uploads;
      }
    return Result<bool,Error>::success(true);
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
        (segment->linear_source ?
          (segment->linear_source->size_px()!=segment->size_px || !segment->rgba_half.empty()) :
          segment->rgba_half.size()!=static_cast<std::size_t>(segment->size_px.width)*segment->size_px.height*4U))
      return Result<PresentReceipt,Error>::failure(windows_d3d_error("WindowsD3DPreview","invalid_linear_p3_source"));
    if(!valid_magnifier_preview(model.magnifier,segment->size_px))
      return Result<PresentReceipt,Error>::failure(windows_d3d_error("WindowsD3DPreview","invalid_magnifier"));
    if (model.clean_content && (model.clean_content->source != model.frozen_desktop ||
        model.clean_content->display_id != display.snapshot.id))
      return Result<PresentReceipt,Error>::failure(windows_d3d_error("WindowsD3DPreview","stale_clean_content"));
    try {
      auto prepared_source=prepare_source(model.frozen_desktop,*segment);
      if(!prepared_source) return Result<PresentReceipt,Error>::failure(prepared_source.error());
      if (loaded_clean_content != model.clean_content || !clean_index_view || !clean_color_view) {
        const auto count = static_cast<std::size_t>(segment->size_px.width) * segment->size_px.height;
        AnnotationGpuUpload upload;
        if (model.clean_content && !model.clean_content->owned.empty()) {
          auto plan = model.clean_content->roi_plan(
              {0,0,segment->size_px.width,segment->size_px.height}, model.annotation_document.revision);
          if (!plan) return Result<PresentReceipt,Error>::failure(plan.error());
          auto cpu = materialize_annotation_plan(plan.value());
          if (!cpu) return Result<PresentReceipt,Error>::failure(cpu.error());
          auto prepared = prepare_annotation_gpu_upload(cpu.value());
          if (!prepared) return Result<PresentReceipt,Error>::failure(prepared.error());
          upload = std::move(prepared.value());
        } else {
          upload.indices.assign(count, 0U);
          upload.samples.push_back({0,0,0,0});
        }
        clean_index_view = nullptr; clean_color_view = nullptr;
        clean_index_buffer = gpu.buffer(static_cast<UINT>(upload.indices.size()*sizeof(UINT)),
            D3D11_BIND_SHADER_RESOURCE, sizeof(UINT), upload.indices.data());
        clean_color_buffer = gpu.buffer(static_cast<UINT>(upload.samples.size()*sizeof(std::array<float,4>)),
            D3D11_BIND_SHADER_RESOURCE, sizeof(std::array<float,4>), upload.samples.data());
        clean_index_view = gpu.srv(clean_index_buffer);
        clean_color_view = gpu.srv(clean_color_buffer);
        loaded_clean_content = model.clean_content;
        ++resource_counters.clean_content_uploads;
      }
      const auto rect=preview_selection_rect(model);
      struct Params { UINT width,height;float white_scale,outside;int selection[4];float ui_white;int border;int padding[2]; int magnifier_rect[4]; int magnifier_source[4]; };
      const Params params{static_cast<UINT>(segment->size_px.width),static_cast<UINT>(segment->size_px.height),
          display_transform->white_scale(),model.overlay_style.outside_linear_dim_factor,
          {rect.x,rect.y,rect.width,rect.height},display.advanced_color_mode==2?model.overlay_style.ui_white_edr:1.0F,
          model.overlay_style.selection_border_width_px,{},
          {model.magnifier.viewport.x,model.magnifier.viewport.y,model.magnifier.viewport.width,model.magnifier.viewport.height},
          {model.magnifier.center.x,model.magnifier.center.y,model.magnifier.zoom,model.magnifier.visible?1:0}};
      const auto constants=gpu.buffer(sizeof(params),D3D11_BIND_CONSTANT_BUFFER,0,&params);
      ID3D11Buffer* cbs[]{constants.get()};
      ID3D11ShaderResourceView* views[]{texture_view.get(),clean_index_view.get(),clean_color_view.get()};
      ID3D11RenderTargetView* targets[]{render_target.get()};
      gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      gpu.context->VSSetShader(vertex.get(),nullptr,0);
      gpu.context->PSSetShader(pixel.get(),nullptr,0);
      gpu.context->PSSetConstantBuffers(0,1,cbs);
      display_transform->bind_ps(gpu.context.get());
      gpu.context->PSSetShaderResources(0,3,views);
      gpu.context->OMSetRenderTargets(1,targets,nullptr);
      const D3D11_VIEWPORT viewport{0,0,static_cast<float>(params.width),static_cast<float>(params.height),0,1};
      gpu.context->RSSetViewports(1,&viewport);
      gpu.context->Draw(3,0); ++resource_counters.background_draws;
      if(swapchain) {background_capacity=false;winrt::check_hresult(swapchain->Present(1,0));++resource_counters.background_presents;}
      if(composition && first_present) {
        winrt::check_hresult(composition->Commit());
        winrt::check_hresult(composition->WaitForCommitCompletion());
        first_present=false;
      }
      return Result<PresentReceipt,Error>::success({model.session_id,request.operation_id,model.frozen_desktop->frame_id,
          model.display_generation,model.selection.revision,model.annotation_document.revision,
          model.initial_highlight.revision});
    } catch(const WindowsSyntheticFailure& e) {
      return Result<PresentReceipt,Error>::failure(e.error);
    } catch(const winrt::hresult_error& e) {
      return Result<PresentReceipt,Error>::failure(windows_d3d_error("WindowsD3DPreview","present",e.code()));
    }
  }
};
WindowsD3DPreviewPresenter::WindowsD3DPreviewPresenter(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
WindowsD3DPreviewPresenter::~WindowsD3DPreviewPresenter()=default;
WindowsPreviewResourceCounters WindowsD3DPreviewPresenter::resource_counters() const {
  const auto& c=impl_->resource_counters;
  return {c.source_texture_uploads.load(),c.clean_content_uploads.load(),
      c.background_draws.load(),c.background_presents.load(),c.card_draws.load(),
      c.card_presents.load(),c.card_ui_uploads.load()};
}
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
Result<std::vector<std::uint16_t>,Error> WindowsD3DPreviewPresenter::render_card_offscreen(const MagnifierCardFrame& frame) {
  const std::scoped_lock lock(impl_->drawing);
  auto result=impl_->draw_card(frame);
  if(!result) return Result<std::vector<std::uint16_t>,Error>::failure(result.error());
  try {
    D3D11_TEXTURE2D_DESC desc{};impl_->card->output->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
    winrt::com_ptr<ID3D11Texture2D> staging;
    winrt::check_hresult(impl_->gpu.device->CreateTexture2D(&desc,nullptr,staging.put()));
    impl_->gpu.context->CopyResource(staging.get(),impl_->card->output.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(impl_->gpu.context->Map(staging.get(),0,D3D11_MAP_READ,0,&mapped));
    std::vector<std::uint16_t> data(std::size_t(desc.Width)*desc.Height*4);
    for(UINT y=0;y<desc.Height;++y) std::memcpy(data.data()+std::size_t(y)*desc.Width*4,
        static_cast<const char*>(mapped.pData)+std::size_t(y)*mapped.RowPitch,desc.Width*8U);
    impl_->gpu.context->Unmap(staging.get(),0);
    return Result<std::vector<std::uint16_t>,Error>::success(std::move(data));
  }catch(const winrt::hresult_error& e){return Result<std::vector<std::uint16_t>,Error>::failure(windows_d3d_error("WindowsD3DPreview","readback_card",e.code()));}
}
Result<std::shared_ptr<WindowsD3DPreviewPresenter>,Error> WindowsD3DPreviewPresenter::create(
    void* hwnd,WindowsDisplayInfo display,std::shared_ptr<DiagnosticsPort> diagnostics,void* card_hwnd) {
  try {
    auto impl=std::make_unique<Impl>(static_cast<HWND>(hwnd),std::move(display),std::move(diagnostics),static_cast<HWND>(card_hwnd));
    if(impl->initialization_error)
      return Result<std::shared_ptr<WindowsD3DPreviewPresenter>,Error>::failure(*impl->initialization_error);
    return Result<std::shared_ptr<WindowsD3DPreviewPresenter>,Error>::success(
      std::shared_ptr<WindowsD3DPreviewPresenter>(new WindowsD3DPreviewPresenter(std::move(impl)))); }
  catch(const WindowsSyntheticFailure& e) {
    return Result<std::shared_ptr<WindowsD3DPreviewPresenter>,Error>::failure(e.error);
  }
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
  SetEvent(impl_->changed.get());
  impl_->ready.notify_one();
}
void WindowsD3DPreviewPresenter::present_card(MagnifierCardFrame frame,MagnifierCardPresenterPort::Completion completion) {
  std::optional<Impl::CardWork> displaced;bool reject;
  {std::scoped_lock lock(impl_->mutex);
    reject=impl_->stopping || frame.generation!=impl_->card_generation;
    if(!reject){displaced=std::move(impl_->latest_card);impl_->latest_card=Impl::CardWork{std::move(frame),std::move(completion)};}}
  if(reject) completion(Result<std::uint64_t,Error>::failure(cancelled()));
  if(displaced) displaced->completion(Result<std::uint64_t,Error>::failure(cancelled()));
  SetEvent(impl_->changed.get());
  impl_->ready.notify_one();
}
void WindowsD3DPreviewPresenter::invalidate_card(std::uint64_t generation) {
  std::optional<Impl::CardWork> abandoned;
  {std::scoped_lock lock(impl_->mutex);impl_->card_generation=generation;
    abandoned=std::move(impl_->latest_card);impl_->latest_card.reset();}
  if(abandoned) abandoned->completion(Result<std::uint64_t,Error>::failure(cancelled()));
  SetEvent(impl_->changed.get());
}
void WindowsD3DPreviewPresenter::cancel(SessionId session,OperationId operation) {
  const std::scoped_lock lock(impl_->mutex); impl_->cancellations.emplace(session.value,operation.value);
}
}  // namespace hdrshot
