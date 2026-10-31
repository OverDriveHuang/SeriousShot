#include "platform/windows/windows_launch_options.hpp"
namespace hdrshot {
Result<WindowsLaunchOptions, Error> parse_windows_launch_options(const std::vector<std::string_view>& args) {
  WindowsLaunchOptions result;
  bool login=false;
  for(std::size_t i=0;i<args.size();++i) {
    if(args[i]=="--login-startup") {
      if(login) return Result<WindowsLaunchOptions,Error>::failure({ErrorCode::invalid_input,"WindowsLaunch",Retryability::after_user_action,{{"reason","--login-startup 不能重复或与截图、帮助、smoke 参数混用。"}}});
      login=true;
    } else if(args[i]=="--update-receipt" && i+2<args.size()) i+=2;
    else result.capture_arguments.push_back(args[i]);
  }
  if(login && !result.capture_arguments.empty()) return Result<WindowsLaunchOptions,Error>::failure({ErrorCode::invalid_input,"WindowsLaunch",Retryability::after_user_action,{{"reason","--login-startup 只能单独用于登录启动。"}}});
  result.origin=login?WindowsLaunchOrigin::login:WindowsLaunchOrigin::manual;
  return Result<WindowsLaunchOptions,Error>::success(std::move(result));
}
}
