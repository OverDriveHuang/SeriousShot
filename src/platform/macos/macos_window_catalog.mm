#include "platform/macos/macos_window_catalog.hpp"

#import <CoreGraphics/CoreGraphics.h>
#import <AppKit/NSRunningApplication.h>
#import <Foundation/Foundation.h>
#include <libproc.h>
#include <dispatch/dispatch.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <utility>
#include <string>

namespace hdrshot {
namespace {
using Key = std::pair<std::uint64_t, std::uint64_t>;
Error window_error(ErrorCode code, const char* reason) {
  return {code, "MacWindowCatalogPort", Retryability::never, {{"reason", reason}}};
}
Result<std::vector<MacWindowRecord>, Error> enumerate_quartz_windows() {
  @autoreleasepool {
    const auto options = kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements;
    CFArrayRef array = CGWindowListCopyWindowInfo(options, kCGNullWindowID);
    if (!array) return Result<std::vector<MacWindowRecord>, Error>::failure(
        window_error(ErrorCode::precondition_failed, "window_server_unavailable"));
    NSArray* windows = CFBridgingRelease(array);
    std::vector<MacWindowRecord> result;
    // PID reuse is irrelevant inside this single snapshot; never cache across
    // capture sessions. Resolving once per owner also bounds AppKit queries.
    std::map<std::int32_t, std::pair<std::string, std::string>> identities;
    result.reserve(windows.count);
    for (NSDictionary* item in windows) {
      const auto* bounds = (__bridge CFDictionaryRef)item[(__bridge NSString*)kCGWindowBounds];
      CGRect rect{};
      if (!bounds || CFGetTypeID(bounds) != CFDictionaryGetTypeID() ||
          !CGRectMakeWithDictionaryRepresentation(bounds, &rect)) continue;
      NSNumber* identifier = item[(__bridge NSString*)kCGWindowNumber];
      NSNumber* pid = item[(__bridge NSString*)kCGWindowOwnerPID];
      NSNumber* layer = item[(__bridge NSString*)kCGWindowLayer];
      NSNumber* alpha = item[(__bridge NSString*)kCGWindowAlpha];
      if (!identifier || !pid || !layer || !alpha) continue;
      auto [entry, inserted] = identities.try_emplace(pid.intValue);
      if (inserted) {
        NSRunningApplication* app =
            [NSRunningApplication runningApplicationWithProcessIdentifier:pid.intValue];
        if (const char* bundle = app.bundleIdentifier.UTF8String) entry->second.first = bundle;
        if (const char* path = app.executableURL.path.UTF8String) entry->second.second = path;
        if (entry->second.second.empty()) {
          char path[PROC_PIDPATHINFO_MAXSIZE]{};
          if (proc_pidpath(pid.intValue, path, sizeof(path)) > 0) entry->second.second = path;
        }
      }
      // Do not read kCGWindowOwnerName or kCGWindowName: both can be localized.
      const auto role = classify_mac_window(entry->second.first, entry->second.second, layer.intValue);
      result.push_back({identifier.unsignedLongLongValue, pid.intValue, layer.intValue,
          {rect.origin.x, rect.origin.y, rect.size.width, rect.size.height},
          alpha.doubleValue, true, role});
    }
    return Result<std::vector<MacWindowRecord>, Error>::success(std::move(result));
  }
}
struct Pending {
  std::mutex mutex;
  WindowCatalogPort::Completion completion;
  void finish(Result<WindowSnapshot, Error> result) {
    WindowCatalogPort::Completion callback;
    { const std::scoped_lock lock(mutex); callback.swap(completion); }
    if (callback) callback(std::move(result));
  }
};
} // namespace

MacWindowRole classify_mac_window(const std::string_view bundle,
                                 const std::string_view path, const std::int32_t layer) {
  const auto identity = [&](std::string_view id, std::string_view executable) {
    // Exact system path is a fallback for non-bundled/temporarily unresolved
    // owners, not a match against a localized basename or an arbitrary suffix.
    return bundle == id || (bundle.empty() && path == executable);
  };
  if (bundle == "dev.hdrshot.desktop" ||
      identity("com.apple.screencaptureui",
          "/System/Library/CoreServices/screencaptureui.app/Contents/MacOS/screencaptureui"))
    return MacWindowRole::ignored;
  const bool visible_system_ui =
      layer == CGWindowLevelForKey(kCGDockWindowLevelKey) ||
      layer == CGWindowLevelForKey(kCGMainMenuWindowLevelKey) ||
      layer == CGWindowLevelForKey(kCGStatusWindowLevelKey) ||
      layer == CGWindowLevelForKey(kCGPopUpMenuWindowLevelKey);
  if (identity("com.apple.dock", "/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock")) {
    // The main Dock carrier can have desktop-sized bounds with transparent
    // contents even at window alpha=1. Ignore it along with lower backdrops;
    // a blocker would still prevent picking the real application underneath.
    // Keep this owner's separate menu/status/popup windows and unknown elevated
    // blockers. No window-area test or process-wide exclusion (ADR-014).
    return layer <= CGWindowLevelForKey(kCGDockWindowLevelKey)
        ? MacWindowRole::ignored
        : visible_system_ui ? MacWindowRole::selectable : MacWindowRole::blocker;
  }
  if (identity("com.apple.systemuiserver",
          "/System/Library/CoreServices/SystemUIServer.app/Contents/MacOS/SystemUIServer") ||
      identity("com.apple.controlcenter",
          "/System/Library/CoreServices/ControlCenter.app/Contents/MacOS/ControlCenter"))
    return visible_system_ui ? MacWindowRole::selectable : MacWindowRole::blocker;
  if (identity("com.apple.loginwindow",
          "/System/Library/CoreServices/loginwindow.app/Contents/MacOS/loginwindow"))
    return MacWindowRole::blocker;
  if (path == "/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/Resources/WindowServer")
    return layer <= CGWindowLevelForKey(kCGNormalWindowLevelKey)
        ? MacWindowRole::ignored
        : visible_system_ui ? MacWindowRole::selectable : MacWindowRole::blocker;
  return MacWindowRole::selectable;
}

WindowSnapshot map_mac_window_records(
    const SnapshotWindowsRequest& request, const std::vector<MacWindowRecord>& records,
    std::int32_t own_pid) {
  WindowSnapshot result{request.session_id, request.operation_id, request.displays.generation, {}};
  for (std::size_t index = 0; index < records.size(); ++index) {
    const auto& record = records[index];
    if (!record.window_id || record.owner_pid == own_pid || !record.on_screen ||
        record.role == MacWindowRole::ignored || record.layer < 0 ||
        !std::isfinite(record.alpha) || record.alpha <= 0.0) continue;
    for (const auto& display : request.displays.displays) {
      const auto frame = display.desktop_frame_points;
      const auto rect = map_window_bounds(record.bounds_points,
          {frame.x, frame.y, frame.width, frame.height}, display.capture_size_px);
      if (rect) result.candidates.push_back({record.window_id, display.id, *rect,
          static_cast<std::uint32_t>(index),
          record.role == MacWindowRole::blocker ? WindowHitRole::blocker : WindowHitRole::selectable});
    }
  }
  return result;
}

struct MacWindowCatalogPort::State {
  Enumerate enumerate;
  std::chrono::milliseconds timeout;
  std::atomic<bool> query_busy{false};
  std::mutex mutex;
  std::map<Key, std::shared_ptr<Pending>> pending;
  void finish(Key key, const std::shared_ptr<Pending>& job, Result<WindowSnapshot, Error> result) {
    {
      const std::scoped_lock lock(mutex);
      const auto it = pending.find(key);
      if (it != pending.end() && it->second == job) pending.erase(it);
    }
    job->finish(std::move(result));
  }
};

MacWindowCatalogPort::MacWindowCatalogPort(Enumerate enumerate, std::chrono::milliseconds timeout)
    : state_(std::make_shared<State>()) {
  state_->enumerate = enumerate ? std::move(enumerate) : enumerate_quartz_windows;
  state_->timeout = std::clamp(timeout, std::chrono::milliseconds{1}, std::chrono::milliseconds{1000});
}
MacWindowCatalogPort::~MacWindowCatalogPort() {
  std::map<Key, std::shared_ptr<Pending>> pending;
  { const std::scoped_lock lock(state_->mutex); pending.swap(state_->pending); }
  for (auto& [key, job] : pending) {
    (void)key;
    job->finish(Result<WindowSnapshot, Error>::failure(
        window_error(ErrorCode::operation_cancelled, "catalog_destroyed")));
  }
}

void MacWindowCatalogPort::snapshot_windows(const SnapshotWindowsRequest& request, Completion done) {
  const auto state = state_;
  // Objective-C blocks capture reference parameters by reference. Own the
  // request before dispatch: the caller may pass a temporary or finish its run.
  const auto owned_request = request;
  const Key key{request.session_id.value, request.operation_id.value};
  auto job = std::make_shared<Pending>();
  job->completion = std::move(done);
  if (state->query_busy.exchange(true)) {
    job->finish(Result<WindowSnapshot, Error>::failure(
        window_error(ErrorCode::precondition_failed, "metadata_query_busy")));
    return;
  }
  { const std::scoped_lock lock(state->mutex); state->pending[key] = job; }
  // The timeout runs independently of the OS query and never blocks UI/cancel.
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
      std::chrono::duration_cast<std::chrono::nanoseconds>(state->timeout).count()),
      dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    state->finish(key, job, Result<WindowSnapshot, Error>::failure(
        window_error(ErrorCode::precondition_failed, "metadata_query_timeout")));
  });
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    auto result = [&] {
      try { return state->enumerate(); }
      catch (...) { return Result<std::vector<MacWindowRecord>, Error>::failure(
          window_error(ErrorCode::precondition_failed, "metadata_query_failed")); }
    }();
    state->query_busy.store(false);
    if (result) {
      state->finish(key, job, Result<WindowSnapshot, Error>::success(
          map_mac_window_records(owned_request, result.value(), static_cast<std::int32_t>(getpid()))));
    } else {
      state->finish(key, job, Result<WindowSnapshot, Error>::failure(result.error()));
    }
  });
}
void MacWindowCatalogPort::cancel(SessionId session, OperationId operation) {
  const Key key{session.value, operation.value};
  std::shared_ptr<Pending> job;
  {
    const std::scoped_lock lock(state_->mutex);
    const auto it = state_->pending.find(key);
    if (it == state_->pending.end()) return;
    job = std::move(it->second);
    state_->pending.erase(it);
  }
  job->finish(Result<WindowSnapshot, Error>::failure(
      window_error(ErrorCode::operation_cancelled, "metadata_cancelled")));
}
} // namespace hdrshot
