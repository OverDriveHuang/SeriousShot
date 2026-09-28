#include "platform/windows/windows_display_transform.hpp"
#include "platform/windows/windows_d3d.hpp"
#include <cmath>

namespace hdrshot {
namespace {
struct alignas(16) DisplayConstants {
  std::uint32_t mode{};
  float white_scale{1};
  float padding[2]{};
};

Error transform_error(const char *reason, HRESULT hr = E_FAIL) {
  return windows_d3d_error("WindowsDisplayTransform", reason, hr);
}
float srgb_eotf(float code) {
  return code <= 0.04045F ? code / 12.92F
                           : std::pow((code + 0.055F) / 1.055F, 2.4F);
}
} // namespace

WindowsDisplayTransform::WindowsDisplayTransform(
    std::uint32_t mode, float white_scale,
    std::shared_ptr<const WindowsIccProfile> profile,
    std::shared_ptr<const WindowsIccGpuTransform> icc,
    winrt::com_ptr<ID3D11Buffer> constants)
    : mode_(mode), white_scale_(white_scale), profile_(std::move(profile)),
      icc_(std::move(icc)), constants_(std::move(constants)) {}

Result<std::shared_ptr<const WindowsDisplayTransform>, Error>
WindowsDisplayTransform::create(ID3D11Device *device,
                                const WindowsDisplayInfo &display) {
  using Out = Result<std::shared_ptr<const WindowsDisplayTransform>, Error>;
  if (!device || display.advanced_color_mode > 2)
    return Out::failure(transform_error("invalid_device_or_mode"));
  if (display.advanced_color_mode == 0 && !display.icc_profile)
    return Out::failure(transform_error("legacy_icc_unavailable"));
  if (display.advanced_color_mode == 2 && !display.source_white.hdr_active)
    return Out::failure(transform_error("hdr_white_unavailable"));

  float white_scale = 1;
  if (display.advanced_color_mode == 2) {
    auto scale = WindowsColor::scrgb_to_edr_scale(display.source_white);
    if (!scale)
      return Out::failure(scale.error());
    white_scale = 1 / scale.value();
  }
  std::shared_ptr<const WindowsIccGpuTransform> icc;
  if (display.advanced_color_mode == 0) {
    auto created = WindowsIccGpuTransform::create(device, display.icc_profile);
    if (!created)
      return Out::failure(created.error());
    icc = std::move(created.value());
  }
  const DisplayConstants params{display.advanced_color_mode, white_scale, {}};
  D3D11_BUFFER_DESC desc{};
  desc.ByteWidth = sizeof(params);
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  const D3D11_SUBRESOURCE_DATA initial{&params, 0, 0};
  winrt::com_ptr<ID3D11Buffer> constants;
  const auto hr = device->CreateBuffer(&desc, &initial, constants.put());
  if (FAILED(hr))
    return Out::failure(transform_error("gpu_constants", hr));
  return Out::success(std::shared_ptr<const WindowsDisplayTransform>(
      new WindowsDisplayTransform(display.advanced_color_mode, white_scale,
                                  display.icc_profile, std::move(icc),
                                  std::move(constants))));
}

void WindowsDisplayTransform::bind_ps(ID3D11DeviceContext *context) const {
  ID3D11Buffer *buffer[]{constants_.get()};
  context->PSSetConstantBuffers(3, 1, buffer);
  if (icc_)
    icc_->bind_ps(context);
  else {
    ID3D11Buffer *empty[]{nullptr};
    context->PSSetConstantBuffers(2, 1, empty);
  }
}

std::array<float, 3>
WindowsDisplayTransform::reference_cpu(std::array<float, 3> p3) const {
  if (mode_ == 0) {
    auto code = windows_icc_p3_to_device_cpu(*profile_, p3);
    for (auto &v : code)
      v = srgb_eotf(v);
    return code;
  }
  return WindowsColor::linear_p3_to_scrgb(p3, white_scale_);
}

std::string_view WindowsDisplayTransform::hlsl_source() {
  return R"HLSL(
cbuffer WindowsDisplayParams : register(b3) {
  uint windows_display_mode;
  float windows_display_white_scale;
  float2 windows_display_padding;
};
float windows_srgb_eotf(float code) {
  return code <= 0.04045 ? code / 12.92 : pow((code + 0.055) / 1.055, 2.4);
}
float3 windows_present_p3(float3 p3) {
  if (windows_display_mode == 0) {
    float3 code = windows_icc_p3_to_device(p3);
    return float3(windows_srgb_eotf(code.r), windows_srgb_eotf(code.g),
                  windows_srgb_eotf(code.b));
  }
  float3 scrgb = float3(1.22494018*p3.r - 0.22494018*p3.g,
                       -0.04205695*p3.r + 1.04205695*p3.g,
                       -0.01963755*p3.r - 0.07863605*p3.g + 1.09827360*p3.b);
  return scrgb * windows_display_white_scale;
}
)HLSL";
}
} // namespace hdrshot
