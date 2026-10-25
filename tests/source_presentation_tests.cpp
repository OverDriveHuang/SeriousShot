#include "ui/qt/analyzer_window.hpp"
#include "ui/qt/pinned_screenshot_window.hpp"
#include "ui/qt/source_presentation_controller.hpp"
#include "platform/cpu/cpu_analysis_port.hpp"
#include "test_support.hpp"
#include <QApplication>
#include <QEventLoop>
#include <QExposeEvent>
#include <QMouseEvent>
#include <QLabel>
#include <QPlatformSurfaceEvent>
#include <QTimer>
#include <QWindow>
#include <algorithm>
#include <cmath>

using namespace hdrshot;
namespace {
void wait(int milliseconds = 85) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}
analysis::Input input() {
  auto source = make_cpu_analysis_source({180,112}, analysis::FloatImage(180*112,{.2f,.3f,.4f,1.f}));
  HDRSHOT_CHECK(source.has_value());
  return {source.value(),1,false};
}
void equal_geometry(const analysis::SourceView &a, const analysis::SourceView &b) {
  HDRSHOT_CHECK(a.target_size == b.target_size);
  HDRSHOT_CHECK_NEAR(a.scale,b.scale,1e-9);
  HDRSHOT_CHECK_NEAR(a.offset_x,b.offset_x,1e-9);
  HDRSHOT_CHECK_NEAR(a.offset_y,b.offset_y,1e-9);
}
void matches_widget(const analysis::SourceView &frame, const AnalyzerSourceView &source) {
  const auto dpr = source.devicePixelRatioF();
  HDRSHOT_CHECK(frame.target_size.width == int(std::lround(source.width()*dpr)));
  HDRSHOT_CHECK(frame.target_size.height == int(std::lround(source.height()*dpr)));
  HDRSHOT_CHECK_NEAR(frame.scale,source.effective_scale()*dpr,1e-9);
  HDRSHOT_CHECK_NEAR(frame.offset_x,source.image_rect().x()*dpr,1e-9);
  HDRSHOT_CHECK_NEAR(frame.offset_y,source.image_rect().y()*dpr,1e-9);
}
struct Hosts {
  analysis::Input image = input();
  std::vector<analysis::SourceView> analyzer_frames, pin_frames;
  int analyses{};
  AnalyzerWindow analyzer{image};
  PinnedScreenshotWindow pin{image,{}};
  Hosts() {
    analyzer.set_request_handler([this](auto) { ++analyses; });
    analyzer.set_present_handler([this](auto frame, auto handle) {
      HDRSHOT_CHECK(handle != 0);
      analyzer_frames.push_back(std::move(frame));
    });
    pin.present_requested = [this](auto frame, auto handle) {
      HDRSHOT_CHECK(handle != 0);
      pin_frames.push_back(std::move(frame));
    };
    analyzer.show();
    pin.show();
    wait(260);
    HDRSHOT_CHECK(!analyzer_frames.empty() && !pin_frames.empty());
  }
};
void display_error_recovery_preserves_other_errors() {
  Hosts hosts;
  auto *analyzer_status=hosts.analyzer.findChild<QLabel *>("analyzerStatus");
  auto *pin_status=hosts.pin.findChild<QLabel *>("pinStatus");
  HDRSHOT_CHECK(analyzer_status && pin_status);
  hosts.analyzer.show_display_error("display unavailable");
  hosts.pin.show_display_error("display unavailable");
  HDRSHOT_CHECK(!analyzer_status->isHidden() && !pin_status->isHidden());
  hosts.analyzer.set_export_busy(false);
  HDRSHOT_CHECK(!analyzer_status->isHidden());
  hosts.analyzer.clear_display_error();
  hosts.pin.clear_display_error();
  HDRSHOT_CHECK(analyzer_status->isHidden() && pin_status->isHidden());
  hosts.analyzer.show_display_error("display unavailable");
  hosts.pin.show_display_error("display unavailable");
  hosts.analyzer.show_error("export failed");
  hosts.pin.show_error("export failed");
  hosts.analyzer.clear_display_error();
  hosts.pin.clear_display_error();
  HDRSHOT_CHECK(!analyzer_status->isHidden() && !pin_status->isHidden());
  HDRSHOT_CHECK(analyzer_status->text()=="export failed" && pin_status->text()=="export failed");
}
void host_geometry_and_options() {
  Hosts hosts;
  // The offscreen screen can clamp the analyzer below the pin's 400px
  // minimum width on first show. Give both actual surfaces a comparable size.
  hosts.analyzer.resize(1400,900);
  wait();
  auto *source = hosts.analyzer.source_widget();
  hosts.pin.resize(source->size());
  wait();
  HDRSHOT_CHECK(hosts.pin.source_view()->size() == source->size());
  for (const auto transform : {AnalyzerSourceTransform{},
                               AnalyzerSourceTransform{false,1.,{13.25,-7.5}},
                               AnalyzerSourceTransform{false,3.75,{-61.5,24.25}}}) {
    source->set_transform(transform);
    hosts.pin.source_view()->set_transform(transform);
    wait();
    const auto analyzer = hosts.analyzer.source_view();
    const auto pin = hosts.pin.presentation_view();
    equal_geometry(analyzer,pin);
    equal_geometry(hosts.analyzer_frames.back(),hosts.pin_frames.back());
    matches_widget(hosts.analyzer_frames.back(),*source);
    HDRSHOT_CHECK(analyzer.settings == pin.settings);
    HDRSHOT_CHECK(!analyzer.operation_overlay && !pin.operation_overlay);
    HDRSHOT_CHECK(bool(hosts.analyzer_frames.back().operation_overlay));
    HDRSHOT_CHECK(!hosts.pin_frames.back().operation_overlay);
  }
}
void batched_refresh_has_no_analysis_or_idle_work() {
  Hosts hosts;
  const auto analyses = hosts.analyses;
  const auto revision = hosts.analyzer.current_request().revision;
  const auto old_analyzer = hosts.analyzer_frames.size(), old_pin = hosts.pin_frames.size();
  for (int i = 0; i < 120; ++i) {
    hosts.analyzer.refresh_presentation();
    hosts.pin.refresh_presentation();
  }
  HDRSHOT_CHECK(hosts.analyzer_frames.size() == old_analyzer);
  HDRSHOT_CHECK(hosts.pin_frames.size() == old_pin);
  wait();
  HDRSHOT_CHECK(hosts.analyzer_frames.size() == old_analyzer+1);
  HDRSHOT_CHECK(hosts.pin_frames.size() == old_pin+1);
  wait(230); // Includes the analyzer's refinement interval.
  HDRSHOT_CHECK(hosts.analyses == analyses);
  HDRSHOT_CHECK(hosts.analyzer.current_request().revision == revision);
  HDRSHOT_CHECK(hosts.analyzer_frames.size() == old_analyzer+1);
  HDRSHOT_CHECK(hosts.pin_frames.size() == old_pin+1);
  QTimer continuous;
  continuous.setInterval(2);
  QObject::connect(&continuous,&QTimer::timeout,&hosts.analyzer,[&] {
    hosts.analyzer.refresh_presentation();
    hosts.pin.refresh_presentation();
  });
  continuous.start();
  wait(100);
  continuous.stop();
  // A trailing-edge debounce would starve every frame until input stopped.
  HDRSHOT_CHECK(hosts.analyzer_frames.size() >= old_analyzer+3);
  HDRSHOT_CHECK(hosts.pin_frames.size() >= old_pin+3);
  HDRSHOT_CHECK(hosts.analyses == analyses);
}
void hidden_dirty_uses_latest_view_on_show() {
  Hosts hosts;
  hosts.analyzer.hide();
  hosts.pin.hide();
  const auto old_analyzer = hosts.analyzer_frames.size(), old_pin = hosts.pin_frames.size();
  hosts.analyzer.source_widget()->set_transform({false,2.75,{31.,-27.}});
  hosts.pin.source_view()->set_transform({false,4.25,{-18.,19.}});
  hosts.analyzer.refresh_presentation();
  hosts.pin.refresh_presentation();
  wait();
  HDRSHOT_CHECK(hosts.analyzer_frames.size() == old_analyzer);
  HDRSHOT_CHECK(hosts.pin_frames.size() == old_pin);
  hosts.analyzer.show();
  hosts.pin.show();
  wait();
  HDRSHOT_CHECK(hosts.analyzer_frames.size() > old_analyzer);
  HDRSHOT_CHECK(hosts.pin_frames.size() > old_pin);
  matches_widget(hosts.analyzer_frames.back(),*hosts.analyzer.source_widget());
  matches_widget(hosts.pin_frames.back(),*hosts.pin.source_view());
}
void native_child_events_represent_both_hosts() {
  Hosts hosts;
  for (bool analyzer : {false,true}) {
    auto *source = analyzer ? hosts.analyzer.source_widget() : hosts.pin.source_view();
    auto &frames = analyzer ? hosts.analyzer_frames : hosts.pin_frames;
    QWidget *owner = analyzer ? static_cast<QWidget *>(&hosts.analyzer) : &hosts.pin;
    auto *surface = source->presentation_surface();
    auto *native = surface->windowHandle();
    HDRSHOT_CHECK(native && owner->windowHandle());
    const auto before = frames.size();
    const auto analyses = hosts.analyses;
    // Real QWindow event routes; these tests never use grab()/render() to
    // accidentally force presentation while checking stale-frame recovery.
    QExposeEvent expose(QRegion(surface->rect()));
    QApplication::sendEvent(native,&expose);
    QApplication::sendEvent(owner->windowHandle(),&expose);
    QEvent dpr(QEvent::DevicePixelRatioChange);
    QApplication::sendEvent(native,&dpr);
    QApplication::sendEvent(surface,&dpr);
    wait();
    HDRSHOT_CHECK(frames.size() == before+1);
    matches_widget(frames.back(),*source);
    HDRSHOT_CHECK(hosts.analyses == analyses);
    const auto before_destroy = frames.size();
    QPlatformSurfaceEvent destroyed(QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed);
    QApplication::sendEvent(native,&destroyed);
    if (analyzer) hosts.analyzer.refresh_presentation(); else hosts.pin.refresh_presentation();
    wait();
    HDRSHOT_CHECK(frames.size() == before_destroy);
    QPlatformSurfaceEvent created(QPlatformSurfaceEvent::SurfaceCreated);
    QApplication::sendEvent(native,&created);
    wait();
    HDRSHOT_CHECK(frames.size() == before_destroy+1);
    matches_widget(frames.back(),*source);
  }
}
void close_cancel_keeps_refresh_and_accepted_close_stops_it() {
  Hosts hosts;
  hosts.analyzer.set_close_confirmation([] { return false; });
  HDRSHOT_CHECK(!hosts.analyzer.close());
  const auto before_cancel = hosts.analyzer_frames.size();
  hosts.analyzer.refresh_presentation();
  wait();
  HDRSHOT_CHECK(hosts.analyzer_frames.size() == before_cancel+1);
  hosts.analyzer.refresh_presentation();
  hosts.pin.refresh_presentation();
  hosts.analyzer.set_close_confirmation([] { return true; });
  HDRSHOT_CHECK(hosts.analyzer.close());
  HDRSHOT_CHECK(hosts.pin.close());
  const auto closed_analyzer = hosts.analyzer_frames.size(), closed_pin = hosts.pin_frames.size();
  hosts.analyzer.refresh_presentation();
  hosts.pin.refresh_presentation();
  wait();
  HDRSHOT_CHECK(hosts.analyzer_frames.size() == closed_analyzer);
  HDRSHOT_CHECK(hosts.pin_frames.size() == closed_pin);
  int calls_after_destroy = 0;
  auto *window = new PinnedScreenshotWindow(hosts.image,{});
  window->present_requested = [&](auto,auto) { ++calls_after_destroy; };
  window->show();
  window->refresh_presentation();
  delete window;
  wait();
  HDRSHOT_CHECK(calls_after_destroy == 0);
}
void pending_updates_during_submit_are_not_lost() {
  QWidget owner;
  auto *source = new AnalyzerSourceView(&owner);
  owner.resize(401,301);
  source->setGeometry(owner.rect());
  source->set_source_size({180,112});
  std::vector<analysis::SourceView> frames;
  bool inject = false;
  auto *controller = new SourcePresentationController(owner,*source,[] {
    SourcePresentationOptions options;
    options.settings.reference_white_nits = 100.;
    options.false_color = true;
    options.mask = {true,analysis::MaskShape::ellipse,{3.,4.,25.,26.}};
    options.mask_outside_factor = .4;
    return options;
  },[&](auto frame,auto) {
    frames.push_back(std::move(frame));
    if (inject) {
      inject = false;
      source->set_transform({false,2.,{11.,12.}});
    }
    return true;
  });
  source->view_changed = [controller] { controller->request_present(); };
  owner.show();
  wait();
  HDRSHOT_CHECK(!frames.empty());
  frames.clear();
  inject = true;
  controller->request_present();
  wait();
  HDRSHOT_CHECK(frames.size() == 2);
  matches_widget(frames.back(),*source);
  HDRSHOT_CHECK(frames.back().settings.reference_white_nits == 100.);
  HDRSHOT_CHECK(frames.back().false_color && frames.back().mask.enabled);
  HDRSHOT_CHECK(frames.back().mask.shape == analysis::MaskShape::ellipse);
  HDRSHOT_CHECK(frames.back().mask_outside_factor == .4);
  source->view_changed = {};
  controller->stop();
}
}
int main(int argc,char **argv) {
  QApplication app(argc,argv);
  app.setQuitOnLastWindowClosed(false);
  return test::run({
    {"display recovery clears its own error without clearing another failure",display_error_recovery_preserves_other_errors},
    {"two host geometry and independent display options",host_geometry_and_options},
    {"same batch merges without analysis or idle polling",batched_refresh_has_no_analysis_or_idle_work},
    {"hidden dirty view replays latest geometry on show",hidden_dirty_uses_latest_view_on_show},
    {"native child expose DPR and surface recreation",native_child_events_represent_both_hosts},
    {"cancel close retains refresh and accepted close cancels pending callback",close_cancel_keeps_refresh_and_accepted_close_stops_it},
    {"refresh during submission presents the latest view",pending_updates_during_submit_are_not_lost}
  });
}
