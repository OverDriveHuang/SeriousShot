#include "application/login_startup_workflow.hpp"
namespace hdrshot {
LoginStartupChange LoginStartupWorkflow::change_enabled(bool enabled) {
  LoginStartupChange result;
  const auto before = port_.query();
  if (!before) {
    result.query_error = before.error();
    return result;
  }
  result.snapshot = before.value();
  const auto state = before.value().state;
  if ((enabled && state == LoginStartupState::enabled) ||
      (!enabled && state == LoginStartupState::disabled)) return result;
  if (!(enabled ? before.value().can_enable : before.value().can_disable)) {
    result.operation_error = Error{ErrorCode::precondition_failed, "login_startup",
        Retryability::after_user_action, {{"reason", "系统当前状态不允许此操作，请检查系统设置后再次勾选。"}}};
    return result;
  }
  const auto changed = port_.set_enabled(enabled);
  if (!changed) result.operation_error = changed.error();
  // Even a failed write may already have changed the system. Never compensate
  // with an inverse write or render the old checkbox optimistically.
  const auto after = port_.query();
  if (after) result.snapshot = after.value();
  else {
    result.snapshot.state = LoginStartupState::unknown;
    result.snapshot.can_enable = result.snapshot.can_disable = false;
    result.query_error = after.error();
  }
  return result;
}
} // namespace hdrshot
