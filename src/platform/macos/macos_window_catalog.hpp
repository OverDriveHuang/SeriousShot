#pragma once

#include "ports/window_catalog_port.hpp"

#include <chrono>
#include <string_view>

namespace hdrshot {
enum class MacWindowRole { selectable, blocker, ignored };
// Platform policy: identities are not localized display names or window titles.
[[nodiscard]] MacWindowRole classify_mac_window(
    std::string_view bundle_identifier, std::string_view executable_path, std::int32_t layer);
// A title-free projection of Quartz dictionaries, also usable by offline tests.
struct MacWindowRecord {
  std::uint64_t window_id{};
  std::int32_t owner_pid{}, layer{};
  WindowRectF bounds_points{};
  double alpha{1.0};
  bool on_screen{true};
  MacWindowRole role{MacWindowRole::selectable};
};
[[nodiscard]] WindowSnapshot map_mac_window_records(
    const SnapshotWindowsRequest&, const std::vector<MacWindowRecord>&, std::int32_t own_pid);

class MacWindowCatalogPort final : public WindowCatalogPort {
 public:
  using Enumerate = std::function<Result<std::vector<MacWindowRecord>, Error>()>;
  explicit MacWindowCatalogPort(
      Enumerate enumerate = {}, std::chrono::milliseconds timeout = std::chrono::milliseconds{250});
  ~MacWindowCatalogPort() override;
  void snapshot_windows(const SnapshotWindowsRequest&, Completion) override;
  void cancel(SessionId, OperationId) override;
 private:
  struct State;
  std::shared_ptr<State> state_;
};
} // namespace hdrshot
