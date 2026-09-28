#include "platform/windows/windows_display_transform.hpp"
#include "platform/windows/windows_d3d.hpp"
#include "test_support.hpp"
#include <cmath>

using namespace hdrshot;
namespace {
WindowsDisplayInfo display(std::uint32_t mode) {
  WindowsDisplayInfo d;
  d.advanced_color_mode = mode;
  d.source_white = {mode == 2, 203};
  return d;
}
std::shared_ptr<const WindowsIccProfile> identity_profile() {
  WindowsIccProfile::Curve curve;
  curve.type = 0;
  curve.parameters[0] = 2.2F;
  const std::array<float, 9> identity{1,0,0, 0,1,0, 0,0,1};
  return std::make_shared<const WindowsIccProfile>(
      "synthetic-identity-p3", std::array{curve,curve,curve},
      identity, identity);
}
void mode_and_profile_guards() {
  WindowsD3DDevice gpu;
  auto legacy = display(0);
  HDRSHOT_CHECK(!WindowsDisplayTransform::create(gpu.device.get(), legacy));
  legacy.icc_profile = identity_profile();
  HDRSHOT_CHECK(WindowsDisplayTransform::create(gpu.device.get(), legacy).has_value());
  HDRSHOT_CHECK(WindowsDisplayTransform::create(gpu.device.get(), display(1)).has_value());
  HDRSHOT_CHECK(WindowsDisplayTransform::create(gpu.device.get(), display(2)).has_value());
  auto invalid = display(3);
  HDRSHOT_CHECK(!WindowsDisplayTransform::create(gpu.device.get(), invalid));
  invalid = display(2); invalid.source_white = {false, 203};
  HDRSHOT_CHECK(!WindowsDisplayTransform::create(gpu.device.get(), invalid));
}
void mode_math_and_shader_contract() {
  WindowsD3DDevice gpu;
  auto legacy = display(0); legacy.icc_profile = identity_profile();
  const auto l = WindowsDisplayTransform::create(gpu.device.get(), legacy).value();
  const auto a = WindowsDisplayTransform::create(gpu.device.get(), display(1)).value();
  const auto h = WindowsDisplayTransform::create(gpu.device.get(), display(2)).value();
  const std::array<float,3> gray{0.25F,0.25F,0.25F};
  const auto code = std::pow(0.25F, 1/2.2F);
  const auto expected = std::pow((code + 0.055F)/1.055F, 2.4F);
  HDRSHOT_CHECK_NEAR(l->reference_cpu(gray)[0], expected, 0.00001);
  HDRSHOT_CHECK_NEAR(a->reference_cpu(gray)[0], 0.25, 0.00001);
  HDRSHOT_CHECK_NEAR(h->reference_cpu(gray)[0], 0.25*203/80, 0.00001);
  const auto extended = a->reference_cpu({-0.25F, 2, 12});
  HDRSHOT_CHECK(extended[0] < 0 && extended[2] > 1);

  const std::string shader = std::string(WindowsIccGpuTransform::hlsl_source()) +
      std::string(WindowsDisplayTransform::hlsl_source()) +
      "float4 ps():SV_Target{return float4(windows_present_p3(float3(.25,.25,.25)),1);}";
  HDRSHOT_CHECK(gpu.compile(shader.c_str(), "ps", "ps_5_0").get() != nullptr);
  l->bind_ps(gpu.context.get());
  a->bind_ps(gpu.context.get());
  h->bind_ps(gpu.context.get());
}
}
int main() {
  return test::run({{"mode and ICC guards",mode_and_profile_guards},
                    {"mode math and shader contract",mode_math_and_shader_contract}});
}
