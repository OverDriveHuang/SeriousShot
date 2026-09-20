#include "platform/cpu/cpu_analysis_port.hpp"
#include "platform/macos/macos_analysis_backend.hpp"
#include "test_support.hpp"
#include "ui/qt/analyzer_source_view.hpp"
#include "ui/qt/analyzer_window.hpp"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QEventLoop>
#include <QTimer>
#include <QProcess>
#include <QToolButton>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QLabel>

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <numeric>

namespace {
using namespace hdrshot;
// Fault injection is local to this test process and this one analysis layer.
// The production presenter does not acquire a test-only branch or API.
std::atomic<std::uintptr_t> injected_layer{};
std::atomic<int> nil_drawables_remaining{}, injected_drawable_calls{};
IMP original_next_drawable{};
id<CAMetalDrawable> injected_next_drawable(id receiver, SEL selector) {
  if (reinterpret_cast<std::uintptr_t>((__bridge void *)receiver) ==
      injected_layer.load()) {
    ++injected_drawable_calls;
    int remaining = nil_drawables_remaining.load();
    while (remaining > 0)
      if (nil_drawables_remaining.compare_exchange_weak(remaining, remaining - 1))
        return nil;
  }
  using NextDrawable = id<CAMetalDrawable> (*)(id, SEL);
  return reinterpret_cast<NextDrawable>(original_next_drawable)(receiver, selector);
}
class ScopedNilDrawable final {
public:
  ScopedNilDrawable(CAMetalLayer *layer, int count) {
    method_ = class_getInstanceMethod(CAMetalLayer.class, @selector(nextDrawable));
    original_next_drawable = method_getImplementation(method_);
    injected_layer.store(reinterpret_cast<std::uintptr_t>((__bridge void *)layer));
    nil_drawables_remaining.store(count);
    injected_drawable_calls.store(0);
    method_setImplementation(method_, reinterpret_cast<IMP>(injected_next_drawable));
  }
  ~ScopedNilDrawable() {
    method_setImplementation(method_, original_next_drawable);
    injected_layer.store(0);
  }
  ScopedNilDrawable(const ScopedNilDrawable &) = delete;
  ScopedNilDrawable &operator=(const ScopedNilDrawable &) = delete;
private:
  Method method_{};
};
bool wait_for_quiescent_presenter(const AnalysisPresenterPort &presenter) {
  QElapsedTimer deadline;
  deadline.start();
  while (deadline.elapsed() < 5000) {
    const auto status = macos_analysis_presentation_status(presenter);
    if (!status.worker_active && !status.pending)
      return true;
    QEventLoop loop;
    QTimer::singleShot(10, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return false;
}
bool wait_for_presentation(const AnalysisPresenterPort &presenter,
                           std::uint64_t generation = 0) {
  QElapsedTimer deadline;
  deadline.start();
  for (;;) {
    const auto status = macos_analysis_presentation_status(presenter);
    if (status.failed)
      return false;
    if (status.requested && status.completed_generation >=
                                (generation ? generation : status.requested))
      return true;
    if (deadline.elapsed() > 5000)
      return false;
    QEventLoop loop;
    QTimer::singleShot(10, &loop, &QEventLoop::quit);
    loop.exec();
  }
}
void production_window_overlay_pixels() {
  analysis::FloatImage pixels(128 * 80);
  for (int y = 0; y < 80; ++y)
    for (int x = 0; x < 128; ++x)
      pixels[std::size_t(y * 128 + x)] = x < 64
          ? std::array<float, 4>{.5f, .02f, .01f, 1.f}
          : std::array<float, 4>{.01f, .5f, .02f, 1.f};
  auto cpu = make_cpu_analysis_source({128, 80}, std::move(pixels));
  HDRSHOT_CHECK(cpu.has_value());
  auto backend = make_macos_analysis_backend();
  HDRSHOT_CHECK(backend.has_value());
  SelectionRoiView roi;
  roi.size_px = {128, 80};
  roi.source_rect_px = {0, 0, 128, 80};
  roi.linear_source = cpu.value();
  roi.row_stride_samples = 128 * 4;
  roi.encoding = {ColorPrimaries::display_p3, TransferFunction::linear, AlphaMode::opaque, 0};
  AnnotationPixelPlan annotations;
  annotations.output_size_px = roi.size_px;
  for (int y = 0; y < 80; ++y)
    annotations.source_visible_spans.push_back({y, 0, 128});
  auto prepared = backend.value().port->prepare(roi, annotations);
  HDRSHOT_CHECK(prepared.has_value());
  analysis::Input input{prepared.value(), 1, false};
  AnalyzerWindow window(input);
  window.resize(1280, 780);
  window.set_close_confirmation([] { return true; });
  window.set_request_handler([&](const analysis::Request &request) {
    auto result = backend.value().port->analyze(input, request);
    HDRSHOT_CHECK(result.has_value());
    window.accept_result(result.value());
  });
  analysis::SourceView presented;
  window.set_present_handler([&](analysis::SourceView view, std::uintptr_t surface) {
    HDRSHOT_CHECK(view.operation_overlay != nullptr);
    HDRSHOT_CHECK(backend.value().presenter->present(input, view, surface).has_value());
    presented = std::move(view);
  });
  window.show();
  window.add_swatch({32, 40}, 1);
  window.add_swatch({96, 40}, 11);
  auto *source = window.source_widget();
  QWidget *surface = source->presentation_surface();
  QEventLoop loop;
  QTimer::singleShot(300, &loop, &QEventLoop::quit);
  loop.exec();
  HDRSHOT_CHECK(wait_for_presentation(*backend.value().presenter));
  QWidget *overlay = source->findChild<QWidget *>("analyzerSourceOverlay");
  HDRSHOT_CHECK(overlay && overlay->isHidden() && !overlay->isVisible());
  if (overlay->testAttribute(Qt::WA_NativeWindow)) {
    NSView *hidden = (__bridge NSView *)reinterpret_cast<void *>(overlay->winId());
    HDRSHOT_CHECK(hidden.isHiddenOrHasHiddenAncestor);
  }
  NSView *native_surface = (__bridge NSView *)reinterpret_cast<void *>(surface->winId());
  CALayer *visible_source = nil;
  for (CALayer *candidate in native_surface.layer.sublayers)
    if ([candidate.name isEqualToString:@"hdrshot.analysis.source"])
      visible_source = candidate;
  HDRSHOT_CHECK(visible_source != nil && !visible_source.hidden && visible_source.zPosition == 1);
  for (CALayer *candidate in native_surface.layer.sublayers)
    if (candidate != visible_source)
      HDRSHOT_CHECK(candidate.zPosition < visible_source.zPosition);
  HDRSHOT_CHECK(presented.operation_overlay != nullptr);
  const auto &marks = *presented.operation_overlay;
  const auto center = (std::size_t(marks.size.height / 2) * std::size_t(marks.size.width) + std::size_t(marks.size.width / 2)) * 4;
  HDRSHOT_CHECK(marks.rgba[center + 3] == 0);
  std::size_t marked = 0;
  for (std::size_t i = 3; i < marks.rgba.size(); i += 4)
    marked += marks.rgba[i] != 0;
  HDRSHOT_CHECK(marked > 10 && marked < marks.rgba.size() / 20);
  // Exercise the actual native Qt child receiver without a preceding press.
  const QPointF hover = source->local_at({45, 25});
  QMouseEvent moved(QEvent::MouseMove, hover, surface->mapToGlobal(hover.toPoint()),
                    Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(surface, &moved);
  QApplication::processEvents();
  auto *readout = window.findChild<QLabel *>("analyzerHoverReadout");
  HDRSHOT_CHECK(readout && readout->text().contains("Y · nit"));
  // The visible-window check captures ONLY this synthetic test window. No
  // desktop/other-app capture, permission prompt or settings mutation.
  const QString directory = qEnvironmentVariable("HDRSHOT_ANALYZER_NATIVE_SCREENSHOT_DIR");
  if (!directory.isEmpty() && qEnvironmentVariableIsSet("HDRSHOT_ANALYZER_WINDOWSERVER_CAPTURE") && CGPreflightScreenCaptureAccess()) {
    QDir().mkpath(directory);
    NSView *native = (__bridge NSView *)reinterpret_cast<void *>(surface->winId());
    const QString path = directory + "/" + QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz") + "_windowserver.png";
    QProcess capture;
    capture.start("/usr/sbin/screencapture", {"-x", "-o", "-l", QString::number(native.window.windowNumber), path});
    HDRSHOT_CHECK(capture.waitForFinished(5000));
    std::cout << "CAPTURE window=" << native.window.windowNumber << " visible=" << native.window.isVisible
              << " occlusion=" << native.window.occlusionState << " exit=" << capture.exitCode()
              << " error=" << capture.readAllStandardError().toStdString() << '\n';
    HDRSHOT_CHECK(capture.exitCode() == 0);
    const QImage image(path);
    HDRSHOT_CHECK(!image.isNull());
    const double ratio = image.width() / native.window.frame.size.width;
    const double title = native.window.frame.size.height - native.window.contentView.bounds.size.height;
    const auto at = [&](QPointF p) {
      const QPointF location = source->mapTo(&window, source->local_at(p).toPoint());
      return image.pixelColor(int(location.x() * ratio), int((title + location.y()) * ratio));
    };
    const auto red = at({25, 20}), green = at({100, 20});
    std::cout << "WINDOWSERVER source red=" << red.name().toStdString() << " green=" << green.name().toStdString() << '\n';
    HDRSHOT_CHECK(red.red() > red.green() + 50);
    HDRSHOT_CHECK(green.green() > green.red() + 50);
    std::cout << "[SCREENSHOT] " << path.toStdString() << '\n';
  } else
    std::cout << "[NOT VERIFIED] Final WindowServer capture requires an unlocked desktop, permission and HDRSHOT_ANALYZER_WINDOWSERVER_CAPTURE=1; no prompt issued\n";
  window.set_present_handler({});
  window.set_request_handler({});
  window.hide();
}
void native_single_surface_and_float_source() {
  AnalyzerSourceView source;
  source.set_source_size({128, 80});
  source.resize(640, 400);
  source.set_pins({{1, {64, 40}, 1}, {2, {90, 55}, 31}});
  source.set_mask(AnalyzerSourceMask{QRectF(20, 15, 85, 50), false});
  source.show();
  QApplication::processEvents();
  QWidget *surface = source.presentation_surface();
  QWidget *overlay = source.findChild<QWidget *>("analyzerSourceOverlay");
  HDRSHOT_CHECK(overlay && surface->testAttribute(Qt::WA_NativeWindow) &&
                overlay->isHidden() && !overlay->isVisible());
  NSView *surface_view =
      (__bridge NSView *)reinterpret_cast<void *>(surface->winId());
  HDRSHOT_CHECK(surface_view);
  HDRSHOT_CHECK(overlay->testAttribute(Qt::WA_TransparentForMouseEvents));
  auto backend = make_macos_analysis_backend();
  HDRSHOT_CHECK(backend.has_value());
  analysis::FloatImage pixels(128 * 80, {.04f, .1f, .2f, 1.f});
  for (int y = 20; y < 60; ++y)
    for (int x = 40; x < 88; ++x)
      pixels[std::size_t(y * 128 + x)] = {4.f, 2.f, .5f, 1.f};
  auto input_source = make_cpu_analysis_source({128, 80}, std::move(pixels));
  HDRSHOT_CHECK(input_source.has_value());
  SelectionRoiView roi;
  roi.size_px = {128, 80};
  roi.source_rect_px = {0, 0, 128, 80};
  roi.encoding = {ColorPrimaries::display_p3, TransferFunction::linear,
                  AlphaMode::opaque, 0};
  roi.linear_source = input_source.value();
  roi.row_stride_samples = 128 * 4;
  AnnotationPixelPlan annotation_plan;
  annotation_plan.output_size_px = roi.size_px;
  for (int y = 0; y < 80; ++y)
    annotation_plan.source_visible_spans.push_back({y, 0, 128});
  auto native_source = backend.value().port->prepare(roi, annotation_plan);
  HDRSHOT_CHECK(native_source.has_value());
  analysis::Input input{native_source.value(), 1, true};
  analysis::SourceView view;
  const double dpr = source.devicePixelRatioF();
  view.target_size = {int(source.width() * dpr), int(source.height() * dpr)};
  view.scale = source.effective_scale() * dpr;
  view.settings.working_space = analysis::WorkingSpace::display_p3_pq;
  view.mask = {true, analysis::MaskShape::rectangle, {20, 15, 85, 50}};
  auto presented = backend.value().presenter->present(
      input, view, std::uintptr_t(surface->winId()));
  HDRSHOT_CHECK(presented.has_value());
  HDRSHOT_CHECK(wait_for_presentation(*backend.value().presenter));
  // QCocoaView owns its backing layer. The presenter must attach its actual
  // Metal layer below it, not replace the Qt-owned root.
  const auto metal_layer = [&]() -> CAMetalLayer * {
    CAMetalLayer *found = nil;
    std::size_t count = 0;
    for (CALayer *candidate in surface_view.layer.sublayers)
      if ([candidate.name isEqualToString:@"hdrshot.analysis.source"]) {
        HDRSHOT_CHECK([candidate isKindOfClass:CAMetalLayer.class]);
        found = (CAMetalLayer *)candidate;
        ++count;
      }
    HDRSHOT_CHECK(count == 1);
    return found;
  };
  CAMetalLayer *layer = metal_layer();
  HDRSHOT_CHECK(layer.device != nil);
  HDRSHOT_CHECK(layer.wantsExtendedDynamicRangeContent);
  HDRSHOT_CHECK(layer.pixelFormat == MTLPixelFormatRGBA16Float);
  HDRSHOT_CHECK(CGSizeEqualToSize(
      layer.drawableSize,
      CGSizeMake(view.target_size.width, view.target_size.height)));
  HDRSHOT_CHECK(CGRectEqualToRect(layer.frame, surface_view.bounds));
  presented = backend.value().presenter->present(
      input, view, std::uintptr_t(surface->winId()));
  HDRSHOT_CHECK(presented.has_value());
  HDRSHOT_CHECK(metal_layer() == layer);
  QApplication::processEvents();
  HDRSHOT_CHECK(metal_layer() == layer);
  HDRSHOT_CHECK((__bridge NSView *)reinterpret_cast<void *>(surface->winId()) ==
                surface_view);
  HDRSHOT_CHECK(layer.zPosition > 0 && overlay->isHidden());
  auto float_image = macos_analysis_render_offscreen(input, view);
  HDRSHOT_CHECK(float_image.has_value());
  auto center = float_image.value()->read_region(
      {view.target_size.width / 2, view.target_size.height / 2, 1, 1});
  HDRSHOT_CHECK(center.has_value());
  HDRSHOT_CHECK(center.value()[0] > 1.f);
  const QString output =
      qEnvironmentVariable("HDRSHOT_ANALYZER_NATIVE_SCREENSHOT_DIR");
  if (!output.isEmpty()) {
    QDir().mkpath(output);
    // Use the exact native renderer's explicit float readback for the source,
    // then the exact native Qt operation layer. This is an SDR layout proof;
    // the >1 assertion above is the separate HDR numerical proof.
    auto full = float_image.value()->read_region(
        {0, 0, view.target_size.width, view.target_size.height});
    HDRSHOT_CHECK(full.has_value());
    QImage preview(view.target_size.width, view.target_size.height,
                   QImage::Format_RGB32);
    for (int y = 0; y < preview.height(); ++y)
      for (int x = 0; x < preview.width(); ++x) {
        const auto i =
            (std::size_t(y) * std::size_t(preview.width()) + std::size_t(x)) *
            4;
        auto c = analysis::display_srgb(
            {full.value()[i], full.value()[i + 1], full.value()[i + 2]},
            analysis::WorkingSpace::display_p3_pq);
        preview.setPixelColor(x, y, QColor::fromRgbF(c[0], c[1], c[2]));
      }
    preview.setDevicePixelRatio(dpr);
    {
      QPainter painter(&preview);
      painter.drawImage(QPoint(), source.retained_overlay(dpr));
    }
    const QString path =
        output + "/" +
        QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz") +
        "_gpu_readback_reference.png";
    HDRSHOT_CHECK(preview.save(path));
    std::cout << "[SCREENSHOT] " << path.toStdString() << '\n';
  }
  // No sleeps, CPU/GPU waits, or event pumping between these calls. This
  // records GUI enqueue latency. Completion is observed separately below;
  // accepted requests are not mislabeled as displayed/completed frames.
  std::array<double, 30> present_ms{};
  std::size_t completed = 0;
  for (std::size_t i = 0; i < present_ms.size(); ++i) {
    view.offset_x = double(i % 11) - 5.;
    view.offset_y = double(i % 7) - 3.;
    const auto began = std::chrono::steady_clock::now();
    const auto result = backend.value().presenter->present(
        input, view, std::uintptr_t(surface->winId()));
    present_ms[i] = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - began)
                        .count();
    if (!result) {
      std::cerr << "RAPID_PRESENT failure iteration=" << i
                << " wall_ms=" << present_ms[i] << '\n';
      for (const auto &[key, value] : result.error().safe_context)
        std::cerr << key << '=' << value << '\n';
      break;
    }
    ++completed;
  }
  std::cout << "RAPID_PRESENT source=128x80 target=" << view.target_size.width
            << 'x' << view.target_size.height << " accepted=" << completed
            << " min_ms="
            << *std::min_element(present_ms.begin(), present_ms.end())
            << " mean_ms="
            << std::accumulate(present_ms.begin(), present_ms.end(), 0.) / 30.
            << " max_ms="
            << *std::max_element(present_ms.begin(), present_ms.end())
            << " above16ms="
            << std::count_if(present_ms.begin(), present_ms.end(),
                             [](double ms) { return ms > 16.; })
            << '\n';
  HDRSHOT_CHECK(completed == present_ms.size());
  const auto queued = macos_analysis_presentation_status(*backend.value().presenter);
  HDRSHOT_CHECK(wait_for_presentation(*backend.value().presenter, queued.requested));
  const auto drained = macos_analysis_presentation_status(*backend.value().presenter);
  std::cout << "ASYNC_PRESENT requested=" << drained.requested
            << " submitted=" << drained.submitted << " completed=" << drained.completed
            << " completed_generation=" << drained.completed_generation
            << " failed=" << drained.failed << '\n';
  HDRSHOT_CHECK(metal_layer() == layer);
  source.resize(720, 450);
  QApplication::processEvents();
  view.target_size = {int(source.width() * dpr), int(source.height() * dpr)};
  view.scale = source.effective_scale() * dpr;
  HDRSHOT_CHECK(
      backend.value()
          .presenter->present(input, view, std::uintptr_t(surface->winId()))
          .has_value());
  HDRSHOT_CHECK(wait_for_presentation(*backend.value().presenter));
  HDRSHOT_CHECK(metal_layer() == layer);
  HDRSHOT_CHECK(CGRectEqualToRect(layer.frame, surface_view.bounds));
  HDRSHOT_CHECK(CGSizeEqualToSize(
      layer.drawableSize,
      CGSizeMake(view.target_size.width, view.target_size.height)));
  HDRSHOT_CHECK(overlay->isHidden());
  HDRSHOT_CHECK(wait_for_quiescent_presenter(*backend.value().presenter));
  {
    ScopedNilDrawable inject(layer, 1);
    HDRSHOT_CHECK(backend.value().presenter->present(
        input, view, std::uintptr_t(surface->winId())).has_value());
    const auto requested = macos_analysis_presentation_status(*backend.value().presenter).requested;
    // No second present or user gesture: one temporarily absent drawable must
    // replay the queued latest generation by itself.
    HDRSHOT_CHECK(wait_for_presentation(*backend.value().presenter, requested));
    HDRSHOT_CHECK(wait_for_quiescent_presenter(*backend.value().presenter));
    const auto status = macos_analysis_presentation_status(*backend.value().presenter);
    HDRSHOT_CHECK(status.requested == requested && status.completed_generation == requested);
    HDRSHOT_CHECK(status.failed == 0 && injected_drawable_calls.load() >= 2);
    std::cout << "DRAWABLE_RETRY generation=" << requested
              << " calls=" << injected_drawable_calls.load() << " failed=0\n";
  }
  {
    ScopedNilDrawable inject(layer, 100);
    HDRSHOT_CHECK(backend.value().presenter->present(
        input, view, std::uintptr_t(surface->winId())).has_value());
    const auto requested = macos_analysis_presentation_status(*backend.value().presenter).requested;
    source.hide();
    // A persistently unavailable hidden surface must drain bounded retries,
    // not leave an eternal pending frame or worker spin after closing/hiding.
    HDRSHOT_CHECK(wait_for_quiescent_presenter(*backend.value().presenter));
    const auto status = macos_analysis_presentation_status(*backend.value().presenter);
    HDRSHOT_CHECK(status.requested == requested && !status.pending && !status.worker_active);
    HDRSHOT_CHECK(status.failed == 1 && injected_drawable_calls.load() <= 3);
    std::cout << "HIDDEN_DRAWABLE_DRAIN calls=" << injected_drawable_calls.load()
              << " expected_terminal_failure=" << status.failed << " pending=0\n";
  }
}
} // namespace
int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "cocoa");
  QApplication app(argc, argv);
  if (!MTLCreateSystemDefaultDevice() || ![NSScreen screens].count) {
    std::cerr
        << "[ENVIRONMENT NOT VERIFIED] Native Metal/WindowServer unavailable\n";
    return 77;
  }
  return hdrshot::test::run(
      {{"production window overlay pixels", production_window_overlay_pixels},
       {"single native analyzer surface and >1 float source",
        native_single_surface_and_float_source}});
}
