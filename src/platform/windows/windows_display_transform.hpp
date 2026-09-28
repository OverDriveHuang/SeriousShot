#pragma once

#include "platform/windows/windows_capture.hpp"
#include "platform/windows/windows_icc_transform.hpp"
#include <d3d11.h>
#include <winrt/base.h>
#include <array>
#include <memory>
#include <string_view>

namespace hdrshot {

// Converts the composited relative Linear Display P3 pixel to the FP16/G10
// DirectComposition surface for one frozen target display.
class WindowsDisplayTransform final {
public:
  static Result<std::shared_ptr<const WindowsDisplayTransform>, Error>
  create(ID3D11Device *device, const WindowsDisplayInfo &display);
  void bind_ps(ID3D11DeviceContext *context) const;
  static std::string_view hlsl_source();

  std::uint32_t effective_mode() const noexcept { return mode_; }
  float white_scale() const noexcept { return white_scale_; }
  std::array<float, 3> reference_cpu(std::array<float, 3> p3) const;

private:
  WindowsDisplayTransform(std::uint32_t mode, float white_scale,
                          std::shared_ptr<const WindowsIccProfile> profile,
                          std::shared_ptr<const WindowsIccGpuTransform> icc,
                          winrt::com_ptr<ID3D11Buffer> constants);
  std::uint32_t mode_{};
  float white_scale_{1};
  std::shared_ptr<const WindowsIccProfile> profile_;
  std::shared_ptr<const WindowsIccGpuTransform> icc_;
  winrt::com_ptr<ID3D11Buffer> constants_;
};
} // namespace hdrshot
