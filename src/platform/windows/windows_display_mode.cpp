#include "platform/windows/windows_display_mode.hpp"

namespace hdrshot {

WindowsModeChoice windows_choose_display_mode(const WindowsModeEvidence& evidence) {
  WindowsModeChoice choice{};
  choice.info2_status = evidence.info2_status;
  choice.info2_active_mode = evidence.info2_active_mode;
  choice.info2_flags = evidence.info2_flags;
  if (evidence.info2_status == 0 && evidence.info2_active_mode <= 2) {
    choice.detected_mode = evidence.info2_active_mode;
    choice.effective_mode = evidence.info2_active_mode;
    choice.source = "info2";
    choice.reason = "confirmed";
  } else if (evidence.info2_status != 0 && evidence.target_dxgi_hdr) {
    choice.detected_mode = 2;
    choice.effective_mode = 2;
    choice.source = "dxgi_hdr";
    choice.reason = "info2_unavailable";
  }
  return choice;
}

WindowsTargetMatch windows_match_target(const std::uint32_t adapter_low,
    const std::int32_t adapter_high, const std::string& gdi_source_name,
    const std::vector<WindowsTargetPath>& paths) {
  WindowsTargetMatch match{};
  bool found = false;
  std::uint32_t found_target = 0;
  for (std::size_t i = 0; i < paths.size(); ++i) {
    const auto& path = paths[i];
    if (path.adapter_low != adapter_low || path.adapter_high != adapter_high ||
        path.gdi_source_name != gdi_source_name) continue;
    if (!found) {
      found = true;
      found_target = path.target_id;
      match.status = WindowsTargetMatchStatus::exact;
      match.index = i;
    } else if (path.target_id != found_target) {
      match.status = WindowsTargetMatchStatus::ambiguous;
      return match;
    }
  }
  return match;
}

WindowsCaptureCapabilityPlan windows_plan_capture_capabilities(
    const bool cursor_property_present, const bool border_property_present,
    const bool border_access_method_present) {
  return {cursor_property_present,
      border_property_present && border_access_method_present};
}

}  // namespace hdrshot
