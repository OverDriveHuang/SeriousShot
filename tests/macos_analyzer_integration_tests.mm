#include "adapters/shared/libultrahdr_encoder.hpp"
#include "application/analysis_workflow.hpp"
#include "platform/macos/macos_analysis_backend.hpp"
#include "platform/macos/macos_metal_export_pixel_processor.hpp"
#include "platform/macos/macos_export_ports.hpp"
#include "platform/macos/macos_qt_analyzer_controller.hpp"
#include "test_support.hpp"
#include "ui/qt/analyzer_scope_plot.hpp"
#include "ui/qt/analyzer_window.hpp"
#include "ultra_hdr_metadata_test_support.hpp"
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMenu>
#include <QPointer>
#include <QThread>
#include <QTemporaryDir>
#include <QToolButton>
#include <zlib.h>
#include <cstring>

namespace {
using namespace hdrshot;
bool spin(const std::function<bool()> &predicate, int timeout = 12000) {
  QElapsedTimer time;
  time.start();
  while (!predicate() && time.elapsed() < timeout) {
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(2);
  }
  return predicate();
}
ExportSnapshot fixture(bool hdr = true) {
  auto desktop = std::make_shared<FrozenDesktop>();
  desktop->frame_id = {44};
  desktop->display_generation = 1;
  CanonicalFrameSegment s;
  s.display_id = {1};
  s.size_px = {256, 160};
  s.point_pixel_scale = 1;
  s.desktop_frame_points = {0, 0, 256, 160};
  desktop->desktop_bounds_points = s.desktop_frame_points;
  s.encoding = {ColorPrimaries::display_p3, TransferFunction::linear,
                AlphaMode::opaque, 0};
  s.pixel_format = PixelFormat::rgba32_float;
  s.display_dynamic_range = DisplayDynamicRange::hdr;
  s.rgba_float.resize(256U * 160U * 4U);
  for (std::size_t p = 0; p < 256U * 160U; ++p) {
    const float x = float(p % 256) / 255;
    s.rgba_float[p * 4] = .02F + x * (hdr ? 5.98F : .98F);
    s.rgba_float[p * 4 + 1] = .03F + x * (hdr ? 2.97F : .97F);
    s.rgba_float[p * 4 + 2] = .04F + x * (hdr ? 3.96F : .96F);
    s.rgba_float[p * 4 + 3] = 1;
  }
  desktop->canonical_segments.push_back(std::move(s));
  ExportSnapshot snapshot;
  snapshot.session_id = {22};
  snapshot.operation_id = {1};
  snapshot.frozen_desktop = desktop;
  snapshot.target_display_id = {1};
  snapshot.selection = {1, {16, 8, 224, 144}};
  snapshot.annotations.revision = 1;
  snapshot.annotation_render_plan = std::make_shared<AnnotationRenderPlan>(
      AnnotationRenderPlan{1, {224, 144}, {}, snapshot.selection.desktop_rect});
  return snapshot;
}
AnalyzerWindow *latest_window() {
  AnalyzerWindow *result = nullptr;
  for (auto *widget : QApplication::topLevelWidgets())
    if (auto *w = dynamic_cast<AnalyzerWindow *>(widget))
      result = w;
  return result;
}
std::uint32_t big(const std::vector<std::uint8_t> &bytes, std::size_t p) {
  HDRSHOT_CHECK(p + 4 <= bytes.size());
  return (std::uint32_t(bytes[p]) << 24) | (std::uint32_t(bytes[p + 1]) << 16) |
         (std::uint32_t(bytes[p + 2]) << 8) | bytes[p + 3];
}
void check_png(const PreparedExport &file, PixelSize size, bool hdr) {
  const auto &b = file.artifact.bytes;
  HDRSHOT_CHECK(b.size() > 32 && b[0] == 137);
  HDRSHOT_CHECK(big(b, 16) == std::uint32_t(size.width) &&
                big(b, 20) == std::uint32_t(size.height));
  bool cicp = false, clli = false, icc = false;
  for (std::size_t p = 8; p + 12 <= b.size();) {
    auto n = big(b, p);
    HDRSHOT_CHECK(n <= b.size() - p - 12);
    const std::string type(reinterpret_cast<const char *>(b.data() + p + 4), 4);
    HDRSHOT_CHECK(crc32(0, b.data() + p + 4, n + 4) == big(b, p + 8 + n));
    if (type == "cICP") {
      cicp = true;
      HDRSHOT_CHECK(n == 4 && b[p + 8] == 12 && b[p + 9] == (hdr ? 16 : 13));
    }
    clli |= type == "cLLI";
    icc |= type == "iCCP";
    p += n + 12;
  }
  HDRSHOT_CHECK(cicp && (hdr ? clli : icc));
}
void check_jpeg(PreparedExport &file, PixelSize size, bool hdr) {
  const auto &b = file.artifact.bytes;
  HDRSHOT_CHECK(b.size() > 4 && b[0] == 255 && b[1] == 216);
  if (!hdr)
    return;
  std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)>
      decoder(uhdr_create_decoder(), uhdr_release_decoder);
  uhdr_compressed_image_t image{};
  image.data = file.artifact.bytes.data();
  image.data_sz = b.size();
  image.capacity = b.size();
  HDRSHOT_CHECK(uhdr_dec_set_image(decoder.get(), &image).error_code ==
                UHDR_CODEC_OK);
  HDRSHOT_CHECK(uhdr_dec_probe(decoder.get()).error_code == UHDR_CODEC_OK);
  HDRSHOT_CHECK(uhdr_dec_get_image_width(decoder.get()) == size.width &&
                uhdr_dec_get_image_height(decoder.get()) == size.height);
  test::check_dual_metadata(decoder.get());
  test::check_p3_base_and_alternate(decoder.get());
}
void write_artifact(const PreparedExport &file, const QString &name) {
  const auto dir = qEnvironmentVariable("HDRSHOT_ANALYZER_INTEGRATION_DIR");
  if (dir.isEmpty())
    return;
  HDRSHOT_CHECK(QDir().mkpath(dir));
  QFile output(dir + "/" + name +
               QString::fromStdString(file.artifact.extension));
  HDRSHOT_CHECK(output.open(QIODevice::WriteOnly));
  HDRSHOT_CHECK(
      output.write(reinterpret_cast<const char *>(file.artifact.bytes.data()),
                   qint64(file.artifact.bytes.size())) ==
      qint64(file.artifact.bytes.size()));
}
void verify_native_destinations(const PreparedExport &prepared) {
  QTemporaryDir folder;
  HDRSHOT_CHECK(folder.isValid());
  struct BoardOwner {
    NSPasteboard *board = [NSPasteboard pasteboardWithUniqueName];
    ~BoardOwner() { [board releaseGlobally]; }
  } owner;
  HDRSHOT_CHECK(owner.board && owner.board.name);
  PosixFileStorePort files;
  MacClockPort clock;
  MacClipboardPort clipboard(folder.filePath("clipboard").toStdString(), owner.board.name.UTF8String);
  QElapsedTimer elapsed;
  elapsed.start();
  auto saved = ExportWorkflow::save_default_prepared(prepared, folder.path().toStdString(), clock, files);
  HDRSHOT_CHECK(saved.has_value());
  const auto save_ns = elapsed.nsecsElapsed();
  auto copied = ExportWorkflow::copy_prepared(prepared, clipboard);
  HDRSHOT_CHECK(copied.has_value());
  const auto copy_ns = elapsed.nsecsElapsed() - save_ns;
  QFile file(QString::fromStdString(std::get<FileReceipt>(saved.value().destination).exact_path));
  HDRSHOT_CHECK(file.open(QIODevice::ReadOnly));
  const auto data = file.readAll();
  HDRSHOT_CHECK(std::size_t(data.size()) == prepared.artifact.bytes.size());
  HDRSHOT_CHECK(std::memcmp(data.data(), prepared.artifact.bytes.data(), prepared.artifact.bytes.size()) == 0);
  NSData *published = [owner.board dataForType:prepared.artifact.mime_type == "image/png" ? NSPasteboardTypePNG : @"public.jpeg"];
  HDRSHOT_CHECK(published.length == prepared.artifact.bytes.size());
  HDRSHOT_CHECK(std::memcmp(published.bytes, data.data(), published.length) == 0);
  std::cout << "native destinations bytes=" << published.length << " save_ms=" << double(save_ns)/1e6
            << " copy_ms=" << double(copy_ns)/1e6 << " exact_bytes=true\n";
}
void report_and_original_use_real_codecs() {
  auto backend = make_macos_analysis_backend();
  HDRSHOT_CHECK(backend.has_value());
  auto processor = MacMetalExportPixelProcessor::create();
  HDRSHOT_CHECK(processor.has_value());
  LibUltraHdrEncoder encoder;
  for (bool hdr : {false, true}) {
    auto prepared = AnalysisWorkflow::prepare(
        fixture(hdr), *backend.value().port, processor.value().get());
    HDRSHOT_CHECK(prepared.has_value());
    analysis::ReportPlan plan;
    plan.revision = 1;
    plan.underlay.size = plan.overlay.size = {272, 192};
    plan.underlay.rgba.assign(272U * 192U * 4U, 24);
    plan.overlay.rgba.assign(272U * 192U * 4U, 0);
    for (std::size_t i = 3; i < plan.underlay.rgba.size(); i += 4)
      plan.underlay.rgba[i] = 255;
    plan.source_rect = {24, 24, 224, 144};
    plan.source_view.target_size = {224, 144};
    plan.source_view.scale = 1;
    plan.source_view.settings.working_space =
        hdr ? analysis::WorkingSpace::display_p3_pq
            : analysis::WorkingSpace::display_p3_sdr;
    auto composed =
        backend.value().port->compose_report(prepared.value().input, plan);
    HDRSHOT_CHECK(composed.has_value());
    auto report = AnalysisWorkflow::report_snapshot(prepared.value().original,
                                                    composed.value());
    HDRSHOT_CHECK(report.has_value());
    for (bool is_report : {false, true})
      for (auto format : {SaveFormat::png_display_p3_dual_range,
                          SaveFormat::ultra_hdr_jpeg}) {
        auto snapshot = is_report ? report.value() : prepared.value().original;
        snapshot.save_format = format;
        QElapsedTimer encoding;
        encoding.start();
        auto output = ExportWorkflow::prepare(
            snapshot, nullptr, processor.value().get(), processor.value().get(),
            &encoder, processor.value().get());
        HDRSHOT_CHECK(output.has_value());
        std::cout << (hdr ? "HDR " : "SDR ") << (is_report ? "report " : "original ")
                  << (format == SaveFormat::png_display_p3_dual_range ? "PNG" : "JPEG")
                  << " encode_ms=" << double(encoding.nsecsElapsed()) / 1e6 << '\n';
        const auto size = is_report ? PixelSize{272, 192} : PixelSize{224, 144};
        if (format == SaveFormat::png_display_p3_dual_range)
          check_png(output.value(), size, hdr);
        else
          check_jpeg(output.value(), size, hdr);
        write_artifact(output.value(), QString(hdr ? "hdr_" : "sdr_") +
                                           (is_report ? "report" : "original"));
        verify_native_destinations(output.value());
      }
  }
}
void production_controller_window_export_and_close() {
  SettingsSnapshot settings;
  settings.default_save_folder = "/fixture-not-written";
  std::vector<ExportSnapshot> exports;
  MacQtAnalyzerController controller(
      *qApp,
      [&](UiCommand, ExportSnapshot snapshot, auto done) {
        exports.push_back(std::move(snapshot));
        ExportReceipt receipt;
        receipt.destination = UserCancelled{};
        done(Result<ExportReceipt, Error>::success(receipt));
        return true;
      },
      [&] { return settings; },
      [&](std::string json) {
        settings.analyzer_preferences_json = std::move(json);
      },
      {});
  bool opened = false;
  controller.open(fixture(), [&](auto result) {
    HDRSHOT_CHECK(result.has_value() && result.value());
    opened = true;
  });
  HDRSHOT_CHECK(spin([&] { return opened; }));
  QPointer<AnalyzerWindow> window = latest_window();
  HDRSHOT_CHECK(window);
  window->resize(1280, 720);
  window->set_close_confirmation([] { return false; });
  HDRSHOT_CHECK(!controller.close_windows() && window && window->isVisible());
  auto *save = window->findChild<QToolButton *>("analyzerSave");
  auto action_named = [save](const QString &name) -> QAction * {
    for (auto *action : save->menu()->actions())
      if (action->text() == name)
        return action;
    return nullptr;
  };
  auto *save_original = action_named("保存原截图");
  auto *save_report = action_named("保存分析图");
  HDRSHOT_CHECK(save_original && save_report);
  save_original->trigger();
  HDRSHOT_CHECK(exports.size() == 1 && exports.back().selection.desktop_rect ==
                                           (PixelRect{0, 0, 224, 144}));
  HDRSHOT_CHECK(exports.back().default_folder == settings.default_save_folder);
  HDRSHOT_CHECK(save->text() == "保存原截图");
  // The last chosen action is remembered by the main button. Select the
  // semantic report action explicitly; menu order is not an output contract.
  // A request may still be pending; click report only after current data
  // appears.
  HDRSHOT_CHECK(spin(
      [&] {
        save_report->trigger();
        return exports.size() >= 2;
      },
      12000));
  HDRSHOT_CHECK(window && window->isVisible());
  HDRSHOT_CHECK(save->text() == "保存分析图");
  HDRSHOT_CHECK(exports.back().selection.desktop_rect.width > 224);
  auto processor = MacMetalExportPixelProcessor::create();
  HDRSHOT_CHECK(processor.has_value());
  LibUltraHdrEncoder encoder;
  auto report_file = ExportWorkflow::prepare(exports.back(), nullptr, processor.value().get(),
      processor.value().get(), &encoder, processor.value().get());
  HDRSHOT_CHECK(report_file.has_value());
  const auto report_rect = exports.back().selection.desktop_rect;
  check_png(report_file.value(), {report_rect.width, report_rect.height}, true);
  write_artifact(report_file.value(), "native_window_report");
  window->findChild<QComboBox *>("analyzerReferenceWhite")->setCurrentIndex(1);
  window->set_close_confirmation([&] {
    bool rejected = false;
    controller.open(fixture(false), [&](auto result) { rejected = !result; });
    HDRSHOT_CHECK(rejected);
    return true;
  });
  HDRSHOT_CHECK(controller.close_windows());
  QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  HDRSHOT_CHECK(window.isNull());
  HDRSHOT_CHECK(settings.analyzer_preferences_json.find("100") !=
                std::string::npos);
  controller.shutdown();
}
} // namespace
int main(int argc, char **argv) {
  @autoreleasepool {
    qputenv("QT_QPA_PLATFORM", "cocoa");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    if (!MTLCreateSystemDefaultDevice() || !NSScreen.screens.count)
      return 77;
    return test::run({{"actual PNG/JPEG report and original output",
                       report_and_original_use_real_codecs},
                      {"production bridge window/export/settings/close",
                       production_controller_window_export_and_close}});
  }
}
