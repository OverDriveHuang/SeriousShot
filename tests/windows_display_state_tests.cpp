#include "platform/windows/windows_display_state_service.hpp"
#include "platform/windows/windows_icc_transform.hpp"
#include "ports/diagnostics_port.hpp"
#include "test_support.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <map>
#include <thread>
#include <vector>

using namespace hdrshot;
namespace {
using Service = WindowsDisplayStateService;
void pump(int milliseconds = 15) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}
void until(const std::function<bool()>& done, int timeout = 3000) {
  QElapsedTimer elapsed; elapsed.start();
  while (!done() && elapsed.elapsed() < timeout) pump(2);
  HDRSHOT_CHECK(done());
}
WindowsDisplayInfo hdr(std::uintptr_t monitor, std::uint32_t white = 1000,
    std::uint32_t target = 1) {
  WindowsDisplayInfo info;
  info.monitor = monitor; info.snapshot.id = DisplayId{monitor};
  info.target_adapter_low = 42; info.target_id = target;
  info.advanced_color_mode = 2; info.source_white.hdr_active = true;
  info.sdr_white_level = white; info.source_white.sdr_white_nits = white * 80.0 / 1000.0;
  return info;
}
struct ManualQuery {
  struct Call { std::uintptr_t monitor; Service::QueryCompletion complete; };
  std::vector<Call> calls;
  Service::Options options() {
    Service::Options opts;
    opts.native_notifications = false; opts.coalesce_ms = 1;
    opts.query = [this](auto monitor, auto complete) { calls.push_back({monitor, std::move(complete)}); };
    return opts;
  }
  void success(std::size_t index, WindowsDisplayInfo info) {
    auto complete = calls.at(index).complete;
    complete(Service::QueryResult::success(std::move(info)));
  }
};
void same_monitor_shared_and_other_monitor_isolated() {
  ManualQuery query;
  Service service(query.options());
  std::vector<WindowsDisplayStateRef> a, b, c;
  service.subscribe(1,[&](auto value) { HDRSHOT_CHECK(value.has_value()); a.push_back(value.value()); });
  service.subscribe(1,[&](auto value) { HDRSHOT_CHECK(value.has_value()); b.push_back(value.value()); });
  service.subscribe(2,[&](auto value) { HDRSHOT_CHECK(value.has_value()); c.push_back(value.value()); });
  until([&] { return query.calls.size() == 2; });
  HDRSHOT_CHECK(a.empty() && b.empty());
  query.success(0,hdr(1)); query.success(1,hdr(2));
  until([&] { return a.size() == 1 && b.size() == 1 && c.size() == 1; });
  HDRSHOT_CHECK(a.back() == b.back());
  const auto b_screen_generation = c.back()->generation;
  service.notify_monitor_changed(1); service.notify_monitor_changed(1);
  until([&] { return query.calls.size() == 3; });
  HDRSHOT_CHECK(query.calls.back().monitor == 1);
  query.success(2,hdr(1,2000));
  until([&] { return a.size() == 2 && b.size() == 2; });
  HDRSHOT_CHECK(c.size() == 1 && c.back()->generation == b_screen_generation);
  std::vector<WindowsDisplayStateRef> later;
  service.subscribe(1,[&](auto value) { later.push_back(value.value()); });
  HDRSHOT_CHECK(later.empty());
  until([&] { return later.size() == 1; });
  HDRSHOT_CHECK(later.front() == a.back() && query.calls.size() == 3);
}
void unchanged_global_event_does_not_publish() {
  ManualQuery query; Service service(query.options());
  int updates = 0;
  service.subscribe(1,[&](auto) { ++updates; });
  service.subscribe(2,[&](auto) { ++updates; });
  until([&] { return query.calls.size() == 2; });
  query.success(0,hdr(1)); query.success(1,hdr(2));
  until([&] { return updates == 2; });
  for (int i=0;i<25;++i) service.notify_color_profiles_changed();
  until([&] { return query.calls.size() == 4; });
  query.success(2,hdr(1)); query.success(3,hdr(2)); pump();
  HDRSHOT_CHECK(updates == 2 && query.calls.size() == 4);
}
void dirty_during_query_retrieves_final_value() {
  ManualQuery query; Service service(query.options());
  std::vector<std::uint32_t> whites;
  service.subscribe(1,[&](auto value) { whites.push_back(value.value()->display.sdr_white_level); });
  until([&] { return query.calls.size() == 1; });
  for (int i=0;i<10;++i) service.notify_monitor_changed(1);
  pump(); HDRSHOT_CHECK(query.calls.size() == 1);
  query.success(0,hdr(1,1100));
  until([&] { return query.calls.size() == 2; });
  query.success(1,hdr(1,2200));
  until([&] { return !whites.empty() && whites.back() == 2200; });
  pump(40); HDRSHOT_CHECK(query.calls.size() == 2);
}
void move_unsubscribe_and_late_result_are_safe() {
  ManualQuery query; Service service(query.options());
  std::vector<std::uintptr_t> monitors;
  auto token = service.subscribe(1,[&](auto value) { monitors.push_back(value.value()->display.monitor); });
  until([&] { return query.calls.size() == 1; });
  service.move(token,2);
  until([&] { return query.calls.size() == 2; });
  query.success(1,hdr(2)); query.success(0,hdr(1));
  until([&] { return monitors.size() == 1; });
  HDRSHOT_CHECK(monitors.front() == 2);
  service.notify_monitor_changed(2);
  until([&] { return query.calls.size() == 3; });
  service.unsubscribe(token);
  // Reusing the same HMONITOR must not accept the previous incarnation's work.
  token = service.subscribe(2,[&](auto value) { monitors.push_back(value.value()->display.monitor); });
  until([&] { return query.calls.size() == 4; });
  query.success(2,hdr(2,1700)); pump(); HDRSHOT_CHECK(monitors.size() == 1);
  query.success(3,hdr(2,2100)); until([&] { return monitors.size() == 2; });
  service.unsubscribe(token);
  service.notify_color_profiles_changed(); pump(); HDRSHOT_CHECK(query.calls.size() == 4);
}
void topology_discards_old_adapter_target() {
  ManualQuery query; Service service(query.options());
  std::vector<WindowsDisplayStateRef> states;
  service.subscribe(1,[&](auto value) { states.push_back(value.value()); });
  until([&] { return query.calls.size() == 1; });
  service.notify_topology_changed(); service.notify_topology_changed(); pump();
  query.success(0,hdr(1,1000,1));
  until([&] { return query.calls.size() == 2; });
  HDRSHOT_CHECK(states.empty());
  query.success(1,hdr(1,1000,2));
  until([&] { return states.size() == 1; });
  HDRSHOT_CHECK(states.front()->display.target_id == 2);
  HDRSHOT_CHECK(states.front()->topology_generation == 2);
}
void errors_are_preserved_and_recovery_is_explicit() {
  ManualQuery query; Service service(query.options());
  std::vector<Service::Update> results;
  service.subscribe(1,[&](auto value) { results.push_back(std::move(value)); });
  until([&] { return query.calls.size() == 1; });
  query.success(0,hdr(1)); until([&] { return results.size() == 1; });
  const auto frozen = results.front().value();
  service.notify_monitor_changed(1); until([&] { return query.calls.size() == 2; });
  const Error error{ErrorCode::presenter_failed,"injected",Retryability::after_recreate,
      {{"api","QueryDisplayConfig"},{"nativeCode","5"}}};
  query.calls[1].complete(Service::QueryResult::failure(error));
  until([&] { return results.size() == 2; });
  HDRSHOT_CHECK(!results.back() && results.back().error() == error);
  HDRSHOT_CHECK(frozen->display.sdr_white_level == 1000);
  pump(60); HDRSHOT_CHECK(query.calls.size() == 2);
  service.notify_monitor_changed(1); until([&] { return query.calls.size() == 3; });
  query.success(2,hdr(1)); until([&] { return results.size() == 3; });
  HDRSHOT_CHECK(results.back().has_value() && results.back().value() == frozen);
}
void callbacks_can_unsubscribe_and_completion_can_outlive_service() {
  ManualQuery query;
  auto service = std::make_unique<Service>(query.options());
  Service::Token token{};
  int updates = 0;
  token = service->subscribe(1,[&](auto) { ++updates; service->unsubscribe(token); });
  until([&] { return query.calls.size() == 1; });
  query.success(0,hdr(1)); until([&] { return updates == 1; });
  service->subscribe(1,[&](auto) { ++updates; });
  until([&] { return query.calls.size() == 2; });
  auto late = query.calls.back().complete;
  service->shutdown(); service.reset();
  std::thread worker([late] { late(Service::QueryResult::success(hdr(1))); });
  worker.join(); pump(); HDRSHOT_CHECK(updates == 1);
}
void coalescing_deadline_is_not_restarted_by_burst() {
  ManualQuery query; auto options = query.options(); options.coalesce_ms = 35;
  Service service(std::move(options));
  service.subscribe(1,[](auto) {});
  QTimer burst; burst.setInterval(2);
  QObject::connect(&burst,&QTimer::timeout,&service,[&] { service.notify_monitor_changed(1); });
  burst.start();
  until([&] { return !query.calls.empty(); }, 500);
  // A debounce timer restarted by every event would never reach this point.
  burst.stop();
  HDRSHOT_CHECK(query.calls.size() == 1);
}
struct Diagnostics final : DiagnosticsPort {
  std::vector<DiagnosticEvent> events;
  bool detailed_logging() const override { return true; }
  Result<DiagnosticReceipt,Error> record(const DiagnosticEvent& event) override {
    events.push_back(event); return Result<DiagnosticReceipt,Error>::success({events.size()});
  }
  bool saw(const char* stage,const char* outcome) const {
    return std::any_of(events.begin(),events.end(),[&](const auto& event) {
      return event.stage == stage && event.outcome == outcome;
    });
  }
};
void unsupported_advanced_notifications_keep_real_wcs_and_no_polling() {
  ManualQuery query; auto options = query.options();
  options.native_notifications = true;
  options.build_number = [] { return 19044u; }; // injected capability, not Win10 hardware
  const auto diagnostics = std::make_shared<Diagnostics>();
  Service service(std::move(options),nullptr,diagnostics);
  service.subscribe(1,[](auto) {});
  HDRSHOT_CHECK(diagnostics->saw("display.notifications.advanced","unsupported"));
  HDRSHOT_CHECK(diagnostics->saw("display.notifications.wcs","success"));
  until([&] { return query.calls.size() == 1; });
  query.success(0,hdr(1)); pump(150);
  HDRSHOT_CHECK(query.calls.size() == 1);
  service.shutdown();
}
void same_path_file_replacement_requires_system_notification() {
  QTemporaryDir directory; HDRSHOT_CHECK(directory.isValid());
  const auto path = directory.filePath("profile.icc");
  auto write = [&](const QByteArray& data) {
    QSaveFile file(path); HDRSHOT_CHECK(file.open(QIODevice::WriteOnly));
    HDRSHOT_CHECK(file.write(data) == data.size()); HDRSHOT_CHECK(file.commit());
  };
  write("first");
  int queries = 0; std::vector<std::string> identities;
  Service::Options options;
  options.native_notifications = true;
  options.coalesce_ms = 2; options.build_number = [] { return 19044u; };
  options.query = [&](auto monitor, auto complete) {
    ++queries; auto display = hdr(monitor); display.advanced_color_mode = 0;
    display.icc_path = path.toUtf8().toStdString();
    QFile file(path); HDRSHOT_CHECK(file.open(QIODevice::ReadOnly));
    display.icc_profile = std::make_shared<const WindowsIccProfile>(
        file.readAll().toStdString(), std::array<WindowsIccProfile::Curve,3>{},
        std::array<float,9>{},std::array<float,9>{});
    complete(Service::QueryResult::success(std::move(display)));
  };
  Service service(std::move(options));
  service.subscribe(1,[&](auto value) { identities.push_back(value.value()->display.icc_profile->sha256); });
  until([&] { return queries == 1 && !identities.empty(); });
  pump(150); HDRSHOT_CHECK(queries == 1 && identities.size() == 1);
  write("second");
  pump(150); HDRSHOT_CHECK(queries == 1 && identities.size() == 1);
  HDRSHOT_CHECK(identities.back() == "first");
  service.notify_color_profiles_changed();
  until([&] { return identities.size() == 2; });
  HDRSHOT_CHECK(queries == 2 && identities.back() == "second");
  write("third");
  pump(150); HDRSHOT_CHECK(queries == 2 && identities.size() == 2);
  service.notify_color_profiles_changed();
  until([&] { return identities.size() == 3; });
  HDRSHOT_CHECK(queries == 3 && identities.back() == "third");
}
void invalid_icc_recovers_on_system_notification_with_same_transform() {
  QTemporaryDir directory; HDRSHOT_CHECK(directory.isValid());
  const auto path = directory.filePath("injected-profile.icc");
  auto write = [&](const QByteArray& bytes) {
    QSaveFile file(path); HDRSHOT_CHECK(file.open(QIODevice::WriteOnly));
    HDRSHOT_CHECK(file.write(bytes) == bytes.size()); HDRSHOT_CHECK(file.commit());
  };
  write("invalid");
  int queries = 0;
  bool fail_path_query = false;
  std::vector<WindowsDisplayStateRef> states;
  Service::Options options;
  options.native_notifications = true;
  options.coalesce_ms = 2; options.build_number = [] { return 19044u; };
  options.query = [&](auto monitor, auto complete) {
    ++queries; auto display = hdr(monitor); display.advanced_color_mode = 0;
    windows_resolve_display_icc(display, nullptr,
        [&](const std::string&) {
          if (fail_path_query)
            return Result<std::string, Error>::failure({ErrorCode::presenter_failed,
                "injected-path-query", Retryability::after_recreate,
                {{"api", "WcsGetDefaultColorProfile"}, {"nativeCode", "5"}}});
          return Result<std::string, Error>::success(path.toStdString());
        },
        [](const std::string& name) {
          QFile file(QString::fromStdString(name)); HDRSHOT_CHECK(file.open(QIODevice::ReadOnly));
          // Inject profile decoding; refresh is requested by the system-notification seam.
          if (file.readAll() == "invalid")
            return Result<std::shared_ptr<const WindowsIccProfile>, Error>::failure({
                ErrorCode::invalid_color_contract, "injected-parser", Retryability::never,
                {{"reason", "invalid_profile"}}});
          return Result<std::shared_ptr<const WindowsIccProfile>, Error>::success(windows_srgb_icc_profile());
        });
    HDRSHOT_CHECK(display.icc_path == (fail_path_query ? std::string{} : path.toStdString()));
    complete(Service::QueryResult::success(std::move(display)));
  };
  Service service(std::move(options));
  service.subscribe(1,[&](auto value) {
    HDRSHOT_CHECK(value.has_value()); states.push_back(value.value());
  });
  until([&] { return queries == 1 && !states.empty(); });
  pump(30);
  HDRSHOT_CHECK(states.size() == 1 && states.back()->display.icc_profile_assumed_srgb);
  const auto settled_queries = queries;
  pump(100); HDRSHOT_CHECK(queries == settled_queries);
  write("valid");
  pump(100); HDRSHOT_CHECK(queries == settled_queries && states.size() == 1);
  service.notify_color_profiles_changed();
  until([&] { return states.size() == 2; });
  HDRSHOT_CHECK(!states.back()->display.icc_profile_assumed_srgb);
  // Identical color parameters must still publish recovery of their provenance.
  HDRSHOT_CHECK(states.front()->display.icc_profile == states.back()->display.icc_profile);
  HDRSHOT_CHECK(states.back()->generation > states.front()->generation);
  write("invalid");
  service.notify_color_profiles_changed();
  until([&] { return states.size() == 3; });
  HDRSHOT_CHECK(states.back()->display.icc_profile_assumed_srgb);
  // A path-query failure keeps sRGB until another explicit system notification.
  fail_path_query = true;
  const auto before_query_failure = queries;
  service.notify_color_profiles_changed();
  until([&] { return queries > before_query_failure; });
  pump(30);
  fail_path_query = false;
  write("valid");
  const auto before_recovery = queries;
  pump(100); HDRSHOT_CHECK(queries == before_recovery && states.size() == 3);
  service.notify_color_profiles_changed();
  until([&] { return states.size() == 4; });
  HDRSHOT_CHECK(!states.back()->display.icc_profile_assumed_srgb);
}
int native_readonly() {
  // Explicit opt-in: real API subscription/initial query, no ICC/brightness
  // mutation. This is not evidence of receiving an actual settings change.
  winrt::init_apartment(winrt::apartment_type::single_threaded);
  const auto diagnostics = std::make_shared<Diagnostics>();
  auto displays = windows_enumerate_displays(diagnostics.get());
  if (!displays) {
    std::cerr << "Native display query unavailable: " << displays.error().module << '\n'; return 77;
  }
  Service service(nullptr,diagnostics);
  int updates = 0, errors = 0;
  for (const auto& display:displays.value())
    service.subscribe(display.monitor,[&](auto value) { if(value) ++updates; else ++errors; });
  until([&] { return updates + errors >= static_cast<int>(displays.value().size()); },10000);
  HDRSHOT_CHECK(!errors);
  HDRSHOT_CHECK(diagnostics->saw("display.notifications.wcs","success"));
  if (windows_build_number() >= 22621)
    HDRSHOT_CHECK(diagnostics->saw("display.notifications.advanced","success"));
  service.shutdown();
  return 0;
}
}  // namespace

int main(int argc,char** argv) {
  QCoreApplication app(argc,argv);
  if (app.arguments().contains("--native-readonly")) return native_readonly();
  return test::run({
      {"same monitor shares state and isolates other monitor",same_monitor_shared_and_other_monitor_isolated},
      {"unchanged global event is one query per target with no publication",unchanged_global_event_does_not_publish},
      {"dirty query obtains final value",dirty_during_query_retrieves_final_value},
      {"move unsubscribe and monitor incarnation discard late results",move_unsubscribe_and_late_result_are_safe},
      {"topology discards old adapter target result",topology_discards_old_adapter_target},
      {"failure preserves original error and explicit recovery",errors_are_preserved_and_recovery_is_explicit},
      {"callback mutation and post-destruction worker completion",callbacks_can_unsubscribe_and_completion_can_outlive_service},
      {"coalescing deadline does not debounce indefinitely",coalescing_deadline_is_not_restarted_by_burst},
      {"injected Win10 capability uses real WCS and has no idle polling",unsupported_advanced_notifications_keep_real_wcs_and_no_polling},
      {"same-path file replacement waits for system notification",same_path_file_replacement_requires_system_notification},
      {"invalid ICC recovers with same transform on system notification",invalid_icc_recovers_on_system_notification_with_same_transform}});
}
