#pragma once
#include "ports/login_startup_port.hpp"
namespace hdrshot {
struct LoginStartupChange {
  LoginStartupSnapshot snapshot;
  std::optional<Error> operation_error;
  std::optional<Error> query_error;
};
class LoginStartupWorkflow final {
 public:
  explicit LoginStartupWorkflow(LoginStartupPort& port) : port_(port) {}
  Result<LoginStartupSnapshot, Error> query() { return port_.query(); }
  LoginStartupChange change_enabled(bool enabled);
 private:
  LoginStartupPort& port_;
};
} // namespace hdrshot
