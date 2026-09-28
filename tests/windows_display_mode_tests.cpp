#include "platform/windows/windows_display_mode.hpp"
#include "test_support.hpp"

using namespace hdrshot;
namespace {
void info2_is_authoritative() {
  for (std::uint32_t mode = 0; mode <= 2; ++mode) {
    const auto selected = windows_choose_display_mode({0, mode, 0x5a, true});
    HDRSHOT_CHECK(selected.detected_mode == mode);
    HDRSHOT_CHECK(selected.effective_mode == mode);
    HDRSHOT_CHECK(std::string(selected.source) == "info2");
    HDRSHOT_CHECK(selected.info2_flags == 0x5a);
  }
}
void fallback_requires_failed_info2_and_exact_hdr_fact() {
  const auto hdr = windows_choose_display_mode({87, 0, 0, true});
  HDRSHOT_CHECK(hdr.detected_mode == 2 && hdr.effective_mode == 2);
  HDRSHOT_CHECK(std::string(hdr.source) == "dxgi_hdr");
  HDRSHOT_CHECK(hdr.info2_status == 87);
  const auto unknown = windows_choose_display_mode({87, 0, 0x17, false});
  HDRSHOT_CHECK(unknown.detected_mode == windows_mode_unknown);
  HDRSHOT_CHECK(unknown.effective_mode == 0);
  HDRSHOT_CHECK(std::string(unknown.reason) == "legacy_fallback");
  HDRSHOT_CHECK(unknown.info2_status == 87 && unknown.info2_flags == 0x17);
}
void invalid_success_value_stays_unknown() {
  const auto choice = windows_choose_display_mode({0, 9, 0xa5, true});
  HDRSHOT_CHECK(choice.detected_mode == windows_mode_unknown);
  HDRSHOT_CHECK(choice.effective_mode == 0);
  HDRSHOT_CHECK(std::string(choice.source) == "info2");
  HDRSHOT_CHECK(choice.info2_active_mode == 9);
}
void exact_target_selection_and_clone_ambiguity() {
  const std::vector<WindowsTargetPath> paths{{1,2,10,20,"\\\\.\\DISPLAY1"},
      {3,4,11,21,"\\\\.\\DISPLAY1"},{1,2,12,22,"\\\\.\\DISPLAY2"}};
  const auto exact = windows_match_target(1,2,"\\\\.\\DISPLAY1",paths);
  HDRSHOT_CHECK(exact.status == WindowsTargetMatchStatus::exact && exact.index == 0);
  const auto missing = windows_match_target(9,2,"\\\\.\\DISPLAY1",paths);
  HDRSHOT_CHECK(missing.status == WindowsTargetMatchStatus::missing);
  auto clones=paths;clones.push_back({1,2,10,23,"\\\\.\\DISPLAY1"});
  const auto ambiguous=windows_match_target(1,2,"\\\\.\\DISPLAY1",clones);
  HDRSHOT_CHECK(ambiguous.status == WindowsTargetMatchStatus::ambiguous);
  clones.back().target_id=20;
  const auto repeated=windows_match_target(1,2,"\\\\.\\DISPLAY1",clones);
  HDRSHOT_CHECK(repeated.status == WindowsTargetMatchStatus::exact);
}
void optional_border_does_not_weaken_cursor_contract() {
  const auto old = windows_plan_capture_capabilities(true, false, false);
  HDRSHOT_CHECK(old.can_exclude_cursor && !old.may_request_borderless);
  const auto no_cursor = windows_plan_capture_capabilities(false, true, true);
  HDRSHOT_CHECK(!no_cursor.can_exclude_cursor && no_cursor.may_request_borderless);
  const auto no_access = windows_plan_capture_capabilities(true, true, false);
  HDRSHOT_CHECK(no_access.can_exclude_cursor && !no_access.may_request_borderless);
}
}
int main() { return test::run({
    {"INFO2 success wins over DXGI", info2_is_authoritative},
    {"failed INFO2 selects target HDR or explicit Legacy fallback", fallback_requires_failed_info2_and_exact_hdr_fact},
    {"unknown INFO2 success is not manufactured HDR", invalid_success_value_stays_unknown},
    {"target identity and clone ambiguity", exact_target_selection_and_clone_ambiguity},
    {"optional border and mandatory cursor exclusion", optional_border_does_not_weaken_cursor_contract}}); }
