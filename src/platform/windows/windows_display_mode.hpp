#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hdrshot {

// Windows-only query facts. A missing/invalid INFO_2 result must never become
// an apparent system report of SDR (zero).
inline constexpr std::uint32_t windows_mode_unknown = 3;

struct WindowsModeEvidence {
  std::uint32_t info2_status{};       // DisplayConfigGetDeviceInfo LONG result.
  std::uint32_t info2_active_mode{};  // Meaningful only when status is zero.
  std::uint32_t info2_flags{};        // Raw INFO_2 flags; diagnostic only.
  bool target_dxgi_hdr{};            // Output6 ColorSpace == G2084/P2020.
};

struct WindowsModeChoice {
  std::uint32_t detected_mode{windows_mode_unknown};
  std::uint32_t effective_mode{};     // 0 Legacy, 1 SDR ACM, 2 HDR.
  const char* source{"info2"};
  const char* reason{"legacy_fallback"};
  std::uint32_t info2_status{};
  std::uint32_t info2_active_mode{};
  std::uint32_t info2_flags{};
};

[[nodiscard]] WindowsModeChoice windows_choose_display_mode(const WindowsModeEvidence& evidence);

struct WindowsTargetPath {
  std::uint32_t adapter_low{};
  std::int32_t adapter_high{};
  std::uint32_t source_id{};
  std::uint32_t target_id{};
  std::string gdi_source_name;
};

enum class WindowsTargetMatchStatus { exact, missing, ambiguous };
struct WindowsTargetMatch {
  WindowsTargetMatchStatus status{WindowsTargetMatchStatus::missing};
  std::size_t index{};
};

// Same adapter LUID and GDI source name are both required. Clone targets with
// distinct target IDs remain ambiguous; never borrow a sibling's INFO_2 state.
[[nodiscard]] WindowsTargetMatch windows_match_target(
    std::uint32_t adapter_low, std::int32_t adapter_high,
    const std::string& gdi_source_name, const std::vector<WindowsTargetPath>& paths);

struct WindowsCaptureCapabilityPlan {
  bool can_exclude_cursor{};
  bool may_request_borderless{};
};
[[nodiscard]] WindowsCaptureCapabilityPlan windows_plan_capture_capabilities(
    bool cursor_property_present, bool border_property_present,
    bool border_access_method_present);

}  // namespace hdrshot
