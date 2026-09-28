// Windows-only acceptance tool. Uses production CapturePort and both native
// presenters, with a separate explicit FP16 fixture and saved raw evidence.
#include "platform/windows/windows_capture.hpp"
#include "platform/windows/windows_d3d.hpp"
#include "platform/windows/windows_d3d_presenter.hpp"
#include "platform/windows/windows_analysis_backend.hpp"
#include "platform/qt/qt_session_diagnostics_port.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include <QCoreApplication>
#include <dcomp.h>
#include <dwmapi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <thread>

using namespace hdrshot;
namespace fs = std::filesystem;
namespace {
constexpr int side=512;
template<class T> T checked(Result<T,Error> r) {
  if(!r) {
    std::string message=r.error().module+":"+to_string(r.error().code);
    for(auto const& [k,v]:r.error().safe_context) message+=" "+k+"="+v;
    throw std::runtime_error(message);
  }
  return std::move(r.value());
}
void pump(unsigned ms) {
  const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
  do {
    MSG msg{};
    while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }while(std::chrono::steady_clock::now()<end);
}
template<class T> T await(std::future<T>& future) {
  const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);
  while(future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) {
    if(std::chrono::steady_clock::now()>end)throw std::runtime_error("acceptance timeout");
    pump(5);
  }
  return future.get();
}
template<class Container> void save(const fs::path& path,const Container& data) {
  if(fs::exists(path))throw std::runtime_error("refusing to overwrite evidence");
  std::ofstream f(path,std::ios::binary);
  f.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size()*sizeof(*data.data())));
  if(!f)throw std::runtime_error("evidence write failed");
}
struct Window {
  HWND handle{};
  Window(int x,int y,int w,int h) {
    handle=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOPMOST|WS_EX_NOREDIRECTIONBITMAP,
      L"STATIC",L"SeriousShot color validation",WS_POPUP,x,y,w,h,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!handle)throw std::runtime_error("window creation failed");
  }
  ~Window(){if(handle)DestroyWindow(handle);}
  void show(){ShowWindow(handle,SW_SHOWNOACTIVATE);SetWindowPos(handle,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);pump(120);}
  void hide(){ShowWindow(handle,SW_HIDE);pump(40);}
};
float eotf(float x){return x<=0.04045f?x/12.92f:std::pow((x+0.055f)/1.055f,2.4f);}
// Independent fixture matrix, not a call to the production display transform.
std::array<float,3> p3_to_scrgb(std::array<float,3> p) {
  return {float(1.2249401763*p[0]-.2249401763*p[1]),
    float(-.0420569547*p[0]+1.0420569547*p[1]),
    float(-.0196375546*p[0]-.0786360456*p[1]+1.0982736002*p[2])};
}
std::vector<std::uint16_t> fixture_pixels(const WindowsDisplayInfo& display) {
  std::vector<std::uint16_t> pixels(side*side*4);
  for(int y=0;y<side;++y)for(int x=0;x<side;++x) {
    const int patch=(y/8)*64+x/8,group=patch/1024,code=patch%1024;
    float level=float(code)/1023;
    std::array<float,3> value{};
    if(group==0)value={level,level,level};else value[group-1]=level;
    if(display.advanced_color_mode==0){for(auto& c:value)c=eotf(c);}
    else {
      if(display.advanced_color_mode==2 && group==0)for(auto& c:value)c*=4;
      value=p3_to_scrgb(value);
      if(display.advanced_color_mode==2)for(auto& c:value)c*=float(display.source_white.sdr_white_nits/80);
    }
    const auto i=std::size_t(y*side+x)*4;
    for(int c=0;c<3;++c)pixels[i+c]=ExtendedP3Mapper::encode_binary16(value[c]);
    pixels[i+3]=0x3c00;
  }
  return pixels;
}
struct FixtureSurface {
  WindowsD3DDevice gpu;
  winrt::com_ptr<IDXGISwapChain1> swap;
  winrt::com_ptr<IDCompositionDevice> composition;
  winrt::com_ptr<IDCompositionTarget> target;
  winrt::com_ptr<IDCompositionVisual> visual;
  FixtureSurface(HWND hwnd,const std::vector<std::uint16_t>& pixels) {
    winrt::com_ptr<IDXGIAdapter> adapter;winrt::check_hresult(gpu.device.as<IDXGIDevice>()->GetAdapter(adapter.put()));
    winrt::com_ptr<IDXGIFactory2> factory;winrt::check_hresult(adapter->GetParent(winrt::guid_of<IDXGIFactory2>(),factory.put_void()));
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=side;desc.Height=side;desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;
    desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
    winrt::check_hresult(factory->CreateSwapChainForComposition(gpu.device.get(),&desc,nullptr,swap.put()));
    auto color=swap.as<IDXGISwapChain3>();winrt::check_hresult(color->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709));
    winrt::check_hresult(DCompositionCreateDevice(gpu.device.as<IDXGIDevice>().get(),winrt::guid_of<IDCompositionDevice>(),composition.put_void()));
    winrt::check_hresult(composition->CreateTargetForHwnd(hwnd,TRUE,target.put()));
    winrt::check_hresult(composition->CreateVisual(visual.put()));winrt::check_hresult(visual->SetContent(swap.get()));
    winrt::check_hresult(target->SetRoot(visual.get()));
    winrt::com_ptr<ID3D11Texture2D> buffer;winrt::check_hresult(swap->GetBuffer(0,winrt::guid_of<ID3D11Texture2D>(),buffer.put_void()));
    gpu.context->UpdateSubresource(buffer.get(),0,nullptr,pixels.data(),side*8,0);
    D3D11_TEXTURE2D_DESC td{};buffer->GetDesc(&td);td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;td.MiscFlags=0;
    winrt::com_ptr<ID3D11Texture2D> staging;winrt::check_hresult(gpu.device->CreateTexture2D(&td,nullptr,staging.put()));
    gpu.context->CopyResource(staging.get(),buffer.get());D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(gpu.context->Map(staging.get(),0,D3D11_MAP_READ,0,&mapped));
    bool equal=true;
    for(int y=0;y<side;++y)equal &= std::memcmp(static_cast<const char*>(mapped.pData)+y*mapped.RowPitch,pixels.data()+y*side*4,side*8)==0;
    gpu.context->Unmap(staging.get(),0);if(!equal)throw std::runtime_error("fixture GPU submission mismatch");
    winrt::check_hresult(swap->Present(1,0));winrt::check_hresult(composition->Commit());
    winrt::check_hresult(composition->WaitForCommitCompletion());
  }
};
std::vector<std::uint16_t> capture_roi(const WindowsDisplayInfo& display,int x,int y,DiagnosticsPort* log) {
  const auto frame=checked(windows_capture_display(display,nullptr,10000,log));
  std::vector<std::uint16_t> out(side*side*4);
  for(int row=0;row<side;++row)std::copy_n(frame.rgba_scrgb.data()+std::size_t((row+y)*frame.size_px.width+x)*4,side*4,out.data()+std::size_t(row*side)*4);
  return out;
}
void capture_evidence(const fs::path& folder,const char* label,const WindowsDisplayInfo& display,int x,int y,DiagnosticsPort* log) {
  pump(160);DwmFlush();
  for(int i=0;i<3;++i)save(folder/(std::string(label)+"_"+std::to_string(i)+".rgba16f"),capture_roi(display,x,y,log));
}
void set_internal_hdr(bool enabled) {
  UINT32 np=0,nm=0;LONG rc=GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&np,&nm);
  if(rc)throw std::runtime_error("display sizes query failed");
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
  rc=QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&np,paths.data(),&nm,modes.data(),nullptr);
  if(rc)throw std::runtime_error("display paths query failed");
  DISPLAYCONFIG_PATH_INFO* selected=nullptr;
  for(UINT i=0;i<np;++i) {
    auto technology=paths[i].targetInfo.outputTechnology;
    if(technology!=DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED&&technology!=DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL)continue;
    if(selected)throw std::runtime_error("multiple internal targets");selected=&paths[i];
  }
  if(!selected)throw std::runtime_error("no internal display");
  DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 before{};
  before.header={DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2,sizeof(before),selected->targetInfo.adapterId,selected->targetInfo.id};
  rc=DisplayConfigGetDeviceInfo(&before.header);if(rc||!before.highDynamicRangeSupported)throw std::runtime_error("internal HDR capability unavailable");
  DISPLAYCONFIG_SET_HDR_STATE state{};
  state.header={DISPLAYCONFIG_DEVICE_INFO_SET_HDR_STATE,sizeof(state),selected->targetInfo.adapterId,selected->targetInfo.id};
  state.enableHdr=enabled?1:0;
  rc=DisplayConfigSetDeviceInfo(&state.header);if(rc)throw std::runtime_error("set HDR failed "+std::to_string(rc));
  pump(600);
  DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 after{};after.header=before.header;
  rc=DisplayConfigGetDeviceInfo(&after.header);
  if(rc||(after.activeColorMode==DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR)!=enabled)throw std::runtime_error("HDR state did not match request");
  std::cout<<"target="<<selected->targetInfo.id<<" before="<<before.activeColorMode<<" after="<<after.activeColorMode<<'\n';
}
void run(unsigned mode,const fs::path& folder) {
  if(fs::exists(folder))throw std::runtime_error("output folder already exists");fs::create_directories(folder);
  auto log=std::make_shared<QtSessionDiagnosticsPort>((folder/"logs").string());checked(log->start("T28_acceptance","Windows"));log->set_detailed_logging(true);
  auto displays=checked(windows_enumerate_displays(log.get()));
  auto it=std::find_if(displays.begin(),displays.end(),[mode](const auto& d){return d.advanced_color_mode==mode;});
  if(it==displays.end())throw std::runtime_error("requested actual mode not present");
  const auto display=*it;const auto sz=display.snapshot.capture_size_px;
  if(sz.width<side||sz.height<side)throw std::runtime_error("display too small");
  const int left=(sz.width-side)/2,top=(sz.height-side)/2;
  std::ofstream meta(folder/"manifest.json");meta<<"{\"mode\":"<<mode<<",\"build\":"<<windows_build_number()<<",\"device\":"<<std::quoted(display.device_name)
    <<",\"display_id\":"<<display.snapshot.id.value<<",\"white\":"<<display.source_white.sdr_white_nits<<",\"gain\":1,\"width\":512,\"height\":512,\"roi_x\":"<<left<<",\"roi_y\":"<<top<<",\"icc_path\":"<<std::quoted(display.icc_path)<<"}\n";meta.close();
  const auto expected=fixture_pixels(display);save(folder/"fixture.rgba16f",expected);
  Window fixture(display.physical_x+left,display.physical_y+top,side,side);
  FixtureSurface native(fixture.handle,expected);fixture.show();
  capture_evidence(folder,"baseline",display,left,top,log.get());
  auto all=std::make_shared<const std::vector<WindowsDisplayInfo>>(displays);
  WindowsCapturePort capture(all,1.0,log);
  auto mailbox=std::make_shared<std::promise<Result<NativeFrameBatch,Error>>>();auto future=mailbox->get_future();
  CaptureBatchRequest request{SessionId{901},OperationId{1},1,{display.snapshot.id}};request.exclude_own_windows=false;
  const auto begin=std::chrono::steady_clock::now();
  capture.capture(request,[mailbox](auto result){mailbox->set_value(std::move(result));});
  auto batch=checked(await(future));if(batch.frames.size()!=1||!batch.frames[0].linear_source)throw std::runtime_error("capture did not return unique FP32 source");
  const auto source=batch.frames[0].linear_source;
  auto p3=checked(source->read_region({left,top,side,side}));save(folder/"capture.rgba32f",p3);
  const double capture_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
  auto frozen=std::make_shared<FrozenDesktop>();frozen->frame_id=FrameId{901};frozen->display_generation=1;
  CanonicalFrameSegment segment{};segment.display_id=display.snapshot.id;segment.size_px=sz;
  segment.encoding=WindowsColor::linear_p3_encoding();segment.pixel_format=PixelFormat::rgba32_float;segment.linear_source=source;
  frozen->canonical_segments.push_back(std::move(segment));
  {
    Window window(display.physical_x,display.physical_y,sz.width,sz.height);
    auto presenter=checked(WindowsD3DPreviewPresenter::create(window.handle,display,log));
    PresentPreviewRequest draw{};draw.operation_id=OperationId{2};draw.target_display_id=display.snapshot.id;
    draw.model.session_id=SessionId{901};draw.model.frozen_desktop=frozen;draw.model.display_generation=1;
    draw.model.overlay_style.outside_linear_dim_factor=1;draw.model.overlay_style.selection_border_width_px=0;
    auto done=std::make_shared<std::promise<Result<PresentReceipt,Error>>>();auto f=done->get_future();
    presenter->present(draw,[done](auto r){done->set_value(std::move(r));});checked(await(f));window.show();
    capture_evidence(folder,"overlay",display,left,top,log.get());
    window.hide();presenter.reset();
  }
  {
    Window window(display.physical_x+left,display.physical_y+top,side,side);
    auto presenter=checked(make_windows_analysis_presenter(log));
    analysis::Input input{source,1,true};analysis::SourceView view{};view.target_size={side,side};view.offset_x=-left;view.offset_y=-top;
    view.settings.working_space=analysis::WorkingSpace::display_p3_pq;
    auto draw=[&](double white,const char* label) {
      view.settings.reference_white_nits=white;
      checked(presenter->present(input,view,reinterpret_cast<std::uintptr_t>(window.handle)));
      auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
      while(true){auto status=windows_analysis_presentation_status(*presenter);if(status.failed)throw std::runtime_error("source presenter failed");if(status.completed_generation==status.requested&&status.completed)break;
        if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("source presenter timeout");pump(10);}
      window.show();capture_evidence(folder,label,display,left,top,log.get());
    };
    draw(100,"source_100");draw(203,"source_203");
    // Preserve the same source and analysis parameters while moving a real
    // Source Signal HWND from Legacy to the current ACM/HDR target and back.
    if(mode==0) {
      const auto other=std::find_if(displays.begin(),displays.end(),[](const auto& d){return d.advanced_color_mode==1||d.advanced_color_mode==2;});
      if(other!=displays.end()) {
        const int other_left=(other->snapshot.capture_size_px.width-side)/2;
        const int other_top=(other->snapshot.capture_size_px.height-side)/2;
        if(!SetWindowPos(window.handle,HWND_TOPMOST,other->physical_x+other_left,
            other->physical_y+other_top,side,side,SWP_NOACTIVATE))
          throw std::runtime_error("cross-display move failed");
        checked(presenter->present(input,view,reinterpret_cast<std::uintptr_t>(window.handle)));
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(true) {
          auto status=windows_analysis_presentation_status(*presenter);
          if(status.failed)throw std::runtime_error("cross-display presenter failed");
          if(status.completed_generation==status.requested&&status.completed)break;
          if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("cross-display timeout");
          pump(10);
        }
        capture_evidence(folder,"source_cross_target",*other,other_left,other_top,log.get());
        std::ofstream cross(folder/"cross_target.json");
        cross<<"{\"mode\":"<<other->advanced_color_mode<<",\"white\":"<<other->source_white.sdr_white_nits<<"}\n";
        if(!SetWindowPos(window.handle,HWND_TOPMOST,display.physical_x+left,
            display.physical_y+top,side,side,SWP_NOACTIVATE))
          throw std::runtime_error("return display move failed");
        draw(203,"source_return");
      }
    }
    auto offscreen=checked(windows_analysis_render_offscreen(input,view));save(folder/"source_offscreen.rgba32f",checked(offscreen->read_region({0,0,side,side})));
    window.hide();presenter.reset();
  }
  fixture.hide();
  std::ofstream result(folder/"completion.json");result<<"{\"capture_ms\":"<<capture_ms<<",\"completed\":true,\"gpu_fixture_bits_verified\":true}\n";
  std::cout<<"Evidence="<<folder.string()<<" capture_ms="<<capture_ms<<'\n';
}
}
int main(int argc,char** argv) {
  try {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QCoreApplication app(argc,argv);winrt::init_apartment(winrt::apartment_type::multi_threaded);
    if(argc==3&&std::string(argv[1])=="--internal-hdr") {
      const std::string value=argv[2];if(value!="on"&&value!="off")throw std::runtime_error("HDR value must be on/off");
      set_internal_hdr(value=="on");return 0;
    }
    if(argc!=3)throw std::runtime_error("usage: probe MODE(0/1/2) NEW_OUTPUT_DIR | --internal-hdr on/off");
    run(unsigned(std::stoul(argv[1])),fs::u8path(argv[2]));return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
  catch(const winrt::hresult_error& e){std::cerr<<"HRESULT="<<std::uint32_t(e.code())<<'\n';return 1;}
}
