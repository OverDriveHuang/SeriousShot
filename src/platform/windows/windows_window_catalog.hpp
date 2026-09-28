#pragma once
#include "platform/windows/windows_capture.hpp"
#include "ports/window_catalog_port.hpp"
#include <chrono>
#include <memory>

namespace hdrshot {
enum class WindowsWindowRole { selectable, blocker, ignored };
struct WindowsWindowRecord {
  std::uint64_t token{};
  std::uint32_t process_id{};
  WindowRectF bounds_physical{};
  bool visible{}, cloaked{}, minimized{};
  bool topmost{}, tool_window{}, no_activate{};
};
[[nodiscard]] WindowsWindowRole
classify_windows_window(const WindowsWindowRecord &record,
                        std::uint32_t own_process_id);
[[nodiscard]] WindowSnapshot
map_windows_window_records(const SnapshotWindowsRequest &request,
                           const std::vector<WindowsWindowRecord> &records,
                           const std::vector<WindowsDisplayInfo> &displays,
                           std::uint32_t own_process_id);

class WindowsWindowCatalogPort final : public WindowCatalogPort {
public:
  using Enumerate =
      std::function<Result<std::vector<WindowsWindowRecord>, Error>()>;
  explicit WindowsWindowCatalogPort(
      std::shared_ptr<const std::vector<WindowsDisplayInfo>> displays = {},
      Enumerate enumerate = {},
      std::chrono::milliseconds timeout = std::chrono::milliseconds{250});
  ~WindowsWindowCatalogPort() override;
  void
  set_displays(std::shared_ptr<const std::vector<WindowsDisplayInfo>> displays);
  void snapshot_windows(const SnapshotWindowsRequest &, Completion) override;
  void cancel(SessionId, OperationId) override;

private:
  struct State;
  std::shared_ptr<State> state_;
};
} // namespace hdrshot
