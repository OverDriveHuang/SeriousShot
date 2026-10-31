#pragma once
#include "core/error.hpp"
#include "core/result.hpp"
#include <string_view>
#include <vector>
namespace hdrshot {
enum class WindowsLaunchOrigin { manual, login };
struct WindowsLaunchOptions {
  WindowsLaunchOrigin origin{WindowsLaunchOrigin::manual};
  std::vector<std::string_view> capture_arguments;
};
Result<WindowsLaunchOptions, Error> parse_windows_launch_options(const std::vector<std::string_view>& args);
}
