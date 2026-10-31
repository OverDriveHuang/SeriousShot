#pragma once
#include "ports/login_startup_port.hpp"
namespace hdrshot {
class MacLoginStartupPort final : public LoginStartupPort {
 public:
  Result<LoginStartupSnapshot, Error> query() override;
  Result<bool, Error> set_enabled(bool enabled) override;
  Result<bool, Error> open_system_settings() override;
};
} // namespace hdrshot
