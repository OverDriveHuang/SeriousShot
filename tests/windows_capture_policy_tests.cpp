#include "platform/windows/windows_capture_policy.hpp"
#include "test_support.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace hdrshot;
namespace {

void all_switch_states_and_modes() {
  // The frozen mathematical anchor is C=2, Wc=204, stored gain=0.5.
  const float normal_scale=static_cast<float>(80.0 / 204.0);
  constexpr double anchors[2][2]={{40.0/51.0,2.0},{20.0/51.0,1.0}};
  for (bool gain_enabled : {false, true}) {
    for (bool bypass_enabled : {false, true}) {
      const WindowsCaptureOptions options{gain_enabled, bypass_enabled, 0.5};
      const auto hdr=resolve_capture_adjustment(options, true, normal_scale);
      HDRSHOT_CHECK(hdr.has_value());
      HDRSHOT_CHECK(hdr.value().effective_gain==(gain_enabled ? 0.5 : 1.0));
      HDRSHOT_CHECK(hdr.value().white_scale==(bypass_enabled ? 1.0F : normal_scale));
      const double expected=anchors[gain_enabled][bypass_enabled];
      const double actual=2.0 * hdr.value().effective_gain * hdr.value().white_scale;
      HDRSHOT_CHECK(std::abs(actual-expected)<1e-7);
      // Both SDR modes enter the policy with hdr_active=false.
      const auto sdr=resolve_capture_adjustment(options, false, normal_scale);
      HDRSHOT_CHECK(sdr.has_value());
      HDRSHOT_CHECK(sdr.value().white_scale==1.0F);
      HDRSHOT_CHECK(sdr.value().effective_gain==hdr.value().effective_gain);
    }
  }
}

void normal_scale_is_forwarded_exactly() {
  const float scale=std::bit_cast<float>(std::uint32_t{0x3eaaaaab});
  const auto old_default=resolve_capture_adjustment({false,false,0.5},true,scale);
  HDRSHOT_CHECK(old_default.has_value());
  HDRSHOT_CHECK(old_default.value().effective_gain==1.0);
  HDRSHOT_CHECK(std::bit_cast<std::uint32_t>(old_default.value().white_scale)==
                std::bit_cast<std::uint32_t>(scale));
  const auto legacy_manual=resolve_capture_adjustment({true,false,1.274008},true,scale);
  HDRSHOT_CHECK(legacy_manual.has_value());
  HDRSHOT_CHECK(legacy_manual.value().effective_gain==1.274008);
  HDRSHOT_CHECK(std::bit_cast<std::uint32_t>(legacy_manual.value().white_scale)==
                std::bit_cast<std::uint32_t>(scale));
  HDRSHOT_CHECK(resolve_capture_adjustment({false,true,0.5},true,scale).value().white_scale==1.0F);
}

void gain_bounds_and_invalid_inputs() {
  for (const double gain : {0.0, 0.5, 1.0, 1.274008, 3.0}) {
    const auto enabled=resolve_capture_adjustment({true,false,gain},true,0.4F);
    HDRSHOT_CHECK(enabled && enabled.value().effective_gain==gain);
    const auto disabled=resolve_capture_adjustment({false,false,gain},true,0.4F);
    HDRSHOT_CHECK(disabled && disabled.value().effective_gain==1.0);
  }
  for (const double gain : {-0.01, 3.01, std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN()})
    HDRSHOT_CHECK(!resolve_capture_adjustment({false,false,gain},true,0.4F));
  for (const float scale : {0.0F, -1.0F, std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::quiet_NaN()}) {
    HDRSHOT_CHECK(!resolve_capture_adjustment({false,true,0.5},true,scale));
    HDRSHOT_CHECK(!resolve_capture_adjustment({false,true,0.5},false,scale));
  }
}

}

int main() {
  return test::run({{"all switches and capture modes",all_switch_states_and_modes},
                    {"normal scale bit preservation",normal_scale_is_forwarded_exactly},
                    {"gain and scale validation",gain_bounds_and_invalid_inputs}});
}
