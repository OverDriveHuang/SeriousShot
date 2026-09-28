#include "platform/windows/windows_window_catalog.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <dwmapi.h>
#include <map>
#include <mutex>
#include <thread>
#include <windows.h>

namespace hdrshot {
namespace {
using Key = std::pair<std::uint64_t, std::uint64_t>;
Error error(const char *reason,
            ErrorCode code = ErrorCode::precondition_failed) {
  return {code,
          "WindowsWindowCatalogPort",
          Retryability::never,
          {{"reason", reason}}};
}
Result<std::vector<WindowsWindowRecord>, Error> enumerate_windows() {
  std::vector<WindowsWindowRecord> result;
  const BOOL okay = EnumWindows(
      [](HWND hwnd, LPARAM data) -> BOOL {
        auto &records =
            *reinterpret_cast<std::vector<WindowsWindowRecord> *>(data);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        BOOL cloaked = FALSE;
        (void)DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                    sizeof(cloaked));
        RECT rect{};
        if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                         &rect, sizeof(rect))) &&
            !GetWindowRect(hwnd, &rect))
          return TRUE;
        const auto style =
            static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        records.push_back(
            {reinterpret_cast<std::uint64_t>(hwnd),
             pid,
             {static_cast<double>(rect.left), static_cast<double>(rect.top),
              static_cast<double>(rect.right - rect.left),
              static_cast<double>(rect.bottom - rect.top)},
             IsWindowVisible(hwnd) != FALSE,
             cloaked != FALSE,
             IsIconic(hwnd) != FALSE,
             (style & WS_EX_TOPMOST) != 0,
             (style & WS_EX_TOOLWINDOW) != 0,
             (style & WS_EX_NOACTIVATE) != 0});
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&result));
  if (!okay)
    return Result<std::vector<WindowsWindowRecord>, Error>::failure(
        error("enum_windows_failed"));
  return Result<std::vector<WindowsWindowRecord>, Error>::success(
      std::move(result));
}
struct Pending {
  std::mutex mutex;
  WindowCatalogPort::Completion completion;
  void finish(Result<WindowSnapshot, Error> value) {
    WindowCatalogPort::Completion callback;
    {
      const std::scoped_lock lock(mutex);
      callback.swap(completion);
    }
    if (callback)
      callback(std::move(value));
  }
};
} // namespace

WindowsWindowRole classify_windows_window(const WindowsWindowRecord &r,
                                          std::uint32_t own_pid) {
  if (!r.token || r.process_id == own_pid || !r.visible || r.cloaked ||
      r.minimized || r.bounds_physical.width <= 0 ||
      r.bounds_physical.height <= 0 || !std::isfinite(r.bounds_physical.x) ||
      !std::isfinite(r.bounds_physical.y) ||
      !std::isfinite(r.bounds_physical.width) ||
      !std::isfinite(r.bounds_physical.height))
    return WindowsWindowRole::ignored;
  return r.topmost || r.tool_window || r.no_activate
             ? WindowsWindowRole::blocker
             : WindowsWindowRole::selectable;
}
WindowSnapshot
map_windows_window_records(const SnapshotWindowsRequest &request,
                           const std::vector<WindowsWindowRecord> &records,
                           const std::vector<WindowsDisplayInfo> &displays,
                           std::uint32_t own_pid) {
  WindowSnapshot result{request.session_id,
                        request.operation_id,
                        request.displays.generation,
                        {}};
  for (std::size_t index = 0; index < records.size(); ++index) {
    const auto &record = records[index];
    const auto role = classify_windows_window(record, own_pid);
    if (role == WindowsWindowRole::ignored)
      continue;
    for (const auto &display : displays) {
      const auto found = std::find_if(request.displays.displays.begin(),
                                      request.displays.displays.end(),
                                      [&](const DisplaySnapshot &d) {
                                        return d.id == display.snapshot.id;
                                      });
      if (found == request.displays.displays.end())
        continue;
      const auto frame = found->desktop_frame_points;
      const double scale_x =
          static_cast<double>(found->capture_size_px.width) / frame.width;
      const double scale_y =
          static_cast<double>(found->capture_size_px.height) / frame.height;
      if (!std::isfinite(scale_x) || !std::isfinite(scale_y) || scale_x <= 0 ||
          scale_y <= 0)
        continue;
      const WindowRectF logical{
          frame.x + (record.bounds_physical.x - display.physical_x) / scale_x,
          frame.y + (record.bounds_physical.y - display.physical_y) / scale_y,
          record.bounds_physical.width / scale_x,
          record.bounds_physical.height / scale_y};
      const auto rect = map_window_bounds(
          logical, {frame.x, frame.y, frame.width, frame.height},
          found->capture_size_px);
      if (rect)
        result.candidates.push_back(
            {record.token, found->id, *rect, static_cast<std::uint32_t>(index),
             role == WindowsWindowRole::blocker ? WindowHitRole::blocker
                                                : WindowHitRole::selectable});
    }
  }
  return result;
}
struct WindowsWindowCatalogPort::State {
  std::shared_ptr<const std::vector<WindowsDisplayInfo>> displays;
  Enumerate enumerate;
  std::chrono::milliseconds timeout;
  std::atomic_bool busy{false};
  std::mutex mutex;
  std::map<Key, std::shared_ptr<Pending>> pending;
  void finish(Key key, const std::shared_ptr<Pending> &job,
              Result<WindowSnapshot, Error> value) {
    {
      const std::scoped_lock lock(mutex);
      auto it = pending.find(key);
      if (it != pending.end() && it->second == job)
        pending.erase(it);
    }
    job->finish(std::move(value));
  }
};
WindowsWindowCatalogPort::WindowsWindowCatalogPort(
    std::shared_ptr<const std::vector<WindowsDisplayInfo>> displays,
    Enumerate enumerate, std::chrono::milliseconds timeout)
    : state_(std::make_shared<State>()) {
  state_->displays = std::move(displays);
  state_->enumerate = enumerate ? std::move(enumerate) : enumerate_windows;
  state_->timeout = std::clamp(timeout, std::chrono::milliseconds{1},
                               std::chrono::milliseconds{1000});
}
WindowsWindowCatalogPort::~WindowsWindowCatalogPort() {
  std::map<Key, std::shared_ptr<Pending>> jobs;
  {
    const std::scoped_lock lock(state_->mutex);
    jobs.swap(state_->pending);
  }
  for (auto &[key, job] : jobs) {
    (void)key;
    job->finish(Result<WindowSnapshot, Error>::failure(
        error("catalog_destroyed", ErrorCode::operation_cancelled)));
  }
}
void WindowsWindowCatalogPort::set_displays(
    std::shared_ptr<const std::vector<WindowsDisplayInfo>> displays) {
  const std::scoped_lock lock(state_->mutex);
  state_->displays = std::move(displays);
}
void WindowsWindowCatalogPort::snapshot_windows(
    const SnapshotWindowsRequest &request, Completion done) {
  const auto state = state_;
  const auto owned = request;
  std::shared_ptr<const std::vector<WindowsDisplayInfo>> displays;
  {
    const std::scoped_lock lock(state->mutex);
    displays = state->displays;
  }
  const Key key{request.session_id.value, request.operation_id.value};
  auto job = std::make_shared<Pending>();
  job->completion = std::move(done);
  if (state->busy.exchange(true)) {
    job->finish(
        Result<WindowSnapshot, Error>::failure(error("metadata_query_busy")));
    return;
  }
  {
    const std::scoped_lock lock(state->mutex);
    state->pending[key] = job;
  }
  try {
    std::thread([state, key, job] {
      std::this_thread::sleep_for(state->timeout);
      state->finish(key, job,
                    Result<WindowSnapshot, Error>::failure(
                        error("metadata_query_timeout")));
    }).detach();
  } catch (...) {
    state->busy.store(false);
    state->finish(key, job,
                  Result<WindowSnapshot, Error>::failure(
                      error("metadata_timeout_worker_unavailable")));
    return;
  }
  try {
    std::thread([state, key, job, owned, displays] {
      Result<std::vector<WindowsWindowRecord>, Error> records =
          Result<std::vector<WindowsWindowRecord>, Error>::failure(
              error("metadata_query_failed"));
      try {
        records = state->enumerate();
      } catch (...) {
      }
      state->busy.store(false);
      if (!records) {
        state->finish(key, job,
                      Result<WindowSnapshot, Error>::failure(records.error()));
        return;
      }
      state->finish(
          key, job,
          Result<WindowSnapshot, Error>::success(map_windows_window_records(
              owned, records.value(),
              displays ? *displays : std::vector<WindowsDisplayInfo>{},
              GetCurrentProcessId())));
    }).detach();
  } catch (...) {
    state->busy.store(false);
    state->finish(key, job,
                  Result<WindowSnapshot, Error>::failure(
                      error("metadata_query_worker_unavailable")));
  }
}
void WindowsWindowCatalogPort::cancel(SessionId session,
                                      OperationId operation) {
  const Key key{session.value, operation.value};
  std::shared_ptr<Pending> job;
  {
    const std::scoped_lock lock(state_->mutex);
    auto it = state_->pending.find(key);
    if (it == state_->pending.end())
      return;
    job = std::move(it->second);
    state_->pending.erase(it);
  }
  job->finish(Result<WindowSnapshot, Error>::failure(
      error("metadata_cancelled", ErrorCode::operation_cancelled)));
}
} // namespace hdrshot
