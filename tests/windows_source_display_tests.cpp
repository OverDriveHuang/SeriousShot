#include "platform/cpu/cpu_analysis_port.hpp"
#include "platform/windows/windows_analysis_backend.hpp"
#include "platform/windows/windows_display_state_service.hpp"
#include "platform/windows/windows_icc_transform.hpp"
#include "platform/windows/windows_source_display_binding.hpp"
#include "test_support.hpp"
#include "ui/qt/analyzer_window.hpp"
#include "ui/qt/pinned_screenshot_window.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QPointer>
#include <QScreen>
#include <QThread>
#include <windows.h>
#include <algorithm>
#include <array>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

using namespace hdrshot;
namespace {
using Service = WindowsDisplayStateService;
using Status = WindowsAnalysisPresentationStatus;

void pump(int milliseconds) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (elapsed.elapsed() < milliseconds) {
    QApplication::processEvents();
    QThread::msleep(2);
  }
}
void until(const std::function<bool()> &done, int timeout = 10000) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (!done() && elapsed.elapsed() < timeout)
    pump(3);
  HDRSHOT_CHECK(done());
}
analysis::Input fixture() {
  analysis::FloatImage pixels;
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 32; ++x)
      pixels.push_back({float(x) / 8, float(y) / 8, .25F, 1});
  auto source = make_cpu_analysis_source({32, 16}, std::move(pixels));
  HDRSHOT_CHECK(source.has_value());
  return {source.value(), 17, true};
}
std::shared_ptr<const WindowsIccProfile> profile(float gamma) {
  std::array<WindowsIccProfile::Curve, 3> curves;
  for (auto &curve : curves) {
    curve.type = 0;
    curve.parameters[0] = gamma;
  }
  const std::array<float, 9> identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
  // Synthetic, in-memory profiles. No association or system ICC file changes.
  return std::make_shared<const WindowsIccProfile>(
      "source-display-fixture-gamma-" + std::to_string(gamma), curves,
      identity, identity);
}

struct Integration {
  struct Errors {
    std::mutex mutex;
    std::vector<Error> values;
    void add(Error error) {
      const std::scoped_lock lock(mutex);
      values.push_back(std::move(error));
    }
    void check() {
      const std::scoped_lock lock(mutex);
      for (const auto &error : values) {
        std::cerr << error.module;
        for (const auto &[key, value] : error.safe_context)
          std::cerr << ' ' << key << '=' << value;
        std::cerr << '\n';
      }
      HDRSHOT_CHECK(values.empty());
    }
  };
  analysis::Input input{fixture()};
  const LinearSourceRef original_source{input.source};
  LinearFloatPixels original_pixels;
  WindowsAnalysisBackend backend;
  std::array<std::shared_ptr<AnalysisPresenterPort>, 3> presenters;
  std::shared_ptr<Errors> errors{std::make_shared<Errors>()};
  WindowsDisplayInfo display;
  std::size_t queries{};
  bool fail_query{};
  bool fail_icc_query{};
  bool fail_icc_parse{};
  std::shared_ptr<const WindowsIccProfile> resolved_profile;
  int recoveries{};
  int analysis_requests{};
  std::shared_ptr<Service> service;
  std::unique_ptr<AnalyzerWindow> analyzer;
  std::array<std::unique_ptr<PinnedScreenshotWindow>, 2> pins;

  explicit Integration(bool first_icc_query_fails = false) : fail_icc_query(first_icc_query_fails) {
    auto pixels = input.source->read_region({0, 0, 32, 16});
    HDRSHOT_CHECK(pixels.has_value());
    original_pixels = std::move(pixels.value());
    auto made = make_windows_analysis_backend();
    HDRSHOT_CHECK(made.has_value());
    backend = std::move(made.value());
    presenters[0] = backend.presenter;
    for (std::size_t i = 1; i < presenters.size(); ++i) {
      auto presenter = make_windows_analysis_presenter();
      HDRSHOT_CHECK(presenter.has_value());
      presenters[i] = std::move(presenter.value());
    }
    for (const auto &presenter : presenters)
      set_windows_analysis_presenter_error_callback(*presenter,
          [mailbox = errors](Error error) { mailbox->add(std::move(error)); });

    analyzer = std::make_unique<AnalyzerWindow>(input);
    analyzer->set_close_confirmation([] { return true; });
    analyzer->set_request_handler([this](analysis::Request request) {
      ++analysis_requests;
      // The small fixture exercises the actual Windows analysis backend too.
      auto result = backend.port->analyze(input, request);
      if (result)
        analyzer->accept_result(std::move(result.value()));
      else
        errors->add(result.error());
    });
    for (auto &pin : pins)
      pin = std::make_unique<PinnedScreenshotWindow>(input,
          PinnedScreenshotDefaults{false, 400});

    const auto screen = QGuiApplication::primaryScreen();
    HDRSHOT_CHECK(screen);
    const auto origin = screen->availableGeometry().topLeft() + QPoint(20, 20);
    std::array<QWidget *, 3> hosts{analyzer.get(), pins[0].get(), pins[1].get()};
    for (auto *host : hosts) {
      host->setAttribute(Qt::WA_ShowWithoutActivating);
      host->setWindowOpacity(0);
      host->move(origin);
      host->show();
    }
    // Settle first-show sizing before binding. All targets below are actual
    // content HWNDs on the same real monitor, including on a locked desktop.
    pump(300);
    std::array<QWidget *, 3> surfaces{
        analyzer->source_widget()->presentation_surface(),
        pins[0]->source_view()->presentation_surface(),
        pins[1]->source_view()->presentation_surface()};
    std::uintptr_t monitor{};
    for (auto *surface : surfaces) {
      const auto hwnd = reinterpret_cast<HWND>(surface->winId());
      HDRSHOT_CHECK(IsWindow(hwnd) && IsWindowVisible(hwnd));
      const auto current = reinterpret_cast<std::uintptr_t>(
          MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
      HDRSHOT_CHECK(current);
      if (!monitor)
        monitor = current;
      HDRSHOT_CHECK(current == monitor);
    }
    // This startup enumeration is read-only and outside the injected service's
    // query counter. Retain the real monitor/adapter/target identity.
    const auto displays = windows_enumerate_displays();
    HDRSHOT_CHECK(displays.has_value());
    const auto found = std::find_if(displays.value().begin(), displays.value().end(),
        [monitor](const auto &value) { return value.monitor == monitor; });
    HDRSHOT_CHECK(found != displays.value().end());
    display = *found;
    display.advanced_color_mode = 2;
    display.sdr_white_level = 1000;
    display.source_white = {true, 80.};
    if (first_icc_query_fails) {
      display.advanced_color_mode = 0;
      display.source_white = {};
    }
    // Color state is deliberately simulated. D3D presentation still uses the
    // real surfaces; passing this is not a physical HDR/settings-change test.
    Service::Options options;
    options.native_notifications = false;
    options.coalesce_ms = 1;
    options.query = [this](std::uintptr_t target, Service::QueryCompletion complete) {
      HDRSHOT_CHECK(target == display.monitor);
      ++queries;
      if (fail_query)
        complete(Service::QueryResult::failure({ErrorCode::presenter_failed,
            "injected-display-query", Retryability::after_recreate,
            {{"reason","injected_query_failure"}}}));
      else {
        auto resolved = display;
        if (fail_icc_query || fail_icc_parse) {
          windows_resolve_display_icc(resolved, nullptr,
              [this](const std::string&) {
                if (fail_icc_query)
                  return Result<std::string, Error>::failure({ErrorCode::presenter_failed,
                      "injected-icc-query", Retryability::after_recreate,
                      {{"api","WcsGetDefaultColorProfile"},{"nativeCode","5"}}});
                return Result<std::string, Error>::success("injected-invalid-profile.icc");
              }, [](const std::string&) {
                return Result<std::shared_ptr<const WindowsIccProfile>, Error>::failure({
                    ErrorCode::invalid_color_contract, "injected-icc-parse", Retryability::never,
                    {{"reason","invalid_profile"}}});
              });
          HDRSHOT_CHECK(resolved.icc_profile_assumed_srgb && resolved.icc_profile);
        }
        resolved_profile = resolved.icc_profile;
        complete(Service::QueryResult::success(std::move(resolved)));
      }
    };
    service = std::make_shared<Service>(std::move(options));
    const auto on_error = [mailbox = errors](Error error) {
      mailbox->add(std::move(error));
    };
    const QPointer<AnalyzerWindow> analyzer_guard(analyzer.get());
    HDRSHOT_CHECK(bind_windows_source_display(*analyzer, *surfaces[0],
        presenters[0], service, [analyzer_guard] {
          if (analyzer_guard) analyzer_guard->refresh_presentation();
        }, on_error, [this] { ++recoveries; }).has_value());
    for (std::size_t i = 0; i < pins.size(); ++i) {
      const QPointer<PinnedScreenshotWindow> guard(pins[i].get());
      HDRSHOT_CHECK(bind_windows_source_display(*pins[i], *surfaces[i + 1],
          presenters[i + 1], service, [guard] {
            if (guard && !guard->is_closed()) guard->refresh_presentation();
          }, on_error, [this] { ++recoveries; }).has_value());
    }
    analyzer->set_present_handler([this](analysis::SourceView view, std::uintptr_t surface) {
      auto result = presenters[0]->present(input, view, surface);
      if (!result) errors->add(result.error());
    });
    for (std::size_t i = 0; i < pins.size(); ++i)
      pins[i]->present_requested = [this, i](analysis::SourceView view, std::uintptr_t surface) {
        auto result = presenters[i + 1]->present(input, view, surface);
        if (!result) errors->add(result.error());
      };
    until([&] { return finished(3); });
    pump(250); // Include the analyzer's ordinary initial refinement debounce.
    until([&] { return finished(3); });
    errors->check();
    HDRSHOT_CHECK(analysis_requests > 0);
    HDRSHOT_CHECK(queries == 1);
  }
  ~Integration() {
    if (analyzer) {
      analyzer->set_present_handler({});
      analyzer->set_request_handler({});
    }
    for (auto &pin : pins)
      if (pin) pin->present_requested = {};
    for (const auto &presenter : presenters)
      if (presenter) set_windows_analysis_presenter_error_callback(*presenter, {});
    // Revoke native bindings before service/query state and callbacks disappear.
    pins = {};
    analyzer.reset();
    if (service) service->shutdown();
  }
  std::array<Status, 3> status() const {
    return {windows_analysis_presentation_status(*presenters[0]),
            windows_analysis_presentation_status(*presenters[1]),
            windows_analysis_presentation_status(*presenters[2])};
  }
  bool finished(std::size_t count) const {
    const auto values = status();
    for (std::size_t i = 0; i < count; ++i) {
      if (values[i].failed) {
        errors->check();
        HDRSHOT_CHECK(values[i].failed == 0);
      }
      if (!values[i].completed ||
          values[i].completed_generation != values[i].requested ||
          values[i].worker_active || values[i].pending)
        return false;
    }
    return true;
  }
  void changed(const std::function<void()> &notify, std::size_t active = 3) {
    const auto before = status();
    const auto count = queries;
    notify();
    until([&] {
      if (!finished(active)) return false;
      const auto after = status();
      for (std::size_t i = 0; i < active; ++i)
        if (after[i].completed_display_generation <= before[i].completed_display_generation)
          return false;
      return true;
    });
    pump(40);
    errors->check();
    HDRSHOT_CHECK(queries == count + 1);
  }
  void unchanged(const std::function<void()> &notify) {
    const auto before = status();
    const auto count = queries;
    notify();
    until([&] { return queries == count + 1; });
    pump(100);
    errors->check();
    HDRSHOT_CHECK(queries == count + 1);
    const auto after = status();
    for (std::size_t i = 0; i < before.size(); ++i) {
      HDRSHOT_CHECK(after[i].display_generation == before[i].display_generation);
      HDRSHOT_CHECK(after[i].completed_display_generation == before[i].completed_display_generation);
      HDRSHOT_CHECK(after[i].requested == before[i].requested);
    }
  }
  void input_unchanged(int expected_requests, std::uint64_t request_revision) {
    HDRSHOT_CHECK(analysis_requests == expected_requests);
    HDRSHOT_CHECK(analyzer->current_request().revision == request_revision);
    HDRSHOT_CHECK(input.source == original_source && input.revision == 17 && input.is_hdr);
    const auto pixels = input.source->read_region({0, 0, 32, 16});
    HDRSHOT_CHECK(pixels.has_value() && pixels.value() == original_pixels);
  }
  void interact_without_query() {
    const auto before = status();
    const auto query_count = queries;
    std::array<AnalyzerSourceView*, 3> views{
        analyzer->source_widget(), pins[0]->source_view(), pins[1]->source_view()};
    for (std::size_t i = 0; i < views.size(); ++i)
      views[i]->set_transform({false, 3. + i, {11. + i, -9. - i}});
    until([&] {
      if (!finished(3)) return false;
      const auto after = status();
      for (std::size_t i = 0; i < before.size(); ++i)
        if (after[i].completed <= before[i].completed) return false;
      return true;
    });
    errors->check();
    HDRSHOT_CHECK(queries == query_count);
  }
};

void one_service_refreshes_analyzer_and_two_pins() {
  Integration fixture;
  const auto requests = fixture.analysis_requests;
  const auto request_revision = fixture.analyzer->current_request().revision;
  const auto monitor_burst = [&] {
    for (int i = 0; i < 25; ++i)
      fixture.service->notify_monitor_changed(fixture.display.monitor);
  };
  const auto profile_burst = [&] {
    for (int i = 0; i < 25; ++i)
      fixture.service->notify_color_profiles_changed();
  };

  fixture.display.sdr_white_level = 2000;
  fixture.display.source_white.sdr_white_nits = 160.;
  fixture.changed(monitor_burst);
  fixture.input_unchanged(requests, request_revision);
  fixture.unchanged(monitor_burst);

  // Simulate activation delivery without taking foreground focus or touching
  // the user's pointer. Neither host type subscribes to an activation query.
  const auto before_activation = fixture.queries;
  for (QWidget *host : {static_cast<QWidget *>(fixture.analyzer.get()),
                        static_cast<QWidget *>(fixture.pins[0].get()),
                        static_cast<QWidget *>(fixture.pins[1].get())}) {
    QEvent deactivate(QEvent::WindowDeactivate), activate(QEvent::WindowActivate);
    QApplication::sendEvent(host, &deactivate);
    QApplication::sendEvent(host, &activate);
    SendMessageW(reinterpret_cast<HWND>(host->winId()), WM_ACTIVATE, WA_INACTIVE, 0);
    SendMessageW(reinterpret_cast<HWND>(host->winId()), WM_ACTIVATE, WA_ACTIVE, 0);
  }
  pump(150);
  HDRSHOT_CHECK(fixture.queries == before_activation);
  fixture.input_unchanged(requests, request_revision);

  fixture.display.advanced_color_mode = 0;
  fixture.display.source_white = {};
  fixture.display.icc_profile = profile(1.8F);
  fixture.changed(profile_burst);
  fixture.display.icc_profile = profile(2.2F);
  fixture.changed(profile_burst);
  fixture.unchanged(profile_burst);
  fixture.input_unchanged(requests, request_revision);

  // Non-ICC device/topology failures still suspend an invalid target. ICC
  // failures are resolved as successful sRGB states and tested separately.
  fixture.fail_query = true;
  const auto before_recovery = fixture.recoveries;
  profile_burst();
  until([&] {
    const std::scoped_lock lock(fixture.errors->mutex);
    return fixture.errors->values.size() == 3;
  });
  {
    const std::scoped_lock lock(fixture.errors->mutex);
    for (const auto &error : fixture.errors->values)
      HDRSHOT_CHECK(error.module == "injected-display-query");
    fixture.errors->values.clear();
  }
  const auto suspended = fixture.status();
  fixture.analyzer->refresh_presentation();
  for (auto &pin : fixture.pins) pin->refresh_presentation();
  pump(100);
  for (std::size_t i = 0; i < suspended.size(); ++i)
    HDRSHOT_CHECK(fixture.status()[i].requested == suspended[i].requested);
  fixture.fail_query = false;
  fixture.changed(profile_burst);
  HDRSHOT_CHECK(fixture.recoveries == before_recovery + 3);
  fixture.input_unchanged(requests, request_revision);

  // A vetoed Close must retain both the shared subscription and presentation.
  fixture.analyzer->set_close_confirmation([] { return false; });
  HDRSHOT_CHECK(!fixture.analyzer->close());
  HDRSHOT_CHECK(fixture.analyzer->isVisible());
  fixture.display.icc_profile = profile(2.4F);
  fixture.changed(profile_burst);
  fixture.input_unchanged(requests, request_revision);
  fixture.analyzer->set_close_confirmation([] { return true; });

  // Destroy one accepted pin close while retaining its presenter for passive
  // status observation. The analyzer and remaining pin still share one query.
  HDRSHOT_CHECK(fixture.pins[1]->close());
  HDRSHOT_CHECK(fixture.pins[1]->is_closed());
  fixture.pins[1].reset();
  pump(60);
  const auto closed = fixture.status()[2];
  fixture.display.icc_profile = profile(2.6F);
  fixture.changed(profile_burst, 2);
  const auto after_closed = fixture.status()[2];
  HDRSHOT_CHECK(after_closed.requested == closed.requested);
  HDRSHOT_CHECK(after_closed.completed_display_generation == closed.completed_display_generation);
  fixture.input_unchanged(requests, request_revision);
  std::cout << "Three real HWND hosts: one initial query, coalesced synthetic HDR/ICC changes, "
               "completed D3D display generations; no system display settings changed\n";
}

void icc_failure_keeps_first_frame_and_interactions_available() {
  Integration fixture(true);
  const auto requests = fixture.analysis_requests;
  const auto revision = fixture.analyzer->current_request().revision;
  HDRSHOT_CHECK(fixture.resolved_profile == windows_srgb_icc_profile());
  fixture.interact_without_query();
  fixture.input_unchanged(requests, revision);
  const auto notify = [&] {
    for (int i = 0; i < 25; ++i) fixture.service->notify_color_profiles_changed();
  };
  // Repeated identical failures keep one shared fallback without redundant frames.
  fixture.unchanged(notify);
  fixture.fail_icc_query = false;
  fixture.display.icc_profile = profile(1.8F);
  fixture.changed(notify);
  HDRSHOT_CHECK(fixture.resolved_profile == fixture.display.icc_profile);
  fixture.fail_icc_parse = true;
  fixture.changed(notify);
  HDRSHOT_CHECK(fixture.resolved_profile == windows_srgb_icc_profile());
  // Change the view again while the invalid ICC file is still selected.
  for (auto* view : {fixture.analyzer->source_widget(), fixture.pins[0]->source_view(),
                    fixture.pins[1]->source_view()}) view->fit();
  until([&] { return fixture.finished(3); });
  fixture.interact_without_query();
  fixture.fail_icc_parse = false;
  fixture.display.icc_profile = profile(2.2F);
  fixture.changed(notify);
  fixture.input_unchanged(requests, revision);
  std::cout << "Injected ICC failure: three first frames, shared sRGB fallback, "
               "zoom/pan frames and notification recovery completed\n";
}
} // namespace

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);
  return test::run({{"shared Windows display state refreshes analyzer and two pins",
                     one_service_refreshes_analyzer_and_two_pins},
                    {"ICC failure keeps first frame zoom pan and recovery available",
                     icc_failure_keeps_first_frame_and_interactions_available}});
}
