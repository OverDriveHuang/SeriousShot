#include "platform/windows/windows_launch_options.hpp"
#include "test_support.hpp"
using namespace hdrshot;
int main() {
  HDRSHOT_CHECK(parse_windows_launch_options({}).value().origin==WindowsLaunchOrigin::manual);
  HDRSHOT_CHECK(parse_windows_launch_options({"--login-startup"}).value().origin==WindowsLaunchOrigin::login);
  HDRSHOT_CHECK(parse_windows_launch_options({"--login-startup","--update-receipt","receipt","nonce"}).value().capture_arguments.empty());
  HDRSHOT_CHECK(parse_windows_launch_options({"--help"}).value().capture_arguments==std::vector<std::string_view>{"--help"});
  HDRSHOT_CHECK(parse_windows_launch_options({"--capture","--output","a.png"}).value().origin==WindowsLaunchOrigin::manual);
  for(const auto flag:{"--login-startup","--help","--capture","--smoke","--smoke-copy","--smoke-sequence","--update-receipt"})
    HDRSHOT_CHECK(!parse_windows_launch_options({"--login-startup",flag}));
  HDRSHOT_CHECK(parse_windows_launch_options({"--smoke","folder"}).value().capture_arguments.size()==2);
  HDRSHOT_CHECK(parse_windows_launch_options({"--update-receipt","path","nonce"}).value().capture_arguments.empty());
}
