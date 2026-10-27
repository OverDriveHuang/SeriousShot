#include "platform/cpu/cpu_analysis_port.hpp"
#include "platform/macos/macos_analysis_backend.hpp"
#include "test_support.hpp"
#include "ui/qt/analyzer_source_view.hpp"
#include "ui/qt/analyzer_window.hpp"
#include "ui/qt/pinned_screenshot_window.hpp"
#include "ui/qt/source_presentation_controller.hpp"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QExposeEvent>
#include <QImage>
#include <QPlatformSurfaceEvent>
#include <QProcess>
#include <QScreen>
#include <QThread>
#include <QWindow>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>

namespace {
using namespace hdrshot;
bool unlocked_session() {
  CFDictionaryRef session = CGSessionCopyCurrentDictionary();
  if (!session)
    return false;
  const auto locked = CFDictionaryGetValue(session, CFSTR("CGSSessionScreenIsLocked"));
  const auto console = CFDictionaryGetValue(session, kCGSessionOnConsoleKey);
  const bool result = locked != kCFBooleanTrue && console == kCFBooleanTrue;
  CFRelease(session);
  return result;
}
bool spin(const std::function<bool()> &ready, int timeout = 6000) {
  QElapsedTimer timer;
  timer.start();
  while (!ready() && timer.elapsed() < timeout) {
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(2);
  }
  return ready();
}
void pump(int milliseconds = 100) {
  QElapsedTimer timer;
  timer.start();
  spin([&] { return timer.elapsed() >= milliseconds; }, milliseconds + 1000);
}
struct Fixture {
  MacAnalysisBackend backend;
  analysis::Input input;
  explicit Fixture(bool hdr = false) {
    auto native = make_macos_analysis_backend();
    HDRSHOT_CHECK(native.has_value());
    backend = std::move(native.value());
    analysis::FloatImage pixels(128 * 80);
    for (int y = 0; y < 80; ++y)
      for (int x = 0; x < 128; ++x)
        pixels[std::size_t(y * 128 + x)] = x < 64
            ? std::array<float, 4>{hdr ? 3.f : .5f, .02f, .01f, 1.f}
            : std::array<float, 4>{.01f, hdr ? 2.f : .5f, .02f, 1.f};
    auto cpu = make_cpu_analysis_source({128, 80}, std::move(pixels));
    HDRSHOT_CHECK(cpu.has_value());
    SelectionRoiView roi;
    roi.size_px = {128, 80};
    roi.source_rect_px = {0, 0, 128, 80};
    roi.linear_source = cpu.value();
    roi.row_stride_samples = 128 * 4;
    roi.encoding = {ColorPrimaries::display_p3, TransferFunction::linear,
                    AlphaMode::opaque, 0};
    AnnotationPixelPlan annotations;
    annotations.output_size_px = roi.size_px;
    for (int y = 0; y < 80; ++y)
      annotations.source_visible_spans.push_back({y, 0, 128});
    auto source = backend.port->prepare(roi, annotations);
    HDRSHOT_CHECK(source.has_value());
    input = {source.value(), 1, hdr};
  }
};
struct Observation {
  Fixture &fixture;
  AnalyzerSourceView &source;
  std::size_t calls{};
  std::uintptr_t surface{};
  analysis::SourceView latest;
  Observation(Fixture &data, AnalyzerSourceView &view) : fixture(data), source(view) {}
  void submit(analysis::SourceView view, std::uintptr_t handle) {
    // This is the production window's submit callback, not a direct test call
    // to a controller or a replacement presenter.
    HDRSHOT_CHECK(handle != 0);
    HDRSHOT_CHECK(fixture.backend.presenter->present(fixture.input, view, handle).has_value());
    latest = std::move(view);
    surface = handle;
    ++calls;
  }
  MacAnalysisPresentationStatus status() const {
    return macos_analysis_presentation_status(*fixture.backend.presenter);
  }
  void drain() const {
    HDRSHOT_CHECK(spin([&] {
      auto state = status();
      return !state.worker_active && !state.pending;
    }));
  }
  void arrived(std::size_t previous) const {
    HDRSHOT_CHECK(spin([&] { return calls > previous; }));
    drain();
    if (unlocked_session()) {
      const auto state = status();
      HDRSHOT_CHECK(state.requested > 0);
      HDRSHOT_CHECK(state.completed_generation == state.requested);
      HDRSHOT_CHECK(state.failed == 0);
    }
  }
  CAMetalLayer *layer() const {
    auto *widget = source.presentation_widget();
    HDRSHOT_CHECK(widget->testAttribute(Qt::WA_NativeWindow));
    HDRSHOT_CHECK(surface == std::uintptr_t(widget->winId()));
    NSView *native = (__bridge NSView *)reinterpret_cast<void *>(surface);
    HDRSHOT_CHECK(native && native.window && native.layer);
    CAMetalLayer *found = nil;
    std::size_t count = 0;
    for (CALayer *candidate in native.layer.sublayers)
      if ([candidate.name isEqualToString:@"hdrshot.analysis.source"]) {
        HDRSHOT_CHECK([candidate isKindOfClass:CAMetalLayer.class]);
        found = (CAMetalLayer *)candidate;
        ++count;
      }
    HDRSHOT_CHECK(count == 1 && found.device && !found.hidden);
    HDRSHOT_CHECK(found.pixelFormat == MTLPixelFormatRGBA16Float);
    HDRSHOT_CHECK(found.wantsExtendedDynamicRangeContent);
    CGColorSpaceRef expected = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearDisplayP3);
    const bool correct_space = found.colorspace && expected && CFEqual(found.colorspace, expected);
    CGColorSpaceRelease(expected);
    HDRSHOT_CHECK(correct_space);
    HDRSHOT_CHECK(found.zPosition == 1);
    HDRSHOT_CHECK(CGRectEqualToRect(found.frame, native.bounds));
    HDRSHOT_CHECK(CGSizeEqualToSize(found.drawableSize,
        CGSizeMake(latest.target_size.width, latest.target_size.height)));
    const double dpr = source.devicePixelRatioF();
    HDRSHOT_CHECK(latest.target_size.width == int(std::lround(source.width() * dpr)));
    HDRSHOT_CHECK(latest.target_size.height == int(std::lround(source.height() * dpr)));
    HDRSHOT_CHECK_NEAR(latest.scale, source.effective_scale() * dpr, 1e-9);
    HDRSHOT_CHECK_NEAR(latest.offset_x, source.image_rect().x() * dpr, 1e-9);
    HDRSHOT_CHECK_NEAR(latest.offset_y, source.image_rect().y() * dpr, 1e-9);
    return found;
  }
};
void require_controller(QWidget &owner) {
  std::size_t controllers = 0;
  for (auto *child : owner.children())
    controllers += dynamic_cast<SourcePresentationController *>(child) != nullptr;
  // Pin owns the main and thumbnail schedulers; its passive card owns the third.
  HDRSHOT_CHECK(controllers == (dynamic_cast<PinnedScreenshotWindow *>(&owner) ? 2U : 1U));
}
void child_events(Observation &observed) {
  QWidget *child = observed.source.presentation_widget();
  QWindow *native = child->windowHandle();
  HDRSHOT_CHECK(native);
  const auto original_size = child->size();
  CAMetalLayer *original_layer = observed.layer();
  auto count = observed.calls;
  // Actual QWidget/native child resize, without resizing analyzer charts.
  child->resize(original_size - QSize(7, 9));
  observed.arrived(count);
  HDRSHOT_CHECK(observed.layer() == original_layer);
  count = observed.calls;
  child->resize(original_size);
  observed.arrived(count);
  HDRSHOT_CHECK(observed.layer() == original_layer);

  count = observed.calls;
  // Inject through the real Cocoa QWindow receiver. This checks routing; it
  // does not claim a real system Expose or a changed physical display DPR.
  QExposeEvent expose(QRegion(child->rect()));
  QApplication::sendEvent(native, &expose);
  QEvent dpr(QEvent::DevicePixelRatioChange);
  QApplication::sendEvent(native, &dpr);
  QApplication::sendEvent(child, &dpr);
  observed.arrived(count);
  HDRSHOT_CHECK(observed.layer() == original_layer);

  child->hide();
  count = observed.calls;
  QExposeEvent hidden_expose(QRegion(child->rect()));
  QApplication::sendEvent(native, &hidden_expose);
  pump();
  HDRSHOT_CHECK(observed.calls == count);
  child->show();
  observed.arrived(count);
  HDRSHOT_CHECK(observed.layer() == original_layer);

  count = observed.calls;
  // Lifetime fault injection keeps the real native view alive. Destruction of
  // a QWidget-owned Cocoa QWindow behind QWidget's back is not a safe fixture.
  // Real native destruction is covered below by destroying entire owners.
  QPlatformSurfaceEvent destroyed(QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed);
  QApplication::sendEvent(native, &destroyed);
  QApplication::sendEvent(native, &expose);
  pump();
  HDRSHOT_CHECK(observed.calls == count);
  QPlatformSurfaceEvent created(QPlatformSurfaceEvent::SurfaceCreated);
  QApplication::sendEvent(native, &created);
  observed.arrived(count);
  HDRSHOT_CHECK(observed.layer() == original_layer);
}

// Optional compositor pixels. Never call QWidget::grab/render, layer.display,
// nextDrawable, or an offscreen renderer to inspect stale-display recovery.
void windowserver_pixels(QWidget &owner, Observation &observed, const QString &stage) {
  if (!qEnvironmentVariableIsSet("HDRSHOT_SOURCE_PRESENTATION_WINDOWSERVER_CAPTURE"))
    return;
  if (!unlocked_session() || !CGPreflightScreenCaptureAccess()) {
    std::cout << "[SKIP] WindowServer " << stage.toStdString()
              << ": locked/non-console session or capture permission unavailable; no prompt issued\n";
    return;
  }
  const auto directory = qEnvironmentVariable("HDRSHOT_SOURCE_PRESENTATION_CAPTURE_DIR");
  HDRSHOT_CHECK(!directory.isEmpty() && QDir().mkpath(directory));
  NSView *native = (__bridge NSView *)reinterpret_cast<void *>(observed.surface);
  // Do not order/activate this window here: a probe-induced Expose could
  // repaint and conceal the stale-frame defect this check is meant to catch.
  if (!native.window.visible || !(native.window.occlusionState & NSWindowOcclusionStateVisible)) {
    std::cout << "[SKIP] WindowServer " << stage.toStdString()
              << ": test window is occluded; probe did not reveal or repaint it\n";
    return;
  }
  const auto path = directory + "/" +
      QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz") + "_" + stage + ".png";
  QProcess capture;
  capture.start("/usr/sbin/screencapture", {"-x", "-o", "-l",
      QString::number(native.window.windowNumber), path});
  HDRSHOT_CHECK(capture.waitForFinished(5000) && capture.exitCode() == 0);
  QImage image(path);
  HDRSHOT_CHECK(!image.isNull());
  const double ratio = image.width() / native.window.frame.size.width;
  const double title_height = native.window.frame.size.height - native.window.contentView.bounds.size.height;
  const auto pixel = [&](QPointF source_point) {
    const QPoint local = observed.source.mapTo(&owner, observed.source.local_at(source_point).toPoint());
    const QPoint at(int(std::lround(local.x() * ratio)),
                    int(std::lround((title_height + local.y()) * ratio)));
    HDRSHOT_CHECK(image.rect().contains(at));
    return image.pixelColor(at);
  };
  const auto red = pixel({25, 40}), green = pixel({100, 40});
  HDRSHOT_CHECK(red.red() > red.green() + 40);
  HDRSHOT_CHECK(green.green() > green.red() + 40);
  std::cout << "[WINDOWSERVER] " << path.toStdString() << '\n';
}

void native_mixed_hosts() {
  Fixture analyzer_fixture, first_fixture, second_fixture(true);
  auto analyzer = std::make_unique<AnalyzerWindow>(analyzer_fixture.input);
  auto first = std::make_unique<PinnedScreenshotWindow>(first_fixture.input, PinnedScreenshotDefaults{});
  auto second = std::make_unique<PinnedScreenshotWindow>(second_fixture.input, PinnedScreenshotDefaults{false, 400});
  // Establish visible, separate fixtures before any presentation is observed.
  // Default cascading positions can fully cover a pin when another owner shows.
  const auto available = QGuiApplication::primaryScreen()->availableGeometry();
  analyzer->resize(std::min(1280, available.width() - 560),
                   std::min(780, available.height() - 60));
  analyzer->move(available.topLeft() + QPoint(20, 30));
  first->resize(450, 300);
  second->resize(450, 300);
  first->move(available.topLeft() + QPoint(available.width() - 480, 30));
  second->move(available.topLeft() + QPoint(available.width() - 480, 400));
  Observation analyzer_output{analyzer_fixture, *analyzer->source_widget()};
  Observation first_output{first_fixture, *first->source_view()};
  Observation second_output{second_fixture, *second->source_view()};
  std::size_t analyses = 0;
  analyzer->set_request_handler([&](const analysis::Request &request) {
    ++analyses;
    auto result = analyzer_fixture.backend.port->analyze(analyzer_fixture.input, request);
    HDRSHOT_CHECK(result.has_value());
    analyzer->accept_result(result.value());
  });
  analyzer->set_present_handler([&](auto view, auto surface) { analyzer_output.submit(std::move(view), surface); });
  first->present_requested = [&](auto view, auto surface) { first_output.submit(std::move(view), surface); };
  second->present_requested = [&](auto view, auto surface) { second_output.submit(std::move(view), surface); };
  for (QWidget *owner : std::array<QWidget *, 3>{analyzer.get(), first.get(), second.get()}) {
    require_controller(*owner);
    owner->show();
  }
  analyzer_output.arrived(0);
  first_output.arrived(0);
  second_output.arrived(0);
  pump(350); // Let startup chart-resolution refinement settle before counting.
  HDRSHOT_CHECK(analyses > 0);
  // This regression observes presentation, not physical-pointer sampling.
  // Incidental OS hover as these windows show/hide must not create a new
  // sample request and obscure the display-only analysis-count assertion.
  analyzer->source_widget()->hover_changed = {};
  const auto analyses_before = analyses;
  const auto revision_before = analyzer->current_request().revision;
  CAMetalLayer *analyzer_layer = analyzer_output.layer();
  CAMetalLayer *first_layer = first_output.layer();
  CAMetalLayer *second_layer = second_output.layer();
  HDRSHOT_CHECK(analyzer_layer != first_layer && first_layer != second_layer && second_layer != analyzer_layer);
  HDRSHOT_CHECK(analyzer_fixture.backend.presenter != first_fixture.backend.presenter &&
                first_fixture.backend.presenter != second_fixture.backend.presenter);
  HDRSHOT_CHECK(bool(analyzer_output.latest.operation_overlay));
  HDRSHOT_CHECK(!first_output.latest.operation_overlay && !second_output.latest.operation_overlay);
  HDRSHOT_CHECK(second_output.latest.settings.working_space == analysis::WorkingSpace::display_p3_pq);
  HDRSHOT_CHECK(first_output.latest.settings.working_space == analysis::WorkingSpace::display_p3_sdr);
  windowserver_pixels(*analyzer, analyzer_output, "analyzer_first_frame");
  windowserver_pixels(*first, first_output, "pin_first_frame");

  child_events(analyzer_output);
  child_events(first_output);
  child_events(second_output);
  HDRSHOT_CHECK(analyses == analyses_before);
  HDRSHOT_CHECK(analyzer->current_request().revision == revision_before);

  // A rejected close must leave the live controller and native target active.
  analyzer->set_close_confirmation([] { return false; });
  HDRSHOT_CHECK(!analyzer->close() && analyzer->isVisible());
  const auto before_cancel = analyzer_output.calls;
  analyzer->refresh_presentation();
  analyzer_output.arrived(before_cancel);
  HDRSHOT_CHECK(analyzer_output.layer() == analyzer_layer);
  windowserver_pixels(*analyzer, analyzer_output, "analyzer_cancel_close");

  for (auto *observed : {&analyzer_output, &first_output, &second_output}) {
    QWidget *owner = observed == &analyzer_output ? static_cast<QWidget *>(analyzer.get()) :
                     observed == &first_output ? first.get() : second.get();
    owner->hide();
    const auto count = observed->calls;
    observed->source.set_transform({false, 2.5, {7., -4.}});
    pump();
    HDRSHOT_CHECK(observed->calls == count);
    owner->show();
    observed->arrived(count);
    (void)observed->layer();
    // Inspect the restored pin before the next test owner can cover it.
    // The probe must not raise/repaint an occluded window to obtain evidence.
    if (observed == &first_output)
      windowserver_pixels(*first, first_output, "pin_hidden_restore");
  }
  pump(250);
  HDRSHOT_CHECK(analyses == analyses_before);
  HDRSHOT_CHECK(analyzer->current_request().revision == revision_before);

  // Persistent pins are display-only marks here: no new sample/analysis is
  // requested. The live operation overlay has ink; report SourceView stays clean.
  const auto before_marks = analyzer_output.calls;
  analyzer->source_widget()->set_pins({{1, {32, 40}, 1}});
  analyzer_output.arrived(before_marks);
  HDRSHOT_CHECK(bool(analyzer_output.latest.operation_overlay));
  const auto &marks = analyzer_output.latest.operation_overlay->rgba;
  std::size_t ink = 0;
  for (std::size_t alpha = 3; alpha < marks.size(); alpha += 4)
    ink += marks[alpha] != 0;
  HDRSHOT_CHECK(ink > 0);
  HDRSHOT_CHECK(!analyzer->source_view().operation_overlay);
  const auto plan = analyzer->report_plan(); // Explicit report operation, after refresh checks.
  HDRSHOT_CHECK(!plan.source_view.operation_overlay);
  const auto report = analyzer_fixture.backend.port->compose_report(analyzer_fixture.input, plan);
  HDRSHOT_CHECK(report.has_value());
  auto polluted = plan;
  polluted.source_view.operation_overlay = analyzer_output.latest.operation_overlay;
  HDRSHOT_CHECK(!analyzer_fixture.backend.port->compose_report(analyzer_fixture.input, polluted));
  const auto original = analyzer_fixture.input.source->read_region({0, 0, 1, 1});
  HDRSHOT_CHECK(original && original.value()[0] == .5f && original.value()[3] == 1.f);
  pump(250);
  HDRSHOT_CHECK(analyses == analyses_before);

  // Destroy one actual native window while its latest controller timer is
  // pending. Its presenter survives long enough to drain queued GPU work.
  first->refresh_presentation();
  const auto first_closed_count = first_output.calls;
  HDRSHOT_CHECK(first->close());
  first.reset();
  pump();
  HDRSHOT_CHECK(first_output.calls == first_closed_count);
  first_output.drain();
  const auto second_before = second_output.calls;
  second->refresh_presentation();
  second_output.arrived(second_before);
  HDRSHOT_CHECK(second_output.layer() == second_layer);
  const auto analyzer_before = analyzer_output.calls;
  analyzer->refresh_presentation();
  analyzer_output.arrived(analyzer_before);

  analyzer->set_close_confirmation([] { return true; });
  analyzer->refresh_presentation();
  const auto analyzer_closed_count = analyzer_output.calls;
  HDRSHOT_CHECK(analyzer->close());
  analyzer.reset();
  pump();
  HDRSHOT_CHECK(analyzer_output.calls == analyzer_closed_count);
  analyzer_output.drain();
  const auto remaining_before = second_output.calls;
  second->source_view()->set_transform({false, 2.75, {-3., 6.}});
  second_output.arrived(remaining_before);
  HDRSHOT_CHECK(second_output.layer() == second_layer);
  windowserver_pixels(*second, second_output, "last_pin_after_analyzer_destroyed");
  HDRSHOT_CHECK(analyses == analyses_before);
  HDRSHOT_CHECK(second->close());
  second.reset();
  second_output.drain();
  std::cout << "native mixed hosts: real Cocoa layers, independent presenters, child resize/hide/show, "
               "injected Expose/DPR/surface events, close cancellation and actual owner destruction passed\n";
}
} // namespace
int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "cocoa");
  QApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);
  if (!MTLCreateSystemDefaultDevice() || !NSScreen.screens.count) {
    std::cout << "[SKIP] Native Metal/WindowServer unavailable\n";
    return 77;
  }
  if (!unlocked_session())
    std::cout << "[SKIP] Locked/non-console session: drawable completion and WindowServer pixels; "
                 "native layer/event/lifetime assertions still run\n";
  if (!qEnvironmentVariableIsSet("HDRSHOT_SOURCE_PRESENTATION_WINDOWSERVER_CAPTURE"))
    std::cout << "[NOT VERIFIED] Final WindowServer pixels require explicit "
                 "HDRSHOT_SOURCE_PRESENTATION_WINDOWSERVER_CAPTURE=1 and HDRSHOT_SOURCE_PRESENTATION_CAPTURE_DIR\n";
  return test::run({{"Mac mixed-host shared SourcePresentationController native regression", native_mixed_hosts}});
}
