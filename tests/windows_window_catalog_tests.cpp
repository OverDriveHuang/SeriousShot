#include "platform/windows/windows_window_catalog.hpp"
#include "test_support.hpp"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace hdrshot;
namespace {
SnapshotWindowsRequest request() {
  return {{1},
          {2},
          {3,
           {{{1}, {0, 0, 1000, 500}, 2, {2000, 1000}},
            {{2}, {-1000, -500, 1000, 500}, 1, {1000, 500}}}}};
}
std::vector<WindowsDisplayInfo> displays() {
  std::vector<WindowsDisplayInfo> result(2);
  const auto requested = request();
  result[0].snapshot = requested.displays.displays[0];
  result[0].physical_x = 0;
  result[0].physical_y = 0;
  result[1].snapshot = requested.displays.displays[1];
  result[1].physical_x = -1000;
  result[1].physical_y = -500;
  return result;
}
void metadata_classification_and_mixed_scale_mapping() {
  const auto records = std::vector<WindowsWindowRecord>{
      {11, 99, {20, 40, 400, 200}, true},
      {12, 10, {20, 40, 400, 200}, true},                        // own process
      {13, 99, {20, 40, 400, 200}, true, true},                  // cloaked
      {14, 99, {-990, -490, 100, 50}, true, false, false, true}, // topmost
                                                                 // blocker
      {15, 99, {-20, -20, 40, 40}, true}, // clipped independently at the seam
      {16, 99, {100, 100, 30, 30}, true, false, true}};
  const auto snapshot =
      map_windows_window_records(request(), records, displays(), 10);
  HDRSHOT_CHECK(snapshot.candidates.size() == 4);
  HDRSHOT_CHECK(snapshot.candidates[0].bounds_px ==
                (PixelRect{20, 40, 400, 200}));
  HDRSHOT_CHECK(snapshot.candidates[1].display_id == DisplayId{2});
  HDRSHOT_CHECK(snapshot.candidates[1].bounds_px ==
                (PixelRect{10, 10, 100, 50}));
  HDRSHOT_CHECK(snapshot.candidates[1].role == WindowHitRole::blocker);
  HDRSHOT_CHECK(snapshot.candidates[2].display_id == DisplayId{1});
  HDRSHOT_CHECK(snapshot.candidates[2].bounds_px == (PixelRect{0, 0, 20, 20}));
  HDRSHOT_CHECK(snapshot.candidates[3].display_id == DisplayId{2});
  HDRSHOT_CHECK(snapshot.candidates[3].bounds_px ==
                (PixelRect{980, 480, 20, 20}));
  HDRSHOT_CHECK(window_at(snapshot, {1}, {10, 10}) ==
                (PixelRect{0, 0, 20, 20}));
}
void completion_is_once_on_timeout_and_late_query() {
  auto native =
      std::make_shared<const std::vector<WindowsDisplayInfo>>(displays());
  std::mutex mutex;
  std::condition_variable condition;
  int calls = 0;
  bool failed = false;
  WindowsWindowCatalogPort port(
      native,
      [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return Result<std::vector<WindowsWindowRecord>, Error>::success({});
      },
      std::chrono::milliseconds(5));
  port.snapshot_windows(request(), [&](auto outcome) {
    {
      const std::scoped_lock lock(mutex);
      failed = outcome.has_value();
      ++calls;
    }
    condition.notify_one();
  });
  {
    std::unique_lock lock(mutex);
    HDRSHOT_CHECK(condition.wait_for(lock, std::chrono::seconds(1),
                                     [&] { return calls == 1; }));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(70));
  {
    const std::scoped_lock lock(mutex);
    HDRSHOT_CHECK(calls == 1 && !failed);
  }
}
} // namespace
int main() {
  return hdrshot::test::run({
      {"metadata classification and mixed-scale mapping",
       metadata_classification_and_mixed_scale_mapping},
      {"metadata completion is once after timeout",
       completion_is_once_on_timeout_and_late_query},
  });
}
