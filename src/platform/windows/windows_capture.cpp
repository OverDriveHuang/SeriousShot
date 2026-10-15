#include "platform/windows/windows_capture.hpp"
#include "platform/windows/windows_capture_diagnostics.hpp"
#include "platform/windows/windows_capture_operations.hpp"
#include "platform/windows/windows_display_mode.hpp"
#include "platform/windows/windows_gpu_source.hpp"
#include "platform/windows/windows_icc_transform.hpp"
#include "ports/diagnostics_port.hpp"
#include "ports/export_ports.hpp"

#include <windows.h>
#include <winternl.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <shellscalingapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace hdrshot {
namespace {
namespace capture = winrt::Windows::Graphics::Capture;
namespace directx = winrt::Windows::Graphics::DirectX;
namespace metadata = winrt::Windows::Foundation::Metadata;
Error capture_error(const char* reason, std::optional<HRESULT> hr = {},
    std::source_location origin = std::source_location::current()) {
  return windows_capture_error(reason, hr, origin);
}
std::string utf8(const wchar_t* s) { return winrt::to_string(s); }
struct Apartment {
  explicit Apartment(std::source_location call_origin = std::source_location::current())
      : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), origin(call_origin) {}
  HRESULT hr;
  std::source_location origin;
  ~Apartment() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
using OperationKey = std::pair<std::uint64_t, std::uint64_t>;
struct BorderlessAccess {
  bool allowed{};
  std::uint32_t native_code{};
  const char* reason{"bordered_fallback"};
  std::optional<Error> error;
};
BorderlessAccess borderless_capture_access() {
  // The optional access prompt is made once per process only when the method
  // exists. A denial or absent API never blocks capture.
  static const BorderlessAccess result=[] {
    try {
      auto access=windows_capture_call("GraphicsCaptureAccess.RequestAccessAsync", [] {
        return capture::GraphicsCaptureAccess::RequestAccessAsync(capture::GraphicsCaptureAccessKind::Borderless); });
      const auto status=windows_capture_call("GraphicsCaptureAccess.wait_for", [&] {
        return access.wait_for(std::chrono::seconds(30)); });
      if(status==winrt::Windows::Foundation::AsyncStatus::Error) {
        // GetResults rethrows the operation's real error; Error is not timeout.
        (void)windows_capture_call("GraphicsCaptureAccess.GetResults", [&] { return access.GetResults(); });
        return BorderlessAccess{false, 0, "bordered_fallback", {}};
      }
      if(status!=winrt::Windows::Foundation::AsyncStatus::Completed) {
        if(status==winrt::Windows::Foundation::AsyncStatus::Started) {
          windows_capture_call("GraphicsCaptureAccess.Cancel", [&] { access.Cancel(); });
          return BorderlessAccess{false, WAIT_TIMEOUT, "bordered_fallback", {}};
        }
        return BorderlessAccess{false, static_cast<std::uint32_t>(status), "bordered_fallback", {}};
      }
      const auto permission=windows_capture_call("GraphicsCaptureAccess.GetResults", [&] { return access.GetResults(); });
      const bool granted=permission==winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus::Allowed;
      return BorderlessAccess{granted, static_cast<std::uint32_t>(permission),
          granted ? "available" : "bordered_fallback", {}};
    } catch(const WindowsCaptureCallFailure& e) {
      return BorderlessAccess{false, static_cast<std::uint32_t>(e.native_code), "bordered_fallback", e.error};
    }
  }();
  return result;
}
const char* mode_name(const std::uint32_t value) {
  switch (value) {
    case 0: return "legacy";
    case 1: return "sdr_acm";
    case 2: return "hdr";
    default: return "unknown";
  }
}
std::string target_token(const WindowsDisplayInfo& display) {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(8)
      << static_cast<std::uint32_t>(display.target_adapter_high)
      << std::setw(8) << display.target_adapter_low << "_t" << std::dec << display.target_id;
  return out.str();
}
void record_mode_facts(DiagnosticsPort* diagnostics, const WindowsDisplayInfo& display) {
  const auto id = std::to_string(display.snapshot.id.value);
  const auto code = std::to_string(display.mode_query_code);
  record_diagnostic_stage(diagnostics, {}, {}, "windows.color", "capture.mode_detected", "success",
      {{"displayId", id}, {"api", display.mode_query_source}, {"nativeCode", code},
       {"returnedSpace", mode_name(display.detected_mode)}});
  record_diagnostic_stage(diagnostics, {}, {}, "windows.color", "capture.mode_effective", "success",
      {{"displayId", id}, {"api", display.mode_query_source}, {"nativeCode", code},
       {"returnedSpace", mode_name(display.advanced_color_mode)},
       {"reason", display.mode_fallback_reason}});
  record_diagnostic_stage(diagnostics, {}, {}, "windows.color", "capture.identity", "success",
      {{"displayId", id}, {"source", target_token(display)}});
  record_diagnostic_stage(diagnostics, {}, {}, "windows.color", "capture.info2_flags", "success",
      {{"displayId", id}, {"nativeCode", std::to_string(display.mode_query_flags)},
       {"returnedSpace", std::to_string(display.mode_raw_active)}});
}
}

std::uint32_t windows_build_number() {
  using RtlVersion = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  const auto fn = reinterpret_cast<RtlVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  RTL_OSVERSIONINFOW version{};
  version.dwOSVersionInfoSize = sizeof(version);
  return fn && fn(&version) == 0 ? version.dwBuildNumber : 0;
}

Result<WindowsCaptureCapabilityPlan, Error> windows_check_capture_runtime_capabilities(
    DiagnosticsPort* diagnostics) {
  using Out = Result<WindowsCaptureCapabilityPlan, Error>;
  // CoInitializeEx returns RPC_E_CHANGED_MODE on an existing STA, in which
  // case this helper uses that apartment without balancing an unowned init.
  Apartment apartment;
  if (FAILED(apartment.hr) && apartment.hr != RPC_E_CHANGED_MODE) {
    auto error = capture_error("wgc_apartment_unavailable", apartment.hr, apartment.origin);
    error.safe_context["api"] = "CoInitializeEx";
    record_diagnostic_stage(diagnostics, {}, {}, "windows.color", "capture.capability",
        "failure", {}, &error);
    return Out::failure(error);
  }
  try {
    const auto pool_name = L"Windows.Graphics.Capture.Direct3D11CaptureFramePool";
    if (!windows_capture_call("GraphicsCaptureSession.IsSupported", [] {
          return capture::GraphicsCaptureSession::IsSupported(); }) ||
        !windows_capture_call("ApiInformation.IsMethodPresent", [&] {
          return metadata::ApiInformation::IsMethodPresent(pool_name, L"CreateFreeThreaded"); })) {
      const auto error = capture_error("wgc_unsupported");
      record_diagnostic_stage(diagnostics, {}, {}, "windows.color", "capture.capability",
          "failure", {{"api", "wgc_free_threaded"}}, &error);
      return Out::failure(error);
    }
    const auto session_name = L"Windows.Graphics.Capture.GraphicsCaptureSession";
    const bool cursor_present = windows_capture_call("ApiInformation.IsPropertyPresent", [&] {
      return metadata::ApiInformation::IsPropertyPresent(session_name, L"IsCursorCaptureEnabled"); });
    const bool border_present = windows_capture_call("ApiInformation.IsPropertyPresent", [&] {
      return metadata::ApiInformation::IsPropertyPresent(session_name, L"IsBorderRequired"); });
    const bool border_access_present = windows_capture_call("ApiInformation.IsTypePresent", [] {
      return metadata::ApiInformation::IsTypePresent(L"Windows.Graphics.Capture.GraphicsCaptureAccess"); }) &&
        windows_capture_call("ApiInformation.IsMethodPresent", [] {
          return metadata::ApiInformation::IsMethodPresent(
              L"Windows.Graphics.Capture.GraphicsCaptureAccess", L"RequestAccessAsync"); });
    const auto plan = windows_plan_capture_capabilities(
        cursor_present, border_present, border_access_present);
    if (!plan.can_exclude_cursor) {
      const auto error = capture_error("cursor_exclusion_unavailable");
      record_diagnostic_stage(diagnostics, {}, {}, "windows.color", "capture.capability",
          "failure", {{"api", "wgc_cursor"}}, &error);
      return Out::failure(error);
    }
    record_diagnostic_stage(diagnostics, {}, {}, "windows.color", "capture.capability",
        "success", {{"api", "wgc_free_threaded"}, {"reason", "available"},
        {"cursor", "exclusion_available"},
        {"osVersion", std::to_string(windows_build_number())}});
    record_diagnostic_stage(diagnostics, {}, {}, "windows.color", "capture.capability",
        "success", {{"api", "wgc_border"},
        {"reason", plan.may_request_borderless ? "optional_available" : "bordered_fallback"}});
    return Out::success(plan);
  } catch (const WindowsCaptureCallFailure& e) {
    const auto& error = e.error;
    record_diagnostic_stage(diagnostics, {}, {}, "windows.color", "capture.capability",
        "failure", {}, &error);
    return Out::failure(error);
  }
}

Result<std::vector<WindowsDisplayInfo>, Error> windows_enumerate_displays(DiagnosticsPort* diagnostics) {
  try {
    winrt::com_ptr<IDXGIFactory1> factory;
    windows_capture_call("CreateDXGIFactory1", [&] {
      winrt::check_hresult(CreateDXGIFactory1(winrt::guid_of<IDXGIFactory1>(), factory.put_void())); });
    std::vector<WindowsDisplayInfo> displays;
    for (UINT a = 0;; ++a) {
      winrt::com_ptr<IDXGIAdapter1> adapter;
      const auto adapter_hr = windows_capture_call("IDXGIFactory.EnumAdapters1", [&] {
        const auto hr = factory->EnumAdapters1(a, adapter.put());
        if (hr != DXGI_ERROR_NOT_FOUND) winrt::check_hresult(hr);
        return hr;
      });
      if (adapter_hr == DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 gpu{};
      windows_capture_call("IDXGIAdapter.GetDesc1", [&] { winrt::check_hresult(adapter->GetDesc1(&gpu)); });
      for (UINT o = 0;; ++o) {
        winrt::com_ptr<IDXGIOutput> output;
        const auto output_hr = windows_capture_call("IDXGIAdapter.EnumOutputs", [&] {
          const auto hr = adapter->EnumOutputs(o, output.put());
          if (hr != DXGI_ERROR_NOT_FOUND) winrt::check_hresult(hr);
          return hr;
        });
        if (output_hr == DXGI_ERROR_NOT_FOUND) break;
        const auto advanced = windows_capture_call("IDXGIOutput.QueryInterface", [&] { return output.as<IDXGIOutput6>(); });
        DXGI_OUTPUT_DESC1 desc{};
        windows_capture_call("IDXGIOutput6.GetDesc1", [&] { winrt::check_hresult(advanced->GetDesc1(&desc)); });
        if (!desc.AttachedToDesktop) continue;
        WindowsDisplayInfo info;
        info.monitor = reinterpret_cast<std::uintptr_t>(desc.Monitor);
        info.snapshot.id = DisplayId{static_cast<std::uint64_t>(info.monitor)};
        info.physical_x = desc.DesktopCoordinates.left;
        info.physical_y = desc.DesktopCoordinates.top;
        info.snapshot.capture_size_px = {desc.DesktopCoordinates.right - desc.DesktopCoordinates.left,
                                         desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top};
        UINT dpi_x = 96, dpi_y = 96;
        const auto dpi_result = windows_capture_status("GetDpiForMonitor", [&] {
          return GetDpiForMonitor(desc.Monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y); });
        if (FAILED(dpi_result.value)) {
          auto error = dpi_result.error("display_dpi", dpi_result.value);
          error.safe_context["displayId"] = std::to_string(info.snapshot.id.value);
          return Result<std::vector<WindowsDisplayInfo>, Error>::failure(std::move(error));
        }
        info.snapshot.point_pixel_scale = static_cast<double>(dpi_x) / 96.0;
        // Qt's Windows screen islands preserve physical origins and scale each
        // screen's extent. The composition root checks the native screen match.
        info.snapshot.desktop_frame_points = {static_cast<double>(info.physical_x),
            static_cast<double>(info.physical_y), info.snapshot.capture_size_px.width / info.snapshot.point_pixel_scale,
            info.snapshot.capture_size_px.height / info.snapshot.point_pixel_scale};
        info.device_name = utf8(desc.DeviceName);
        info.gpu_name = utf8(gpu.Description);
        info.dxgi_adapter_low = gpu.AdapterLuid.LowPart;
        info.dxgi_adapter_high = gpu.AdapterLuid.HighPart;
        info.color_space = static_cast<std::uint32_t>(desc.ColorSpace);
        info.bits_per_color = desc.BitsPerColor;
        info.min_nits = desc.MinLuminance;
        info.max_nits = desc.MaxLuminance;
        info.max_full_frame_nits = desc.MaxFullFrameLuminance;
        // Output6 reports a precise HDR fallback only. It is not evidence for
        // SDR ACM, and mode zero is not silently asserted before INFO_2.
        info.detected_mode = windows_mode_unknown;
        displays.push_back(std::move(info));
      }
    }
    UINT32 path_count = 0, mode_count = 0;
    LONG config_result = ERROR_INSUFFICIENT_BUFFER;
    std::optional<Error> config_error;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    for (int attempt = 0; attempt < 3 && config_result == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
      const auto sizes = windows_capture_status("GetDisplayConfigBufferSizes", [&] {
        return GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count); });
      config_result = sizes.value;
      if (config_result != ERROR_SUCCESS) {
        config_error = sizes.error("query_display_config", HRESULT_FROM_WIN32(config_result));
        break;
      }
      paths.resize(path_count); modes.resize(mode_count);
      const auto queried = windows_capture_status("QueryDisplayConfig", [&] {
        return QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr); });
      config_result = queried.value;
      if (config_result != ERROR_SUCCESS)
        config_error = queried.error("query_display_config", HRESULT_FROM_WIN32(config_result));
    }
    if (config_result != ERROR_SUCCESS) {
      auto error = config_error.value_or(capture_error("query_display_config"));
      return Result<std::vector<WindowsDisplayInfo>, Error>::failure(std::move(error));
    }
    paths.resize(path_count);
    std::vector<WindowsTargetPath> target_paths;
    target_paths.reserve(paths.size());
    for (const auto& path : paths) {
      DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
      source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source),
          path.sourceInfo.adapterId, path.sourceInfo.id};
      const auto source_result = windows_capture_status("DisplayConfigGetDeviceInfo.SOURCE_NAME", [&] {
        return DisplayConfigGetDeviceInfo(&source.header); });
      if (source_result.value != ERROR_SUCCESS) {
        auto error = source_result.error("display_source_name", HRESULT_FROM_WIN32(source_result.value));
        record_diagnostic_stage(diagnostics, {}, {}, "windows.color", "display.source_name", "failure", {}, &error);
      }
      target_paths.push_back({path.sourceInfo.adapterId.LowPart,
          path.sourceInfo.adapterId.HighPart, path.sourceInfo.id, path.targetInfo.id,
          source_result.value == ERROR_SUCCESS ? utf8(source.viewGdiDeviceName) : std::string{}});
    }
    for (auto& info : displays) {
      const auto match = windows_match_target(info.dxgi_adapter_low,
          info.dxgi_adapter_high, info.device_name, target_paths);
      if (match.status == WindowsTargetMatchStatus::missing)
        return Result<std::vector<WindowsDisplayInfo>, Error>::failure(capture_error("display_path_not_found"));
      if (match.status == WindowsTargetMatchStatus::ambiguous)
        return Result<std::vector<WindowsDisplayInfo>, Error>::failure(capture_error("display_target_ambiguous"));
      {
        const auto& path = paths[match.index];
        info.target_adapter_low = path.targetInfo.adapterId.LowPart;
        info.target_adapter_high = path.targetInfo.adapterId.HighPart;
        info.target_id = path.targetInfo.id;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target), path.targetInfo.adapterId, path.targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) info.friendly_name = utf8(target.monitorFriendlyDeviceName);
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 color{};
        color.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2, sizeof(color), path.targetInfo.adapterId, path.targetInfo.id};
        const auto color_result = DisplayConfigGetDeviceInfo(&color.header);
        const auto selected = windows_choose_display_mode({static_cast<std::uint32_t>(color_result),
            static_cast<std::uint32_t>(color.activeColorMode), color.value,
            info.color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020});
        info.detected_mode = selected.detected_mode;
        info.advanced_color_mode = selected.effective_mode;
        info.mode_query_source = selected.source;
        info.mode_fallback_reason = selected.reason;
        info.mode_query_code = selected.info2_status;
        info.mode_query_flags = selected.info2_flags;
        info.mode_raw_active = selected.info2_active_mode;
        info.source_white.hdr_active = selected.effective_mode == 2;
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof(white), path.targetInfo.adapterId, path.targetInfo.id};
        const auto white_result = windows_capture_status("DisplayConfigGetDeviceInfo.SDR_WHITE_LEVEL", [&] {
          return DisplayConfigGetDeviceInfo(&white.header); });
        if (white_result.value == ERROR_SUCCESS) info.source_white.sdr_white_nits = white.SDRWhiteLevel * 80.0 / 1000.0;
        else if (info.source_white.hdr_active) {
          auto error = white_result.error("sdr_white_unavailable", HRESULT_FROM_WIN32(white_result.value));
          error.safe_context["displayId"] = std::to_string(info.snapshot.id.value);
          return Result<std::vector<WindowsDisplayInfo>, Error>::failure(std::move(error));
        }
      }
      record_mode_facts(diagnostics, info);
      info.snapshot.dynamic_range = info.source_white.hdr_active ? DisplayDynamicRange::hdr : DisplayDynamicRange::sdr;
      const auto scale = WindowsColor::scrgb_to_edr_scale(info.source_white);
      if (!scale) return Result<std::vector<WindowsDisplayInfo>, Error>::failure(scale.error());
      if (info.advanced_color_mode == 2) {
        record_diagnostic_stage(diagnostics, {}, {}, "windows.color", "capture.white", "success",
            {{"displayId", std::to_string(info.snapshot.id.value)},
             {"diffuseWhite", std::to_string(info.source_white.sdr_white_nits)},
             {"scale", std::to_string(scale.value())}});
      } else if (info.advanced_color_mode == 1) {
        record_diagnostic_stage(diagnostics, {}, {}, "windows.color", "capture.white", "success",
            {{"displayId", std::to_string(info.snapshot.id.value)}, {"scale", "1"}});
      }
      const auto name = winrt::to_hstring(info.device_name);
      const auto dc_result = windows_capture_status("CreateDCW", [&] {
        return CreateDCW(L"DISPLAY", name.c_str(), nullptr, nullptr); });
      const HDC dc = dc_result.value;
      std::optional<Error> profile_query_error;
      if (dc) {
        DWORD chars = 32768;
        std::vector<wchar_t> profile(chars);
        const auto queried = windows_capture_status("GetICMProfileW", [&] {
          const bool success = GetICMProfileW(dc, &chars, profile.data()) != FALSE;
          const DWORD code = success ? ERROR_SUCCESS : GetLastError();
          return std::pair{success, code};
        });
        if (queried.value.first) info.icc_path = utf8(profile.data());
        else {
          const auto native_error = queried.value.second;
          profile_query_error = queried.error("legacy_profile_unavailable",
              native_error ? std::optional<HRESULT>{HRESULT_FROM_WIN32(native_error)} : std::nullopt);
        }
        DeleteDC(dc);
      } else {
        // CreateDCW does not promise a meaningful GetLastError on failure.
        profile_query_error = dc_result.error("legacy_profile_unavailable");
      }
      if (info.advanced_color_mode == 0) {
        if (info.icc_path.empty()) {
          auto error = profile_query_error.value_or(capture_error("legacy_profile_unavailable"));
          error.safe_context["displayId"] = std::to_string(info.snapshot.id.value);
          return Result<std::vector<WindowsDisplayInfo>, Error>::failure(std::move(error));
        }
        auto profile = windows_load_icc_profile(info.icc_path);
        if (!profile) {
          auto error = profile.error();
          error.safe_context["displayId"] = std::to_string(info.snapshot.id.value);
          return Result<std::vector<WindowsDisplayInfo>, Error>::failure(std::move(error));
        }
        info.icc_profile = std::move(profile.value());
        record_diagnostic_stage(diagnostics, {}, {}, "windows.color", "capture.profile", "success",
            {{"displayId", std::to_string(info.snapshot.id.value)},
             {"source", info.icc_profile->sha256}, {"reason", "loaded"}});
      }
    }
    if (displays.empty()) return Result<std::vector<WindowsDisplayInfo>, Error>::failure(capture_error("no_displays"));
    std::sort(displays.begin(), displays.end(), [](const auto& a, const auto& b) { return a.snapshot.id < b.snapshot.id; });
    return Result<std::vector<WindowsDisplayInfo>, Error>::success(std::move(displays));
  } catch (const WindowsCaptureCallFailure& e) {
    return Result<std::vector<WindowsDisplayInfo>, Error>::failure(e.error);
  } catch (const winrt::hresult_error& e) {
    // Only non-native conversion code should reach this fallback.
    return Result<std::vector<WindowsDisplayInfo>, Error>::failure(capture_error("enumerate_displays", e.code()));
  }
}

Result<WindowsRawCapture, Error> windows_capture_display(
    const WindowsDisplayInfo& display, const std::atomic_bool* cancelled,
    const std::uint32_t timeout_ms, DiagnosticsPort* diagnostics) {
  const auto capability = windows_check_capture_runtime_capabilities(diagnostics);
  if (!capability) {
    auto error = capability.error();
    error.safe_context["displayId"] = std::to_string(display.snapshot.id.value);
    return Result<WindowsRawCapture, Error>::failure(std::move(error));
  }
  Apartment apartment;
  if (FAILED(apartment.hr) && apartment.hr != RPC_E_CHANGED_MODE) {
    auto error = capture_error("wgc_apartment_unavailable", apartment.hr, apartment.origin);
    error.safe_context["api"] = "CoInitializeEx";
    error.safe_context["displayId"] = std::to_string(display.snapshot.id.value);
    return Result<WindowsRawCapture, Error>::failure(std::move(error));
  }
  capture::Direct3D11CaptureFramePool pool{nullptr};
  capture::GraphicsCaptureSession session{nullptr};
  capture::Direct3D11CaptureFrame frame{nullptr};
  capture::Direct3D11CaptureFramePool::FrameArrived_revoker token{};
  using FrameState = WindowsCaptureFrameState<capture::Direct3D11CaptureFrame>;
  std::shared_ptr<FrameState> state;
  const auto perform = [&]() -> Result<WindowsRawCapture, Error> {
  try {
    const auto interop = windows_capture_call("GraphicsCaptureItem.GetActivationFactory", [] {
      return winrt::get_activation_factory<capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>(); });
    capture::GraphicsCaptureItem item{nullptr};
    windows_capture_call("CreateForMonitor", [&] {
      winrt::check_hresult(interop->CreateForMonitor(reinterpret_cast<HMONITOR>(display.monitor),
          winrt::guid_of<capture::GraphicsCaptureItem>(), winrt::put_abi(item))); });
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    windows_capture_call("D3D11CreateDevice", [&] {
      winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
          D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, device.put(), nullptr, context.put())); });
    winrt::com_ptr<IInspectable> inspectable;
    const auto dxgi_device = windows_capture_call("ID3D11Device.QueryInterface", [&] { return device.as<IDXGIDevice>(); });
    windows_capture_call("CreateDirect3D11DeviceFromDXGIDevice", [&] {
      winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), inspectable.put())); });
    const auto rt_device = windows_capture_call("IDirect3DDevice.QueryInterface", [&] {
      return inspectable.as<directx::Direct3D11::IDirect3DDevice>(); });
    const auto item_size = windows_capture_call("GraphicsCaptureItem.Size", [&] { return item.Size(); });
    pool = windows_capture_call("Direct3D11CaptureFramePool.CreateFreeThreaded", [&] {
      return capture::Direct3D11CaptureFramePool::CreateFreeThreaded(rt_device,
          directx::DirectXPixelFormat::R16G16B16A16Float, 2, item_size); });
    session = windows_capture_call("CreateCaptureSession", [&] { return pool.CreateCaptureSession(item); });
    const auto session_name = L"Windows.Graphics.Capture.GraphicsCaptureSession";
    const bool border_present = windows_capture_call("ApiInformation.IsPropertyPresent", [&] {
      return metadata::ApiInformation::IsPropertyPresent(session_name, L"IsBorderRequired"); });
    try {
      windows_capture_call("IsCursorCaptureEnabled.set", [&] { session.IsCursorCaptureEnabled(false); });
    } catch (const WindowsCaptureCallFailure& e) {
      auto error = e.error;
      error.safe_context["reason"] = "cursor_exclusion_failed";
      return Result<WindowsRawCapture, Error>::failure(std::move(error));
    }
    BorderlessAccess border_access{};
    if (capability.value().may_request_borderless) border_access = borderless_capture_access();
    if (border_access.error)
      record_diagnostic_stage(diagnostics, {}, {}, "windows.color", "capture.border_optional", "fallback",
          {{"displayId", std::to_string(display.snapshot.id.value)}}, &*border_access.error);
    if (border_present && border_access.allowed) {
      try { windows_capture_call("IsBorderRequired.set", [&] { session.IsBorderRequired(false); }); }
      catch (const WindowsCaptureCallFailure& e) {
        border_access = {false, static_cast<std::uint32_t>(e.native_code), "bordered_fallback", {}};
        record_diagnostic_stage(diagnostics, {}, {}, "windows.color", "capture.border_optional", "fallback", {}, &e.error);
      }
    }
    bool border_observed = false;
    bool border_required = true;
    if (border_present) {
      try {
        border_required = windows_capture_call("IsBorderRequired.get", [&] { return session.IsBorderRequired(); });
        border_observed = true;
      } catch (const WindowsCaptureCallFailure& e) {
        border_access = {false, static_cast<std::uint32_t>(e.native_code), "bordered_fallback", {}};
        record_diagnostic_stage(diagnostics, {}, {}, "windows.color", "capture.border_optional", "fallback", {}, &e.error);
      }
    }
    record_diagnostic_stage(diagnostics, {}, {}, "windows.color", "capture.border", "success",
        {{"displayId", std::to_string(display.snapshot.id.value)}, {"api", "wgc_border"},
         {"osVersion", std::to_string(windows_build_number())},
         {"nativeCode", std::to_string(border_access.native_code)},
         {"reason", border_observed && !border_required ? "available" : "bordered_fallback"}});
    state = std::make_shared<FrameState>();
    token = windows_capture_call("Direct3D11CaptureFramePool.FrameArrived", [&] {
      return pool.FrameArrived(winrt::auto_revoke, [state](const auto& sender, const auto&) {
        state->receive([&] { return sender.TryGetNextFrame(); });
      }); });
    windows_capture_call("StartCapture", [&] { session.StartCapture(); });
    auto received = state->wait(cancelled, timeout_ms);
    if (!received) return Result<WindowsRawCapture, Error>::failure(received.error());
    frame = std::move(received.value());
    const auto surface = windows_capture_call("Direct3D11CaptureFrame.Surface", [&] { return frame.Surface(); });
    const auto access = windows_capture_call("IDirect3DDxgiInterfaceAccess.QueryInterface", [&] {
      return surface.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>(); });
    winrt::com_ptr<ID3D11Texture2D> texture;
    windows_capture_call("IDirect3DDxgiInterfaceAccess.GetInterface", [&] {
      winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), texture.put_void())); });
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    const auto size = windows_capture_call("Direct3D11CaptureFrame.ContentSize", [&] { return frame.ContentSize(); });
    if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || size.Width <= 0 || size.Height <= 0 ||
        size.Width != display.snapshot.capture_size_px.width || size.Height != display.snapshot.capture_size_px.height ||
        static_cast<UINT>(size.Width) > desc.Width || static_cast<UINT>(size.Height) > desc.Height) {
      return Result<WindowsRawCapture, Error>::failure(capture_error("capture_size_or_format_changed"));
    }
    WindowsRawCapture result{display.snapshot.id, {size.Width, size.Height},
        windows_capture_call("Direct3D11CaptureFrame.SystemRelativeTime", [&] {
          return frame.SystemRelativeTime().count(); }), static_cast<std::uint32_t>(desc.Format), {}};
    desc.Width = static_cast<UINT>(size.Width); desc.Height = static_cast<UINT>(size.Height);
    desc.MipLevels = 1; desc.ArraySize = 1;
    desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0; desc.Usage = D3D11_USAGE_STAGING;
    winrt::com_ptr<ID3D11Texture2D> staging;
    windows_capture_call("ID3D11Device.CreateTexture2D", [&] {
      winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, staging.put())); });
    const D3D11_BOX box{0, 0, 0, desc.Width, desc.Height, 1};
    context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, texture.get(), 0, &box);
    const auto row_samples = static_cast<std::size_t>(size.Width) * 4U;
    result.rgba_scrgb.resize(row_samples * static_cast<std::size_t>(size.Height));
    D3D11_MAPPED_SUBRESOURCE mapped{};
    windows_capture_call("ID3D11DeviceContext.Map", [&] {
      winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)); });
    for (std::size_t y = 0; y < static_cast<std::size_t>(size.Height); ++y) {
      std::memcpy(result.rgba_scrgb.data() + y * row_samples,
          static_cast<const char*>(mapped.pData) + y * mapped.RowPitch, row_samples * sizeof(std::uint16_t));
    }
    context->Unmap(staging.get(), 0);
    texture = nullptr;
    return Result<WindowsRawCapture, Error>::success(std::move(result));
  } catch (const WindowsCaptureCallFailure& e) {
    return Result<WindowsRawCapture, Error>::failure(e.error);
  } catch (const std::exception&) {
    return Result<WindowsRawCapture, Error>::failure(capture_error("capture_resource_failure"));
  } catch (...) {
    return Result<WindowsRawCapture, Error>::failure(capture_error("capture_worker_failure"));
  }
  };
  auto result = perform();
  if (state) state->close();
  WindowsCaptureCleanup cleanup;
  cleanup.attempt("FrameArrived.revoke", [&] { token.revoke(); });
  if (frame) cleanup.attempt("Direct3D11CaptureFrame.Close", [&] { frame.Close(); });
  if (session) cleanup.attempt("GraphicsCaptureSession.Close", [&] { session.Close(); });
  if (pool) cleanup.attempt("Direct3D11CaptureFramePool.Close", [&] { pool.Close(); });
  if (cleanup.failed() && !result) {
    try {
      auto error = cleanup.error();
      error.safe_context["displayId"] = std::to_string(display.snapshot.id.value);
      record_diagnostic_stage(diagnostics, {}, {}, "capture", "cleanup", "failure", {}, &error);
    } catch (...) { /* Secondary diagnostic allocation cannot replace primary. */ }
  }
  result = cleanup.complete(std::move(result));
  if (!result) {
    try {
      auto error = result.error();
      error.safe_context["displayId"] = std::to_string(display.snapshot.id.value);
      return Result<WindowsRawCapture, Error>::failure(std::move(error));
    } catch (...) { return result; } // Keep primary even if annotation cannot allocate.
  }
  return result;
}

void WindowsDisplayCatalogPort::snapshot_displays(const SnapshotDisplaysRequest&, Completion completion) {
  const auto native = displays_ ? Result<std::vector<WindowsDisplayInfo>,Error>::success(*displays_) : windows_enumerate_displays();
  if (!native) { completion(Result<DisplaySnapshotSet, Error>::failure(native.error())); return; }
  static std::atomic_uint64_t generation{0};
  DisplaySnapshotSet set{++generation, {}};
  for (const auto& display : native.value()) set.displays.push_back(display.snapshot);
  completion(Result<DisplaySnapshotSet, Error>::success(std::move(set)));
}

struct WindowsCapturePort::Impl {
  std::shared_ptr<const std::vector<WindowsDisplayInfo>> displays;
  std::shared_ptr<DiagnosticsPort> diagnostics;
  WindowsCaptureOptions options{};
  std::mutex mutex;
  std::map<OperationKey, std::shared_ptr<std::atomic_bool>> cancellations;
  std::vector<std::future<void>> workers;
};
WindowsCapturePort::WindowsCapturePort(std::shared_ptr<const std::vector<WindowsDisplayInfo>> displays,
    double gain, std::shared_ptr<DiagnosticsPort> diagnostics) : impl_(std::make_unique<Impl>()) {
  impl_->displays=std::move(displays);
  impl_->diagnostics=std::move(diagnostics);
  if (valid_windows_scrgb_gain(gain)) impl_->options={true, false, gain};
}
WindowsCapturePort::WindowsCapturePort(std::shared_ptr<const std::vector<WindowsDisplayInfo>> displays,
    WindowsCaptureOptions options, std::shared_ptr<DiagnosticsPort> diagnostics)
    : impl_(std::make_unique<Impl>()) {
  impl_->displays=std::move(displays);
  impl_->diagnostics=std::move(diagnostics);
  if (valid_windows_scrgb_gain(options.gain)) impl_->options=options;
}
void WindowsCapturePort::set_gain(double gain) {
  if (!valid_windows_scrgb_gain(gain)) return;
  const std::scoped_lock lock(impl_->mutex);
  impl_->options.gain=gain;
  impl_->options.gain_enabled=true;
}
void WindowsCapturePort::set_capture_options(WindowsCaptureOptions options) {
  if (!valid_windows_scrgb_gain(options.gain)) return;
  const std::scoped_lock lock(impl_->mutex);
  impl_->options=options;
}
WindowsCapturePort::~WindowsCapturePort() {
  { std::scoped_lock lock(impl_->mutex);
    for (auto& [key, flag] : impl_->cancellations) flag->store(true);
  }
  for (auto& work : impl_->workers) work.wait();
}
void WindowsCapturePort::capture(const CaptureBatchRequest& request, Completion completion) {
  if (request.targets.empty() || request.display_generation == 0 || request.desired_format != PixelFormat::rgba16_float ||
      std::set<DisplayId>(request.targets.begin(), request.targets.end()).size() != request.targets.size()) {
    completion(Result<NativeFrameBatch, Error>::failure(capture_error("invalid_capture_request"))); return;
  }
  const OperationKey key{request.session_id.value, request.operation_id.value};
  const auto flag = std::make_shared<std::atomic_bool>(false);
  std::unique_lock lock(impl_->mutex);
  const auto frozen_options = impl_->options;
  if (!impl_->cancellations.emplace(key, flag).second) {
    lock.unlock(); completion(Result<NativeFrameBatch, Error>::failure(capture_error("duplicate_operation"))); return;
  }
  std::erase_if(impl_->workers, [](auto& work) { return work.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
  impl_->workers.push_back(std::async(std::launch::async, [request, completion = std::move(completion), flag,
      native=impl_->displays, diagnostics=impl_->diagnostics, frozen_options]() mutable {
    const auto perform=[&]() -> Result<NativeFrameBatch,Error> {
    try {
    const auto displays = native ? Result<std::vector<WindowsDisplayInfo>,Error>::success(*native)
                                 : windows_enumerate_displays(diagnostics.get());
    if (!displays) return Result<NativeFrameBatch, Error>::failure(displays.error());
    std::vector<std::future<Result<NativeCaptureFrame, Error>>> futures;
    for (const auto target : request.targets) {
      const auto found = std::find_if(displays.value().begin(), displays.value().end(), [target](const auto& d) { return d.snapshot.id == target; });
      if (found == displays.value().end()) {
        auto error = capture_error("target_display_missing");
        error.safe_context["displayId"] = std::to_string(target.value);
        return Result<NativeFrameBatch, Error>::failure(std::move(error));
      }
      futures.push_back(std::async(std::launch::async, [display = *found, flag, diagnostics,
          frozen_options, session_id=request.session_id, operation_id=request.operation_id] {
        const auto failure = [&](Error error) {
          error.safe_context.try_emplace("displayId", std::to_string(display.snapshot.id.value));
          return Result<NativeCaptureFrame, Error>::failure(std::move(error));
        };
        if (display.advanced_color_mode == 0 && !display.icc_profile)
          return failure(capture_error("legacy_profile_unavailable"));
        auto raw = windows_capture_display(display, flag.get(), 5000, diagnostics.get());
        if (!raw) return failure(raw.error());
        const auto scale = WindowsColor::scrgb_to_edr_scale(display.source_white);
        if (!scale) return failure(scale.error());
        const auto adjustment=resolve_capture_adjustment(frozen_options,
            display.source_white.hdr_active, scale.value());
        if (!adjustment) return failure(adjustment.error());
        record_capture_adjustment_diagnostics(diagnostics.get(), session_id, operation_id,
            display.snapshot.id.value, frozen_options, adjustment.value(),
            display.source_white.sdr_white_nits, scale.value(), display.source_white.hdr_active);
        auto linear = windows_normalize_scrgb_source(raw.value().size_px,
            std::move(raw.value().rgba_scrgb), adjustment.value().white_scale,
            adjustment.value().effective_gain,
            display.advanced_color_mode == 0 ? display.icc_profile : nullptr);
        if (!linear) return failure(linear.error());
        record_diagnostic_stage(diagnostics.get(), {}, {}, "windows.color", "capture.normalize", "success",
            {{"displayId", std::to_string(display.snapshot.id.value)},
             {"transfer", display.advanced_color_mode == 0 ? "legacy_icc_oetf" : "scrgb_matrix"},
             {"format", "rgba32f"}, {"sourceStorage", "rgba16f"}});
        NativeCaptureFrame frame{};
        frame.display_id = display.snapshot.id;
        frame.size_px = raw.value().size_px;
        frame.pixel_format = PixelFormat::rgba32_float;
        frame.encoding = WindowsColor::linear_p3_encoding();
        frame.linear_source = std::move(linear.value());
        frame.capture_sdr_tolerance = display.source_white.hdr_active;
        frame.capture_frozen_sdr = !display.source_white.hdr_active;
        return Result<NativeCaptureFrame, Error>::success(std::move(frame));
      }));
    }
    NativeFrameBatch batch{request.session_id, request.operation_id, request.display_generation, {}};
    for (auto& future : futures) {
      auto frame = future.get();
      if (!frame) return Result<NativeFrameBatch, Error>::failure(frame.error());
      batch.frames.push_back(std::move(frame.value()));
    }
    if (flag->load()) {
      return Result<NativeFrameBatch, Error>::failure({ErrorCode::operation_cancelled, "WindowsCapture", Retryability::never, {}});
    }
    return Result<NativeFrameBatch, Error>::success(std::move(batch));
    } catch(const std::exception&) {
      return Result<NativeFrameBatch,Error>::failure(capture_error("capture_worker_failure"));
    }
    };
    auto result=perform();
    if(flag->load()) result=Result<NativeFrameBatch,Error>::failure(
        {ErrorCode::operation_cancelled,"WindowsCapture",Retryability::never,{}});
    completion(std::move(result));
  }));
}
void WindowsCapturePort::cancel(SessionId session, OperationId operation) {
  const std::scoped_lock lock(impl_->mutex);
  if (const auto found = impl_->cancellations.find({session.value, operation.value}); found != impl_->cancellations.end()) found->second->store(true);
}
}  // namespace hdrshot
