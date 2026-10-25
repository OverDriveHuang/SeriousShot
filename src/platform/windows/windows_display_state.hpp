#pragma once
#include "platform/windows/windows_capture.hpp"
#include <cstdint>
#include <memory>

namespace hdrshot {
// Immutable UI-to-renderer value; no Qt or live service dependency.
struct WindowsDisplayState {
  WindowsDisplayInfo display;
  std::uint64_t generation{};
  std::uint64_t topology_generation{};
};
using WindowsDisplayStateRef = std::shared_ptr<const WindowsDisplayState>;
}  // namespace hdrshot
