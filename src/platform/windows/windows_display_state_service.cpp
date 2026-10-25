#include "platform/windows/windows_display_state_service.hpp"
#include "platform/windows/windows_icc_transform.hpp"
#include "ports/diagnostics_port.hpp"
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QThread>
#include <QTimer>
#include <QWinEventNotifier>
#include <DispatcherQueue.h>
#include <windows.graphics.display.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.System.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace hdrshot {
namespace {
using Service = WindowsDisplayStateService;
Error state_error(const char* stage, std::optional<std::uint32_t> code = {},
    std::source_location origin = std::source_location::current()) {
  Error error{ErrorCode::presenter_failed, "WindowsDisplayState",
      Retryability::after_recreate, {{"reason", stage}, {"api", stage}}, origin};
  if (code) error.safe_context["nativeCode"] = std::to_string(*code);
  return error;
}

// Workers never own a QObject. Closing this gate before QObject destruction
// makes even a completion from an uninterruptible OS call harmless.
struct DeliveryGate {
  std::mutex mutex;
  QObject* receiver{};
  void post(std::function<void()> work) {
    const std::scoped_lock lock(mutex);
    if (receiver) QMetaObject::invokeMethod(receiver, std::move(work), Qt::QueuedConnection);
  }
  void close() { const std::scoped_lock lock(mutex); receiver = nullptr; }
};

class QueryExecutor {
  struct State {
    std::mutex mutex;
    std::condition_variable ready, drained;
    std::deque<std::function<void()>> queue;
    bool stopped{};
    int alive{2};
  };
 public:
  QueryExecutor() : state_(std::make_shared<State>()) {
    for (auto& worker : workers_) worker = std::thread([state = state_] {
      const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      for (;;) {
        std::function<void()> work;
        {
          std::unique_lock lock(state->mutex);
          state->ready.wait(lock, [&] { return state->stopped || !state->queue.empty(); });
          if (state->stopped) break;
          work = std::move(state->queue.front()); state->queue.pop_front();
        }
        work();
      }
      if (SUCCEEDED(apartment)) CoUninitialize();
      { const std::scoped_lock lock(state->mutex); --state->alive; }
      state->drained.notify_all();
    });
  }
  ~QueryExecutor() { stop(); }
  void submit(std::function<void()> work) {
    { const std::scoped_lock lock(state_->mutex);
      if (state_->stopped) return;
      state_->queue.push_back(std::move(work)); }
    state_->ready.notify_one();
  }
  void stop() {
    if (!state_) return;
    { const std::scoped_lock lock(state_->mutex);
      state_->stopped = true; state_->queue.clear(); }
    state_->ready.notify_all();
    {
      std::unique_lock lock(state_->mutex);
      state_->drained.wait_for(lock, std::chrono::milliseconds(100), [&] { return !state_->alive; });
    }
    // Windows APIs do not offer cancellation for WCS queries. At most two
    // outstanding calls retain their value-only state after this bounded drain.
    for (auto& worker : workers_) if (worker.joinable()) worker.detach();
    state_.reset();
  }
 private:
  std::shared_ptr<State> state_;
  std::array<std::thread, 2> workers_;
};

class RegistryWatch final {
 public:
  RegistryWatch(QObject* owner, HKEY root, std::wstring path,
      std::function<void()> changed, std::function<void(Error)> failed)
      : owner_(owner), root_(root), path_(std::move(path)),
        changed_(std::move(changed)), failed_(std::move(failed)) {
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event_) { failed_(state_error("CreateEventW.WCS", GetLastError())); return; }
    notifier_ = std::make_unique<QWinEventNotifier>(event_, owner_);
    QObject::connect(notifier_.get(), &QWinEventNotifier::activated, owner_, [this] {
      notifier_->setEnabled(false);
      rearm(); // A one-shot wait is consumed before another is registered.
      changed_();
    });
    notifier_->setEnabled(false);
    rearm();
  }
  ~RegistryWatch() {
    if (notifier_) notifier_->setEnabled(false);
    notifier_.reset();
    if (key_) RegCloseKey(key_);
    if (event_) CloseHandle(event_);
  }
  bool active() const { return notifier_ && notifier_->isEnabled(); }
 private:
  void rearm() {
    if (!event_) return;
    for (int attempt = 0; attempt < 3; ++attempt) {
      if (key_) { RegCloseKey(key_); key_ = nullptr; }
      // Closing a key signals its event. Clear that synthetic signal before
      // registering the next wait, not after (which could lose a real event).
      ResetEvent(event_);
      std::wstring candidate = path_;
      LSTATUS code;
      for (;;) {
        code = RegOpenKeyExW(root_, candidate.c_str(), 0, KEY_NOTIFY, &key_);
        if (code == ERROR_SUCCESS) break;
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND && code != ERROR_KEY_DELETED) {
          failed_(state_error("RegOpenKeyExW.WCS", code)); return;
        }
        if (candidate.empty()) { failed_(state_error("RegOpenKeyExW.WCS", code)); return; }
        const auto slash = candidate.find_last_of(L'\\');
        candidate = slash == std::wstring::npos ? L"" : candidate.substr(0, slash);
      }
      code = RegNotifyChangeKeyValue(key_, TRUE,
          REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC,
          event_, TRUE);
      // The association key may disappear between open and registration. Rebind
      // immediately to its nearest existing parent; never create a watch key.
      if (code == ERROR_KEY_DELETED && attempt < 2) continue;
      if (code != ERROR_SUCCESS) { failed_(state_error("RegNotifyChangeKeyValue.WCS", code)); return; }
      notifier_->setEnabled(true);
      return;
    }
  }
  QObject* owner_{};
  HKEY root_{}, key_{};
  HANDLE event_{};
  std::wstring path_;
  std::function<void()> changed_;
  std::function<void(Error)> failed_;
  std::unique_ptr<QWinEventNotifier> notifier_;
};

bool same_target(const WindowsDisplayInfo& a, const WindowsDisplayInfo& b) {
  return a.monitor == b.monitor && a.target_adapter_low == b.target_adapter_low &&
      a.target_adapter_high == b.target_adapter_high && a.target_id == b.target_id;
}
bool same_display(const WindowsDisplayInfo& a, const WindowsDisplayInfo& b) {
  if (!same_target(a,b) || a.advanced_color_mode != b.advanced_color_mode) return false;
  if (a.advanced_color_mode == 2)
    return a.sdr_white_level == b.sdr_white_level &&
        a.source_white.sdr_white_nits == b.source_white.sdr_white_nits;
  if (a.advanced_color_mode == 0)
    return a.icc_profile && b.icc_profile && a.icc_profile->sha256 == b.icc_profile->sha256 &&
        a.icc_profile_assumed_srgb == b.icc_profile_assumed_srgb;
  return true;
}
}  // namespace

struct WindowsDisplayStateService::Impl final : QAbstractNativeEventFilter {
  struct Subscriber { std::uintptr_t monitor{}; Callback callback; std::uint64_t revision{1}; };
  struct Target {
    std::uint64_t incarnation{}, request{};
    std::size_t subscribers{};
    bool in_flight{}, dirty{};
    WindowsDisplayStateRef state;
    std::optional<Error> last_error;
    winrt::Windows::Graphics::Display::DisplayInformation info{nullptr};
    winrt::Windows::Graphics::Display::DisplayInformation::AdvancedColorInfoChanged_revoker color;
  };
  WindowsDisplayStateService& owner;
  Options options;
  std::shared_ptr<DiagnosticsPort> diagnostics;
  std::shared_ptr<DeliveryGate> gate{std::make_shared<DeliveryGate>()};
  std::unique_ptr<QueryExecutor> executor;
  std::map<Token, Subscriber> subscribers;
  std::map<std::uintptr_t, Target> targets;
  std::set<std::uintptr_t> pending;
  QTimer coalesce;
  std::unique_ptr<RegistryWatch> user_registry, system_registry;
  std::uint64_t next_token{1}, next_incarnation{1}, topology{1}, generation{};
  bool stopped{}, topology_pending{};
  bool native_filter_installed{};

  Impl(WindowsDisplayStateService& owner, Options opts, std::shared_ptr<DiagnosticsPort> log)
      : owner(owner), options(std::move(opts)), diagnostics(std::move(log)) {
    gate->receiver = &owner;
    coalesce.setSingleShot(true);
    QObject::connect(&coalesce, &QTimer::timeout, &owner, [this] { flush(); });
    if (!options.query) {
      executor = std::make_unique<QueryExecutor>();
      options.query = [this](std::uintptr_t monitor, QueryCompletion complete) {
        executor->submit([monitor, complete = std::move(complete), log = diagnostics] {
          try { complete(windows_query_display_state(monitor, log.get())); }
          catch (const winrt::hresult_error& e) {
            complete(QueryResult::failure(state_error("query.exception", static_cast<std::uint32_t>(e.code().value))));
          } catch (const std::exception&) {
            complete(QueryResult::failure(state_error("query.exception")));
          }
        });
      };
    }
    if (options.native_notifications && QCoreApplication::instance()) {
      QCoreApplication::instance()->installNativeEventFilter(this);
      native_filter_installed = true;
    }
  }
  void check_thread() const { Q_ASSERT(QThread::currentThread() == owner.thread()); }
  void failure(Error error, const char* stage = "display.state") {
    record_diagnostic_failure(diagnostics.get(), stage, error);
  }
  bool nativeEventFilter(const QByteArray&, void* message, qintptr*) override {
    if (!stopped && static_cast<MSG*>(message)->message == WM_DISPLAYCHANGE)
      owner.notify_topology_changed();
    return false;
  }
  void start_global_notifications() {
    if (!options.native_notifications || user_registry) return;
    auto changed = [this] { owner.notify_color_profiles_changed(); };
    auto failed = [this](Error error) { failure(std::move(error), "display.notifications.wcs"); };
    user_registry = std::make_unique<RegistryWatch>(&owner, HKEY_CURRENT_USER,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ICM\\ProfileAssociations\\Display\\{4d36e96e-e325-11ce-bfc1-08002be10318}", changed, failed);
    system_registry = std::make_unique<RegistryWatch>(&owner, HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e96e-e325-11ce-bfc1-08002be10318}", changed, failed);
    if (user_registry->active() && system_registry->active())
      record_diagnostic_stage(diagnostics.get(), {}, {}, "windows.color",
          "display.notifications.wcs", "success", {{"api", "RegNotifyChangeKeyValue"}});
  }
  void start_monitor_notifications(std::uintptr_t monitor, Target& target) {
    if (!options.native_notifications) return;
    // The public desktop interop contract starts at 22621. Older systems keep
    // WCS notifications, with no substitute white-level queries or polling.
    if ((options.build_number ? options.build_number() : windows_build_number()) < 22621) {
      record_diagnostic_stage(diagnostics.get(), {}, {}, "windows.color",
          "display.notifications.advanced", "unsupported", {{"reason", "os_build_unsupported"}});
      return;
    }
    const char* stage = "DispatcherQueue.GetForCurrentThread";
    try {
      using namespace winrt::Windows;
      if (!System::DispatcherQueue::GetForCurrentThread()) {
        // One current-thread queue lives with the UI apartment. Do not tear it
        // down while another Windows component still has registered events.
        static System::DispatcherQueueController controller{nullptr};
        if (!controller) {
          stage = "CreateDispatcherQueueController";
          DispatcherQueueOptions queue_options{sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
          winrt::check_hresult(CreateDispatcherQueueController(queue_options,
              reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(controller))));
        }
      }
      stage = "DisplayInformation.activation";
      auto interop = winrt::get_activation_factory<Graphics::Display::DisplayInformation, IDisplayInformationStaticsInterop>();
      stage = "DisplayInformation.GetForMonitor";
      winrt::check_hresult(interop->GetForMonitor(reinterpret_cast<HMONITOR>(monitor),
          winrt::guid_of<Graphics::Display::DisplayInformation>(), winrt::put_abi(target.info)));
      stage = "DisplayInformation.AdvancedColorInfoChanged";
      const auto delivery = gate;
      const auto incarnation = target.incarnation;
      target.color = target.info.AdvancedColorInfoChanged(winrt::auto_revoke,
          [this, delivery, monitor, incarnation](auto const&, auto const&) {
            delivery->post([this, monitor, incarnation] {
              const auto found = targets.find(monitor);
              if (found != targets.end() && found->second.incarnation == incarnation)
                owner.notify_monitor_changed(monitor);
            });
          });
      record_diagnostic_stage(diagnostics.get(), {}, {}, "windows.color",
          "display.notifications.advanced", "success", {{"api", stage}});
    } catch (const winrt::hresult_error& e) {
      target.color.revoke(); target.info = nullptr;
      const auto code = static_cast<std::uint32_t>(e.code().value);
      if (e.code() == E_NOINTERFACE || e.code() == REGDB_E_CLASSNOTREG || e.code() == E_NOTIMPL)
        record_diagnostic_stage(diagnostics.get(), {}, {}, "windows.color",
            "display.notifications.advanced", "unsupported", {{"api", stage}, {"nativeCode", std::to_string(code)}});
      else failure(state_error(stage, code), "display.notifications.advanced");
    }
  }
  void add_target(std::uintptr_t monitor) {
    auto [found, inserted] = targets.try_emplace(monitor);
    auto& target = found->second;
    ++target.subscribers;
    if (!inserted) return;
    target.incarnation = next_incarnation++;
    start_global_notifications();
    start_monitor_notifications(monitor, target); // subscribe before first query
    enqueue(monitor);
  }
  void remove_target(std::uintptr_t monitor) {
    const auto found = targets.find(monitor);
    if (found == targets.end() || --found->second.subscribers) return;
    found->second.color.revoke();
    targets.erase(found); pending.erase(monitor);
    if (targets.empty()) { user_registry.reset(); system_registry.reset(); coalesce.stop(); }
  }
  void enqueue(std::uintptr_t monitor) {
    if (stopped || !targets.contains(monitor)) return;
    pending.insert(monitor);
    if (!coalesce.isActive()) coalesce.start(std::max(0, options.coalesce_ms));
  }
  void flush() {
    topology_pending = false;
    const auto batch = std::exchange(pending, {});
    for (auto monitor : batch) {
      const auto found = targets.find(monitor);
      if (found == targets.end()) continue;
      if (found->second.in_flight) found->second.dirty = true;
      else query(monitor, found->second);
    }
  }
  void query(std::uintptr_t monitor, Target& target) {
    target.in_flight = true; target.dirty = false;
    const auto incarnation = target.incarnation, request = ++target.request, version = topology;
    const auto delivery = gate;
    auto complete = [this, delivery, monitor, incarnation, request, version](QueryResult result) {
      delivery->post([this, monitor, incarnation, request, version, result = std::move(result)]() mutable {
        finished(monitor, incarnation, request, version, std::move(result));
      });
    };
    try { options.query(monitor, complete); }
    catch (const std::exception&) { complete(QueryResult::failure(state_error("query.dispatch"))); }
  }
  void deliver(Token token, Update update) {
    const auto found = subscribers.find(token);
    if (found == subscribers.end()) return;
    const auto revision = found->second.revision;
    const auto version = topology;
    // Queuing each consumer separately permits callbacks to unsubscribe/move
    // themselves or other consumers without invalidating service iteration.
    gate->post([this, token, revision, version, update = std::move(update)]() mutable {
      const auto current = subscribers.find(token);
      if (current == subscribers.end() || current->second.revision != revision || version != topology) return;
      if (update && update.value()->topology_generation != topology) return;
      auto callback = current->second.callback;
      callback(std::move(update));
    });
  }
  void publish(std::uintptr_t monitor, Update update) {
    for (const auto& [token, subscriber] : subscribers)
      if (subscriber.monitor == monitor) deliver(token, update);
  }
  void finished(std::uintptr_t monitor, std::uint64_t incarnation,
      std::uint64_t request, std::uint64_t version, QueryResult result) {
    if (stopped) return;
    const auto found = targets.find(monitor);
    if (found == targets.end() || found->second.incarnation != incarnation ||
        found->second.request != request || !found->second.in_flight) return;
    auto& target = found->second;
    target.in_flight = false;
    if (version != topology) target.dirty = true;
    else if (!result) {
      target.last_error = result.error();
      failure(result.error());
      publish(monitor, Update::failure(result.error()));
    } else if (result.value().monitor != monitor) {
      const auto error = state_error("query.monitor_mismatch");
      target.last_error = error; failure(error); publish(monitor, Update::failure(error));
    } else {
      auto display = std::move(result.value());
      const bool recovered = target.last_error.has_value();
      target.last_error.reset();
      const bool changed = !target.state || !same_display(target.state->display, display) ||
          target.state->topology_generation != topology;
      if (changed) {
        target.state = std::make_shared<const WindowsDisplayState>(
            WindowsDisplayState{std::move(display), ++generation, topology});
        publish(monitor, Update::success(target.state));
      } else if (recovered) publish(monitor, Update::success(target.state));
    }
    if (target.dirty && !pending.contains(monitor)) query(monitor, target);
  }
  void stop() {
    check_thread();
    if (stopped) return;
    stopped = true; gate->close(); coalesce.stop(); pending.clear();
    if (native_filter_installed && QCoreApplication::instance())
      QCoreApplication::instance()->removeNativeEventFilter(this);
    native_filter_installed = false;
    user_registry.reset(); system_registry.reset();
    for (auto& [monitor, target] : targets) target.color.revoke();
    targets.clear(); subscribers.clear();
    if (executor) executor->stop();
  }
};

WindowsDisplayStateService::WindowsDisplayStateService(QObject* parent,
    std::shared_ptr<DiagnosticsPort> diagnostics)
    : WindowsDisplayStateService(Options{}, parent, std::move(diagnostics)) {}
WindowsDisplayStateService::WindowsDisplayStateService(Options options, QObject* parent,
    std::shared_ptr<DiagnosticsPort> diagnostics)
    : QObject(parent), impl_(std::make_unique<Impl>(*this, std::move(options), std::move(diagnostics))) {}
WindowsDisplayStateService::~WindowsDisplayStateService() { shutdown(); }
WindowsDisplayStateService::Token WindowsDisplayStateService::subscribe(std::uintptr_t monitor, Callback callback) {
  impl_->check_thread();
  if (impl_->stopped || !monitor || !callback) return 0;
  const auto token = impl_->next_token++;
  impl_->subscribers.emplace(token, Impl::Subscriber{monitor, std::move(callback)});
  impl_->add_target(monitor);
  const auto& target = impl_->targets.at(monitor);
  if (target.last_error) impl_->deliver(token, Update::failure(*target.last_error));
  else if (target.state) impl_->deliver(token, Update::success(target.state));
  return token;
}
void WindowsDisplayStateService::move(Token token, std::uintptr_t monitor) {
  impl_->check_thread();
  const auto found = impl_->subscribers.find(token);
  if (impl_->stopped || !monitor || found == impl_->subscribers.end() || found->second.monitor == monitor) return;
  impl_->remove_target(found->second.monitor);
  found->second.monitor = monitor; ++found->second.revision;
  impl_->add_target(monitor);
  const auto& target = impl_->targets.at(monitor);
  if (target.last_error) impl_->deliver(token, Update::failure(*target.last_error));
  else if (target.state) impl_->deliver(token, Update::success(target.state));
}
void WindowsDisplayStateService::unsubscribe(Token token) {
  impl_->check_thread();
  const auto found = impl_->subscribers.find(token);
  if (found == impl_->subscribers.end()) return;
  impl_->remove_target(found->second.monitor); impl_->subscribers.erase(found);
}
void WindowsDisplayStateService::shutdown() { if (impl_) impl_->stop(); }
void WindowsDisplayStateService::notify_monitor_changed(std::uintptr_t monitor) {
  impl_->check_thread(); impl_->enqueue(monitor);
}
void WindowsDisplayStateService::notify_color_profiles_changed() {
  impl_->check_thread();
  for (const auto& [monitor, target] : impl_->targets) impl_->enqueue(monitor);
}
void WindowsDisplayStateService::notify_topology_changed() {
  impl_->check_thread();
  if (impl_->stopped) return;
  if (!impl_->topology_pending) {
    impl_->topology_pending = true; ++impl_->topology;
    for (auto& [monitor, target] : impl_->targets) {
      target.color.revoke(); target.info = nullptr;
      impl_->start_monitor_notifications(monitor, target);
    }
  }
  notify_color_profiles_changed();
}
}  // namespace hdrshot
