#pragma once
#include "core/error.hpp"
#include "core/result.hpp"
#include <optional>
#include <string>
namespace hdrshot {
enum class LoginStartupState {
  disabled, enabled, requires_approval, blocked, service_not_found, invalid_target, unknown
};
struct LoginStartupSnapshot {
  LoginStartupState state{LoginStartupState::unknown};
  std::string current_bundle_path;
  // Only populate when the OS actually supplies its registered target path.
  std::optional<std::string> system_registered_target_path;
  bool can_enable{};
  bool can_disable{};
  bool can_open_system_settings{};
  std::string diagnostic;
  std::string approval_message;
};
class LoginStartupPort {
 public:
  virtual ~LoginStartupPort() = default;
  virtual Result<LoginStartupSnapshot, Error> query() = 0;
  virtual Result<bool, Error> set_enabled(bool enabled) = 0;
  virtual Result<bool, Error> open_system_settings() = 0;
};
} // namespace hdrshot
