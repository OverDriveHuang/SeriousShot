#include "application/overlay_input_recovery.hpp"
#include "test_support.hpp"
using namespace hdrshot;
namespace {
using E = RecoveryPointerEvent;
using A = RecoveryPointerAction;
void normal_input_and_complete_recovery_drag() {
  OverlayInputRecovery state;
  HDRSHOT_CHECK(state.pointer(E::press) == A::forward);
  HDRSHOT_CHECK(state.pointer(E::release) == A::forward);
  state.interrupt();
  state.interrupt();
  HDRSHOT_CHECK(state.pointer(E::move) == A::consume);
  HDRSHOT_CHECK(state.pointer(E::press) == A::restore_focus);
  for (int i = 0; i < 100; ++i) HDRSHOT_CHECK(state.pointer(E::move) == A::consume);
  HDRSHOT_CHECK(state.pointer(E::release) == A::consume);
  HDRSHOT_CHECK(state.pointer(E::press) == A::forward);
  HDRSHOT_CHECK(state.pointer(E::release) == A::forward);
  HDRSHOT_CHECK(state.pointer(E::double_click) == A::forward);
}
void recovery_click_cannot_pair_with_second_click_to_complete() {
  OverlayInputRecovery state;
  state.interrupt();
  HDRSHOT_CHECK(state.pointer(E::double_click) == A::restore_focus);
  HDRSHOT_CHECK(state.pointer(E::release) == A::consume);
  HDRSHOT_CHECK(state.pointer(E::double_click) == A::ordinary_press);
  HDRSHOT_CHECK(state.pointer(E::release) == A::forward);
  HDRSHOT_CHECK(state.pointer(E::press) == A::forward);
  HDRSHOT_CHECK(state.pointer(E::double_click) == A::forward);
}
void second_interruption_during_recovery_still_requires_recovery() {
  OverlayInputRecovery state;
  state.interrupt();
  HDRSHOT_CHECK(state.pointer(E::press) == A::restore_focus);
  state.interrupt();
  HDRSHOT_CHECK(state.pointer(E::release) == A::consume);
  HDRSHOT_CHECK(state.pointer(E::press) == A::restore_focus);
  HDRSHOT_CHECK(state.pointer(E::release) == A::consume);
  HDRSHOT_CHECK(state.pointer(E::press) == A::forward);
}
void lost_release_does_not_leave_recovery_stuck() {
  OverlayInputRecovery state;
  state.interrupt();
  HDRSHOT_CHECK(state.pointer(E::press) == A::restore_focus);
  state.interrupt(); // OS took the grab; the old release never arrives.
  HDRSHOT_CHECK(state.pointer(E::press) == A::restore_focus);
  HDRSHOT_CHECK(state.pointer(E::release) == A::consume);
  HDRSHOT_CHECK(state.pointer(E::press) == A::forward);
}
}
int main() {
  return hdrshot::test::run({
      {"normal input and full recovery gesture", normal_input_and_complete_recovery_drag},
      {"recovery click is not part of completion double click", recovery_click_cannot_pair_with_second_click_to_complete},
      {"lost release does not strand recovery", lost_release_does_not_leave_recovery_stuck},
      {"interruption during recovery remains latched", second_interruption_during_recovery_still_requires_recovery}});
}
