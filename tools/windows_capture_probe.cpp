#include "platform/windows/windows_capture.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <dwmapi.h>
#include <winrt/base.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>

using namespace hdrshot;
namespace {
void error(const Error& e) {
  std::cerr << e.module << ' ' << to_string(e.code);
  for (const auto& [k,v] : e.safe_context) std::cerr << ' ' << k << '=' << v;
  std::cerr << '\n';
}
struct PatchWindow {
  HWND hwnd{};
  winrt::com_ptr<ID3D11Device> device;
  winrt::com_ptr<ID3D11DeviceContext> context;
  winrt::com_ptr<IDXGISwapChain1> swapchain;
  winrt::com_ptr<ID3D11RenderTargetView> target;
  std::vector<std::array<float,3>> colors;
  int x{}, y{};
  ~PatchWindow() { if (hwnd) DestroyWindow(hwnd); }
  void create(const WindowsDisplayInfo& d) {
    WNDCLASSW klass{};
    klass.lpfnWndProc = DefWindowProcW;
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = L"SeriousShotCaptureProbe";
    RegisterClassW(&klass);
    x = d.physical_x + (d.snapshot.capture_size_px.width - 640) / 2;
    y = d.physical_y + (d.snapshot.capture_size_px.height - 96) / 2;
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, klass.lpszClassName,
        L"SeriousShot capture numerical probe", WS_POPUP, x,y,640,96,nullptr,nullptr,klass.hInstance,nullptr);
    winrt::check_bool(hwnd != nullptr);
    winrt::check_hresult(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,device.put(),nullptr,context.put()));
    winrt::com_ptr<IDXGIAdapter> adapter;
    winrt::check_hresult(device.as<IDXGIDevice>()->GetAdapter(adapter.put()));
    winrt::com_ptr<IDXGIFactory2> factory;
    winrt::check_hresult(adapter->GetParent(winrt::guid_of<IDXGIFactory2>(),factory.put_void()));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width=640; desc.Height=96; desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount=2; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
    winrt::check_hresult(factory->CreateSwapChainForHwnd(device.get(),hwnd,&desc,nullptr,nullptr,swapchain.put()));
    winrt::check_hresult(swapchain.as<IDXGISwapChain3>()->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709));
    winrt::com_ptr<ID3D11Texture2D> buffer;
    winrt::check_hresult(swapchain->GetBuffer(0,winrt::guid_of<ID3D11Texture2D>(),buffer.put_void()));
    winrt::check_hresult(device->CreateRenderTargetView(buffer.get(),nullptr,target.put()));
    const float white = 1.0F/WindowsColor::scrgb_to_edr_scale(d.source_white).value();
    colors = {{0,0,0},{0.18F*white,0.18F*white,0.18F*white},{0.5F*white,0.5F*white,0.5F*white},
        {white,white,white},{0.5F*white,0,0},{0,0.5F*white,0},{0,0,0.5F*white},
        {d.source_white.hdr_active ? 2.0F*white : white, d.source_white.hdr_active ? 2.0F*white : white, d.source_white.hdr_active ? 2.0F*white : white}};
    ShowWindow(hwnd,SW_SHOWNOACTIVATE);
    // A runner may launch the process with STARTF_USESHOWWINDOW/SW_HIDE,
    // overriding its first ShowWindow call. Explicitly expose the fixture.
    SetWindowPos(hwnd,HWND_TOPMOST,x,y,640,96,SWP_NOACTIVATE|SWP_SHOWWINDOW);
    for (int frame=0;frame<3;++frame) {
      for (int i=0;i<8;++i) {
        const float color[]{colors[i][0],colors[i][1],colors[i][2],1};
        const D3D11_RECT rect{i*80,0,(i+1)*80,96};
        context.as<ID3D11DeviceContext1>()->ClearView(target.get(),color,&rect,1);
      }
      const auto present_hr=swapchain->Present(1,0);
      std::cout<<"patch_present_hresult="<<static_cast<unsigned long>(present_hr)<<'\n';
      winrt::check_hresult(present_hr);
      DwmFlush();
      MSG msg{};
      while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    RECT actual_rect{}; GetWindowRect(hwnd,&actual_rect);
    DWORD affinity=0; GetWindowDisplayAffinity(hwnd,&affinity);
    DWORD cloaked=0; DwmGetWindowAttribute(hwnd,DWMWA_CLOAKED,&cloaked,sizeof(cloaked));
    HDC desktop_dc=GetDC(nullptr);
    auto at_point=WindowFromPoint({x+280,y+48});
    wchar_t at_class[256]{}; GetClassNameW(at_point,at_class,256);
    std::cout<<"patch_point_is_self="<<(at_point==hwnd)<<" point_class="<<winrt::to_string(at_class)<<'\n';
    const auto gdi=GetPixel(desktop_dc,x+280,y+48);
    ReleaseDC(nullptr,desktop_dc);
    std::cout << "patch_visible=" << IsWindowVisible(hwnd) << " affinity=" << affinity << " cloaked=" << cloaked
        << " rect=" << actual_rect.left << ',' << actual_rect.top << ',' << actual_rect.right << ',' << actual_rect.bottom
        << " gdi_white_rgb=" << static_cast<int>(GetRValue(gdi)) << ',' << static_cast<int>(GetGValue(gdi)) << ',' << static_cast<int>(GetBValue(gdi)) << '\n';
  }
};
}
int main(int argc,char** argv) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  winrt::init_apartment(winrt::apartment_type::single_threaded);
  wchar_t desktop_name[256]{};
  DWORD name_bytes=0, process_session=0;
  GetUserObjectInformationW(GetProcessWindowStation(),UOI_NAME,desktop_name,sizeof(desktop_name),&name_bytes);
  std::cout << "window_station=" << winrt::to_string(desktop_name) << '\n';
  GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,desktop_name,sizeof(desktop_name),&name_bytes);
  ProcessIdToSessionId(GetCurrentProcessId(),&process_session);
  std::cout << "process_session=" << process_session << " console_session=" << WTSGetActiveConsoleSessionId()
      << " desktop=" << winrt::to_string(desktop_name) << " remote=" << GetSystemMetrics(SM_REMOTESESSION) << '\n';
  HDESK input_desktop=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
  if(input_desktop) {
    GetUserObjectInformationW(input_desktop,UOI_NAME,desktop_name,sizeof(desktop_name),&name_bytes);
    std::cout << "input_desktop=" << winrt::to_string(desktop_name) << '\n';
    CloseDesktop(input_desktop);
  } else std::cout << "input_desktop_error=" << GetLastError() << '\n';
  if (argc < 2) { std::cerr << "Usage: hdrshot_windows_capture_probe <new-output-directory> [--patches]\n"; return 2; }
  const auto directory = std::filesystem::u8path(argv[1]);
  if (std::filesystem::exists(directory)) { std::cerr << "Output directory must be new\n"; return 2; }
  std::filesystem::create_directories(directory);
  const auto displays = windows_enumerate_displays();
  if (!displays) { error(displays.error()); return 1; }
  std::ofstream manifest(directory/"manifest.json");
  manifest << "{\n\"windows_build\":" << windows_build_number()
      << ",\n\"patch_absolute_tolerance\":0.002,\n\"patch_relative_tolerance\":0.002,\n\"displays\":[\n";
  bool failed = false;
  for (std::size_t index=0; index<displays.value().size(); ++index) {
    const auto& d=displays.value()[index];
    if(index) manifest << ",\n";
    manifest << "{\"id\":" << d.snapshot.id.value << ",\"device\":" << std::quoted(d.device_name)
        << ",\"name\":" << std::quoted(d.friendly_name) << ",\"gpu\":" << std::quoted(d.gpu_name)
        << ",\"icc\":" << std::quoted(d.icc_path) << ",\"width\":" << d.snapshot.capture_size_px.width
        << ",\"height\":" << d.snapshot.capture_size_px.height << ",\"scale\":" << d.snapshot.point_pixel_scale
        << ",\"advanced_color_mode\":" << d.advanced_color_mode << ",\"dxgi_color_space\":" << d.color_space
        << ",\"bits_per_color\":" << d.bits_per_color << ",\"sdr_white_nits\":" << d.source_white.sdr_white_nits
        << ",\"hdr_active\":" << (d.source_white.hdr_active ? "true":"false")
        << ",\"min_nits\":" << d.min_nits << ",\"max_nits\":" << d.max_nits
        << ",\"max_full_frame_nits\":" << d.max_full_frame_nits;
    PatchWindow patch;
    if (argc>2 && std::string_view(argv[2])=="--patches") patch.create(d);
    auto capture = std::async(std::launch::async,[d] { return windows_capture_display(d); });
    // Pump the STA window while WGC captures on an independent MTA thread.
    while(capture.wait_for(std::chrono::milliseconds(10))!=std::future_status::ready) {
      MSG msg{};
      while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    const auto raw=capture.get();
    if(!raw) { error(raw.error()); manifest << ",\"capture_ok\":false}"; failed=true; continue; }
    const auto filename="display-"+std::to_string(d.snapshot.id.value)+"-scrgb.rgba16f";
    std::ofstream file(directory/filename,std::ios::binary);
    file.write(reinterpret_cast<const char*>(raw.value().rgba_scrgb.data()),static_cast<std::streamsize>(raw.value().rgba_scrgb.size()*2U));
    if(!file) { std::cerr<<"raw file write failed\n"; return 1; }
    manifest << ",\"capture_ok\":true,\"raw_file\":" << std::quoted(filename)
        << ",\"texture_format\":" << raw.value().texture_format << ",\"timestamp_100ns\":" << raw.value().capture_time_100ns;
    const auto normalized=WindowsColor::normalize_capture(raw.value().rgba_scrgb,d.source_white);
    if(!normalized) { error(normalized.error()); failed=true; }
    manifest << ",\"normalization_ok\":" << (normalized?"true":"false");
    if (patch.hwnd) {
      manifest << ",\"patches\":[";
      for (std::size_t i=0; i<patch.colors.size(); ++i) {
        std::array<double,3> actual{};
        const int px=patch.x-d.physical_x+static_cast<int>(i)*80+40;
        const int py=patch.y-d.physical_y+48;
        for(int dy=-2;dy<2;++dy) for(int dx=-2;dx<2;++dx) {
          const auto p=(static_cast<std::size_t>(py+dy)*raw.value().size_px.width+static_cast<std::size_t>(px+dx))*4U;
          for (std::size_t c=0;c<3;++c) actual[c]+=ExtendedP3Mapper::decode_binary16(raw.value().rgba_scrgb[p+c]).value()/16.0;
        }
        bool pass=true;
        for(std::size_t c=0;c<3;++c) if(std::abs(actual[c]-patch.colors[i][c])>std::max(0.002,0.002*std::abs(static_cast<double>(patch.colors[i][c])))) pass=false;
        if(i) manifest << ',';
        manifest << "{\"expected\":[" << patch.colors[i][0] << ',' << patch.colors[i][1] << ',' << patch.colors[i][2]
            << "],\"actual\":[" << actual[0] << ',' << actual[1] << ',' << actual[2] << "],\"pass\":" << (pass?"true":"false") << '}';
        failed|=!pass;
      }
      manifest << ']';
    }
    manifest << '}';
    std::cout << "display=" << d.snapshot.id.value << " name=" << d.friendly_name << " hdr=" << d.source_white.hdr_active
        << " white=" << d.source_white.sdr_white_nits << " captured=" << raw.value().size_px.width << 'x' << raw.value().size_px.height << '\n';
  }
  manifest << "\n],\n\"passed\":" << (failed?"false":"true") << "\n}\n";
  return failed?1:0;
}
