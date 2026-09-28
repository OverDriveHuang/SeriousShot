#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "core/error.hpp"
#include "core/result.hpp"
#include <d3d11.h>
#include <winrt/base.h>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hdrshot {

// Frozen RGB matrix/TRC profile. All matrices include the single PCS D50 to
// Display P3 D65 adaptation; values are never quantized to device code values.
class WindowsIccProfile final {
public:
  struct Curve {
    // ICC parametricCurveType 0..4; -1 is identity; 5 is sampled curv.
    int type = -1;
    std::array<float, 7> parameters{};
    std::vector<float> samples;
  };
  const std::string sha256;
  const std::array<Curve, 3> decode_curves;
  const std::array<float, 9> device_to_p3;
  const std::array<float, 9> p3_to_device;

  WindowsIccProfile(std::string hash, std::array<Curve, 3> curves,
                    std::array<float, 9> forward, std::array<float, 9> inverse);
};

Result<std::shared_ptr<const WindowsIccProfile>, Error>
windows_load_icc_profile(const std::string &utf8_path);

// CPU reference for contract tests and small probes; rendering uses the GPU.
std::array<float, 3> windows_icc_device_to_p3_cpu(
    const WindowsIccProfile &profile, std::array<float, 3> encoded);
std::array<float, 3> windows_icc_p3_to_device_cpu(
    const WindowsIccProfile &profile, std::array<float, 3> linear_p3);

class WindowsIccGpuTransform final {
public:
  static Result<std::shared_ptr<const WindowsIccGpuTransform>, Error>
  create(ID3D11Device *device,
         std::shared_ptr<const WindowsIccProfile> profile);
  void bind_cs(ID3D11DeviceContext *context) const;
  void bind_ps(ID3D11DeviceContext *context) const;
  const WindowsIccProfile &profile() const { return *profile_; }

  // Prepend this source before a CS/PS entry point. Functions only perform
  // ICC conversion; gain, sRGB OETF/EOTF and display white belong to callers.
  static std::string_view hlsl_source();

private:
  WindowsIccGpuTransform(std::shared_ptr<const WindowsIccProfile> profile,
                         winrt::com_ptr<ID3D11Buffer> constants,
                         winrt::com_ptr<ID3D11ShaderResourceView> decode_table,
                         winrt::com_ptr<ID3D11ShaderResourceView> encode_table);
  std::shared_ptr<const WindowsIccProfile> profile_;
  winrt::com_ptr<ID3D11Buffer> constants_;
  winrt::com_ptr<ID3D11ShaderResourceView> decode_table_;
  winrt::com_ptr<ID3D11ShaderResourceView> encode_table_;
};

} // namespace hdrshot
