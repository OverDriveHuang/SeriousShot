#include "platform/macos/macos_window_catalog.hpp"
#include "test_support.hpp"

#include <CoreGraphics/CGWindowLevel.h>

#include <condition_variable>
#include <future>
#include <thread>

using namespace hdrshot;
using namespace std::chrono_literals;
namespace {
SnapshotWindowsRequest request() {
  return {{1}, {2}, {3, {
      {{1}, {0, 0, 1000, 500}, 2, {2000, 1000}},
      {{2}, {-1000, -500, 1000, 500}, 1, {1000, 500}}}}};
}
void native_records_map_without_titles_or_shadow_expansion() {
  std::vector<MacWindowRecord> records{
      {1, 99, 0, {10, 20, 200, 100}},
      {2, 10, 0, {20, 30, 100, 50}}, // own process
      {3, 99, 0, {20, 30, 100, 50}, 0}, // transparent
      {4, 99, 0, {20, 30, 100, 50}, 1, false},
      {5, 99, -1, {20, 30, 100, 50}},
      {6, 99, 10, {-990, -490, 100, 50}, 1, true, MacWindowRole::blocker},
      {7, 99, 5, {50, 50, 30, 30}}}; // app floating window must not be excluded
  auto result = map_mac_window_records(request(), records, 10);
  HDRSHOT_CHECK(result.candidates.size() == 3);
  HDRSHOT_CHECK(result.candidates[0].bounds_px == (PixelRect{20, 40, 400, 200}));
  HDRSHOT_CHECK(result.candidates[1].bounds_px == (PixelRect{10, 10, 100, 50}));
  HDRSHOT_CHECK(result.candidates[1].role == WindowHitRole::blocker);
  HDRSHOT_CHECK(result.candidates[2].front_to_back_order == 6);
}
void stable_identity_roles_preserve_front_to_back_applications() {
  // No localized name is part of the classifier's input, on Chinese or English
  // systems. Same geometry for a desktop layer and a genuine full-screen app.
  const auto role = [](const char* id, int layer) { return classify_mac_window(id, "", layer); };
  HDRSHOT_CHECK(role("com.apple.screencaptureui", 1000) == MacWindowRole::ignored);
  HDRSHOT_CHECK(role("dev.hdrshot.desktop", 1000) == MacWindowRole::ignored);
  HDRSHOT_CHECK(role("com.apple.dock", 0) == MacWindowRole::ignored);
  HDRSHOT_CHECK(role("com.apple.dock", 20) == MacWindowRole::ignored);
  HDRSHOT_CHECK(role("com.apple.controlcenter", 25) == MacWindowRole::selectable);
  HDRSHOT_CHECK(role("com.apple.systemuiserver", 25) == MacWindowRole::selectable);
  HDRSHOT_CHECK(role("com.apple.systemuiserver", 1000) == MacWindowRole::blocker);
  HDRSHOT_CHECK(role("com.apple.loginwindow", 25) == MacWindowRole::blocker);
  HDRSHOT_CHECK(role("com.apple.finder", 0) == MacWindowRole::selectable);
  HDRSHOT_CHECK(role("org.example.floating", 5) == MacWindowRole::selectable);
  HDRSHOT_CHECK(classify_mac_window("", "", 0) == MacWindowRole::selectable);
  HDRSHOT_CHECK(classify_mac_window("",
      "/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock", 0) == MacWindowRole::ignored);
  HDRSHOT_CHECK(classify_mac_window("", "/Applications/Dock.app/Contents/MacOS/Dock", 0) ==
      MacWindowRole::selectable);
  const WindowRectF full{0, 0, 1000, 500};
  const auto snapshot = map_mac_window_records(request(), {
      {1, 99, 1000, full, 1, true, role("com.apple.screencaptureui", 1000)},
      {2, 99, 0, full, 1, true, role("com.apple.dock", 0)},
      {3, 99, 20, {0, 0, 1000, 20}, 1, true, role("com.apple.dock", 20)},
      {4, 99, 5, {100, 100, 200, 100}},
      {5, 99, 0, full},
      {6, 99, 0, {50, 50, 400, 300}}}, 10);
  HDRSHOT_CHECK(snapshot.candidates.size() == 3);
  HDRSHOT_CHECK(window_at(snapshot, {1}, {200, 10}) == (PixelRect{0, 0, 2000, 1000}));
  HDRSHOT_CHECK(window_at(snapshot, {1}, {250, 250}) == (PixelRect{200, 200, 400, 200}));
  HDRSHOT_CHECK(window_at(snapshot, {1}, {900, 600}) == (PixelRect{0, 0, 2000, 1000}));
}
void system_menu_bar_and_status_items_use_real_z_order() {
  constexpr const char* server = "/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/Resources/WindowServer";
  HDRSHOT_CHECK(classify_mac_window("", server, 0) == MacWindowRole::ignored);
  HDRSHOT_CHECK(classify_mac_window("", server, 24) == MacWindowRole::selectable);
  HDRSHOT_CHECK(classify_mac_window("", server, 1000) == MacWindowRole::blocker);
  for (const int layer : {24, 25, 101})
    HDRSHOT_CHECK(classify_mac_window("com.apple.systemuiserver", "", layer) == MacWindowRole::selectable);
  HDRSHOT_CHECK(classify_mac_window("", "/System/Library/CoreServices/ControlCenter.app/Contents/MacOS/ControlCenter", 25)
      == MacWindowRole::selectable);
  const auto snapshot = map_mac_window_records(request(), {
      {1, 20, 25, {900, 0, 50, 20}, 1, true, classify_mac_window("com.apple.controlcenter", "", 25)},
      {2, 21, 24, {0, 0, 1000, 20}, 1, true, classify_mac_window("", server, 24)},
      {3, 22, 0, {0, 0, 1000, 500}},
      {4, 23, 0, {0, 0, 1000, 500}, 1, true, classify_mac_window("com.apple.dock", "", 0)}}, 10);
  HDRSHOT_CHECK(window_at(snapshot, {1}, {1850, 10}) == (PixelRect{1800, 0, 100, 40}));
  HDRSHOT_CHECK(window_at(snapshot, {1}, {500, 10}) == (PixelRect{0, 0, 2000, 40}));
  HDRSHOT_CHECK(window_at(snapshot, {1}, {500, 200}) == (PixelRect{0, 0, 2000, 1000}));
}
void dock_carrier_is_ignored_on_retina_and_negative_origin_displays() {
  // Synthetic Quartz records, not a claim about a live/locked desktop. The
  // carrier reports alpha=1 even though most pixels can be transparent.
  const auto dock_level = CGWindowLevelForKey(kCGDockWindowLevelKey);
  const auto snapshot = map_mac_window_records(request(), {
      {1, 90, dock_level, {-1000, -500, 2000, 1000}, 1, true,
          classify_mac_window("com.apple.dock", "", dock_level)},
      {2, 91, 5, {100, 100, 200, 100}},
      {3, 92, 0, {0, 0, 1000, 500}},
      {4, 93, 0, {110, 110, 20, 20}}, // smaller but behind the two apps
      {5, 94, 0, {-950, -450, 400, 300}}}, 10);
  HDRSHOT_CHECK(snapshot.candidates.size() == 4);
  for (std::size_t i = 0; i < snapshot.candidates.size(); ++i)
    HDRSHOT_CHECK(snapshot.candidates[i].front_to_back_order == i + 1);
  HDRSHOT_CHECK(window_at(snapshot, {1}, {240, 240}) == (PixelRect{200, 200, 400, 200}));
  HDRSHOT_CHECK(window_at(snapshot, {1}, {900, 600}) == (PixelRect{0, 0, 2000, 1000}));
  HDRSHOT_CHECK(window_at(snapshot, {2}, {100, 100}) == (PixelRect{50, 50, 400, 300}));
  HDRSHOT_CHECK(!window_at(snapshot, {2}, {900, 400}));

  WindowSelectionGesture gesture(std::make_shared<const WindowSnapshot>(snapshot), {1}, {2000, 1000});
  const SelectionPointer pointer{{240, 240}, 120, 120, true};
  gesture.move(pointer);
  HDRSHOT_CHECK(gesture.preview().kind == PreviewHighlightKind::window_candidate);
  HDRSHOT_CHECK(gesture.preview().rect == (PixelRect{200, 200, 400, 200}));
  gesture.press(pointer);
  HDRSHOT_CHECK(gesture.release(pointer) == (PixelRect{200, 200, 400, 200}));
}
void dock_menus_are_not_removed_with_the_carrier() {
  const auto dock_level = CGWindowLevelForKey(kCGDockWindowLevelKey);
  for (const auto key : {kCGMainMenuWindowLevelKey, kCGStatusWindowLevelKey, kCGPopUpMenuWindowLevelKey}) {
    const auto menu_level = CGWindowLevelForKey(key);
    HDRSHOT_CHECK(classify_mac_window("com.apple.dock", "", menu_level) == MacWindowRole::selectable);
    const auto snapshot = map_mac_window_records(request(), {
        {1, 90, menu_level, {100, 100, 100, 50}, 1, true,
            classify_mac_window("com.apple.dock", "", menu_level)},
        {2, 90, dock_level, {0, 0, 1000, 500}, 1, true,
            classify_mac_window("com.apple.dock", "", dock_level)},
        {3, 91, 0, {0, 0, 1000, 500}}}, 10);
    HDRSHOT_CHECK(snapshot.candidates.size() == 2);
    HDRSHOT_CHECK(snapshot.candidates[1].front_to_back_order == 2);
    HDRSHOT_CHECK(window_at(snapshot, {1}, {250, 250}) == (PixelRect{200, 200, 200, 100}));
    HDRSHOT_CHECK(window_at(snapshot, {1}, {900, 600}) == (PixelRect{0, 0, 2000, 1000}));
  }
}
void dock_filter_uses_identity_and_level_not_bounds_or_names() {
  const auto dock_level = CGWindowLevelForKey(kCGDockWindowLevelKey);
  const char* dock_path = "/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock";
  HDRSHOT_CHECK(classify_mac_window("", dock_path, dock_level) == MacWindowRole::ignored);
  HDRSHOT_CHECK(classify_mac_window("com.example.dock", "", dock_level) == MacWindowRole::selectable);
  HDRSHOT_CHECK(classify_mac_window("", "/Applications/Dock.app/Contents/MacOS/Dock", dock_level)
      == MacWindowRole::selectable);
  HDRSHOT_CHECK(classify_mac_window("", "", dock_level) == MacWindowRole::selectable);
  for (const auto key : {kCGNormalWindowLevelKey, kCGFloatingWindowLevelKey,
                        kCGModalPanelWindowLevelKey, kCGUtilityWindowLevelKey, kCGPopUpMenuWindowLevelKey}) {
    const auto level = CGWindowLevelForKey(key);
    HDRSHOT_CHECK(classify_mac_window("org.example.app", "", level) == MacWindowRole::selectable);
  }
  // Small Dock carriers are ignored too. No screen-area cutoff is involved.
  const auto snapshot = map_mac_window_records(request(), {
      {1, 90, dock_level, {20, 20, 40, 30}, 1, true, classify_mac_window("", dock_path, dock_level)}}, 10);
  HDRSHOT_CHECK(snapshot.candidates.empty());
}
void ignoring_dock_does_not_bypass_security_blockers() {
  const auto dock_level = CGWindowLevelForKey(kCGDockWindowLevelKey);
  const auto high_level = CGWindowLevelForKey(kCGScreenSaverWindowLevelKey);
  for (const char* identity : {"com.apple.loginwindow", "com.apple.dock"}) {
    const auto snapshot = map_mac_window_records(request(), {
        {1, 90, dock_level, {0, 0, 1000, 500}, 1, true,
            classify_mac_window("com.apple.dock", "", dock_level)},
        {2, 91, high_level, {100, 100, 100, 50}, 1, true,
            classify_mac_window(identity, "", high_level)},
        {3, 92, 0, {0, 0, 1000, 500}}}, 10);
    HDRSHOT_CHECK(snapshot.candidates.size() == 2);
    HDRSHOT_CHECK(snapshot.candidates[0].role == WindowHitRole::blocker);
    HDRSHOT_CHECK(!window_at(snapshot, {1}, {250, 250}));
    HDRSHOT_CHECK(window_at(snapshot, {1}, {900, 600}) == (PixelRect{0, 0, 2000, 1000}));
  }
}
void async_success_and_destructor_do_not_duplicate_completion() {
  std::atomic<int> calls{};
  std::promise<bool> result;
  auto ready = result.get_future();
  {
    MacWindowCatalogPort port([] { return Result<std::vector<MacWindowRecord>, Error>::success({
        {1, -1, 0, {10, 20, 200, 100}}}); }, 250ms);
    port.snapshot_windows(request(), [&](auto value) {
      if (++calls == 1) result.set_value(value && value.value().candidates.size() == 1);
    });
    HDRSHOT_CHECK(ready.wait_for(2s) == std::future_status::ready);
    HDRSHOT_CHECK(ready.get());
  }
  std::this_thread::sleep_for(40ms);
  HDRSHOT_CHECK(calls == 1);
}
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool released{};
  std::atomic<bool> started{};
  Result<std::vector<MacWindowRecord>, Error> query() {
    started = true;
    std::unique_lock lock(mutex);
    changed.wait_for(lock, 1s, [&] { return released; });
    return Result<std::vector<MacWindowRecord>, Error>::success({});
  }
  void release() { { const std::scoped_lock lock(mutex); released = true; } changed.notify_all(); }
};
void timeout_and_late_result_complete_once() {
  auto gate = std::make_shared<Gate>();
  std::atomic<int> calls{};
  std::promise<bool> result;
  auto ready = result.get_future();
  MacWindowCatalogPort port([gate] { return gate->query(); }, 15ms);
  port.snapshot_windows(request(), [&](auto value) {
    if (++calls == 1) result.set_value(!value && value.error().safe_context.at("reason") == "metadata_query_timeout");
  });
  HDRSHOT_CHECK(ready.wait_for(2s) == std::future_status::ready);
  HDRSHOT_CHECK(ready.get());
  gate->release();
  std::this_thread::sleep_for(40ms);
  HDRSHOT_CHECK(calls == 1);
}
void request_is_owned_and_busy_queries_fall_back_without_another_worker() {
  auto gate = std::make_shared<Gate>();
  std::promise<Result<WindowSnapshot, Error>> result;
  auto ready = result.get_future();
  MacWindowCatalogPort port([gate] { return gate->query(); }, 1000ms);
  auto caller_request = request();
  port.snapshot_windows(caller_request, [&](auto value) { result.set_value(std::move(value)); });
  caller_request.session_id = {99};
  caller_request.displays.displays.clear();
  bool busy = false;
  port.snapshot_windows(caller_request, [&](auto value) {
    busy = !value && value.error().safe_context.at("reason") == "metadata_query_busy";
  });
  HDRSHOT_CHECK(busy);
  gate->release();
  HDRSHOT_CHECK(ready.wait_for(2s) == std::future_status::ready);
  const auto value = ready.get();
  HDRSHOT_CHECK(value && value.value().session_id == SessionId{1});
}
void cancel_and_destroy_are_bounded_while_query_is_pending() {
  for (bool destroy : {false, true}) {
    auto gate = std::make_shared<Gate>();
    std::atomic<int> calls{};
    std::atomic<bool> cancelled{};
    auto port = std::make_unique<MacWindowCatalogPort>([gate] { return gate->query(); }, 50ms);
    port->snapshot_windows(request(), [&](auto result) {
      cancelled = !result && result.error().code == ErrorCode::operation_cancelled;
      ++calls;
    });
    if (destroy) port.reset(); else port->cancel({1}, {2});
    HDRSHOT_CHECK(calls == 1 && cancelled);
    gate->release();
    std::this_thread::sleep_for(65ms);
    HDRSHOT_CHECK(calls == 1);
  }
}
} // namespace
int main() {
  return hdrshot::test::run({
      {"Quartz metadata projection and filtering", native_records_map_without_titles_or_shadow_expansion},
      {"stable owner identity and system surface roles", stable_identity_roles_preserve_front_to_back_applications},
      {"menu bar and status item selection preserve stacking", system_menu_bar_and_status_items_use_real_z_order},
      {"Dock carrier cannot steal app hover or click across displays", dock_carrier_is_ignored_on_retina_and_negative_origin_displays},
      {"Dock menus survive carrier filtering", dock_menus_are_not_removed_with_the_carrier},
      {"Dock filter is independent of area and localized names", dock_filter_uses_identity_and_level_not_bounds_or_names},
      {"ignored Dock does not bypass security blockers", ignoring_dock_does_not_bypass_security_blockers},
      {"async success completes once", async_success_and_destructor_do_not_duplicate_completion},
      {"timeout does not wait for OS query", timeout_and_late_result_complete_once},
      {"owned request and busy query fallback", request_is_owned_and_busy_queries_fall_back_without_another_worker},
      {"cancel/destruction ignore late OS reply", cancel_and_destroy_are_bounded_while_query_is_pending}});
}
