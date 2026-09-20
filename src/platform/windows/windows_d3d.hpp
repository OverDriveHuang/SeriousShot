#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "core/error.hpp"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <winrt/base.h>
#include <cstring>
#include <iostream>

namespace hdrshot {
inline Error windows_d3d_error(const char* module, const char* reason, HRESULT code=E_FAIL) {
  return {ErrorCode::presenter_failed,module,Retryability::after_recreate,
      {{"reason",reason},{"hresult",std::to_string(static_cast<std::uint32_t>(code))}}};
}
struct WindowsD3DDevice {
  winrt::com_ptr<ID3D11Device> device;
  winrt::com_ptr<ID3D11DeviceContext> context;
  WindowsD3DDevice() {
    constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    winrt::check_hresult(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,2,D3D11_SDK_VERSION,device.put(),nullptr,context.put()));
  }
  winrt::com_ptr<ID3DBlob> compile(const char* source,const char* entry,const char* target) {
    winrt::com_ptr<ID3DBlob> blob,diagnostics;
    const auto hr=D3DCompile(source,std::strlen(source),"SeriousShot Windows shader",nullptr,nullptr,
        entry,target,D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_IEEE_STRICTNESS,0,blob.put(),diagnostics.put());
    if(FAILED(hr) && diagnostics) std::cerr.write(static_cast<const char*>(diagnostics->GetBufferPointer()),
        static_cast<std::streamsize>(diagnostics->GetBufferSize()));
    winrt::check_hresult(hr);
    return blob;
  }
  winrt::com_ptr<ID3D11Buffer> buffer(UINT bytes,UINT bind,UINT stride=0,const void* initial=nullptr) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth=bytes; desc.BindFlags=bind; desc.Usage=D3D11_USAGE_DEFAULT;
    if(stride) { desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; desc.StructureByteStride=stride; }
    const D3D11_SUBRESOURCE_DATA data{initial,0,0};
    winrt::com_ptr<ID3D11Buffer> result;
    winrt::check_hresult(device->CreateBuffer(&desc,initial?&data:nullptr,result.put()));
    return result;
  }
  template<class Resource>
  winrt::com_ptr<ID3D11ShaderResourceView> srv(const winrt::com_ptr<Resource>& resource) {
    winrt::com_ptr<ID3D11ShaderResourceView> view;
    winrt::check_hresult(device->CreateShaderResourceView(resource.get(),nullptr,view.put()));
    return view;
  }
  winrt::com_ptr<ID3D11UnorderedAccessView> uav(const winrt::com_ptr<ID3D11Buffer>& resource) {
    winrt::com_ptr<ID3D11UnorderedAccessView> view;
    winrt::check_hresult(device->CreateUnorderedAccessView(resource.get(),nullptr,view.put()));
    return view;
  }
  void read_buffer(const winrt::com_ptr<ID3D11Buffer>& source,void* output,UINT bytes) {
    D3D11_BUFFER_DESC desc{}; source->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0;
    desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; desc.MiscFlags=0; desc.StructureByteStride=0;
    winrt::com_ptr<ID3D11Buffer> staging;
    winrt::check_hresult(device->CreateBuffer(&desc,nullptr,staging.put()));
    context->CopyResource(staging.get(),source.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(context->Map(staging.get(),0,D3D11_MAP_READ,0,&mapped));
    std::memcpy(output,mapped.pData,bytes);
    context->Unmap(staging.get(),0);
  }
};
}  // namespace hdrshot
