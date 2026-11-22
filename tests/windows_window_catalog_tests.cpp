#include "platform/windows/windows_window_catalog.hpp"
#include "test_support.hpp"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <future>
#include <windows.h>

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
void own_settings_exception_and_session_isolation() {
  const auto pid = GetCurrentProcessId();
  const std::vector<WindowsWindowRecord> records{
      {10, pid, {10, 20, 200, 100}, true}, {11, pid, {20, 30, 50, 50}, true},
      {12, 99, {0, 0, 500, 500}, true}};
  HDRSHOT_CHECK(map_windows_window_records(request(), records, displays(), pid).candidates.size() == 1);
  auto selected = map_windows_window_records(request(), records, displays(), pid, 10);
  HDRSHOT_CHECK(selected.candidates.size() == 2);
  HDRSHOT_CHECK(window_at(selected, {1}, {15, 25}) == (PixelRect{10, 20, 200, 100}));
  auto front = records; front.insert(front.begin(), {14, 90, {10, 20, 50, 50}, true, false, false, true});
  HDRSHOT_CHECK(!window_at(map_windows_window_records(request(), front, displays(), pid, 10), {1}, {15, 25}));
  front[0].topmost = false;
  HDRSHOT_CHECK(window_at(map_windows_window_records(request(), front, displays(), pid, 10), {1}, {15, 25}) == (PixelRect{10, 20, 50, 50}));
  for (int flag = 0; flag < 4; ++flag) {
    auto r = records[0]; if (flag == 0) r.visible = false; if (flag == 1) r.cloaked = true;
    if (flag == 2) r.minimized = true; if (flag == 3) r.bounds_physical.width = 0;
    HDRSHOT_CHECK(classify_windows_window(r, pid, 10) == WindowsWindowRole::ignored);
  }
  auto blocker = records[0]; blocker.topmost = true;
  HDRSHOT_CHECK(classify_windows_window(blocker, pid, 10) == WindowsWindowRole::blocker);
  struct Fixture {
    std::mutex mutex; std::condition_variable cv; bool started = false, released = false;
    std::vector<WindowsWindowRecord> records;
    void release() { { std::scoped_lock lock(mutex); released = true; } cv.notify_all(); }
  };
  auto fixture = std::make_shared<Fixture>(); fixture->records = records;
  struct ReleaseOnExit { std::shared_ptr<Fixture> fixture; ~ReleaseOnExit() { fixture->release(); } } guard{fixture};
  auto native = std::make_shared<const std::vector<WindowsDisplayInfo>>(displays());
  WindowsWindowCatalogPort port(native, [fixture] { std::unique_lock lock(fixture->mutex); fixture->started = true; fixture->cv.notify_all();
    fixture->cv.wait_for(lock, std::chrono::seconds(2), [&] { return fixture->released; });
    return Result<std::vector<WindowsWindowRecord>, Error>::success(fixture->records); }, std::chrono::milliseconds(1000));
  port.set_displays(native, {1}, 10);
  auto promise = std::make_shared<std::promise<Result<WindowSnapshot, Error>>>(); auto future = promise->get_future();
  port.snapshot_windows(request(), [promise](auto result) { promise->set_value(std::move(result)); });
  { std::unique_lock lock(fixture->mutex); HDRSHOT_CHECK(fixture->cv.wait_for(lock, std::chrono::seconds(1), [&] { return fixture->started; })); }
  port.set_displays(native, {2}, 11); fixture->release();
  HDRSHOT_CHECK(future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  HDRSHOT_CHECK(future.get().value().candidates[0].token == 10);
  auto query = [&](SessionId id) { auto req = request(); req.session_id = id;
    auto p = std::make_shared<std::promise<Result<WindowSnapshot, Error>>>(); auto f = p->get_future();
    port.snapshot_windows(req, [p](auto r) { p->set_value(std::move(r)); });
    HDRSHOT_CHECK(f.wait_for(std::chrono::seconds(2)) == std::future_status::ready); return f.get().value(); };
  HDRSHOT_CHECK(query({2}).candidates[0].token == 11);
  HDRSHOT_CHECK(query({3}).candidates.size() == 1);
  port.set_displays(native, {2}, {}); HDRSHOT_CHECK(query({2}).candidates.size() == 1);
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
      {"settings exception and session isolation", own_settings_exception_and_session_isolation},
      {"metadata classification and mixed-scale mapping",
       metadata_classification_and_mixed_scale_mapping},
      {"metadata completion is once after timeout",
       completion_is_once_on_timeout_and_late_query},
  });
}
