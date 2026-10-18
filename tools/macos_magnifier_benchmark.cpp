// Synthetic, lock-screen-safe measurement. No capture/window/presentation
// claim.
#include "application/async_clean_content_cache.hpp"
#include "application/clean_content_cache.hpp"
#include "domain/frame/frame_pipeline.hpp"
#include "platform/macos/macos_gpu_source.hpp"
#include "platform/macos/macos_metal_export_pixel_processor.hpp"
#include "platform/macos/metal_edr_presenter.hpp"
#include "platform/qt/qt_export_task_executor.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
using namespace hdrshot;
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}
template <class T> T check(Result<T, Error> v) {
  if (!v) {
    std::cerr << v.error().module << ' ' << to_string(v.error().code) << '\n';
    std::exit(1);
  }
  return std::move(v.value());
}
void stats(std::ostream &out, const char *name, std::vector<double> v) {
  std::sort(v.begin(), v.end());
  out << '"' << name << "\":{\"n\":" << v.size()
      << ",\"p50_ms\":" << v[v.size() / 2]
      << ",\"p95_ms\":" << v[v.size() * 95 / 100] << ",\"max_ms\":" << v.back()
      << '}';
}
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  if (argc != 2)
    return 2;
  constexpr int w = 6144, h = 3456;
  NativeCaptureFrame input;
  input.display_id = DisplayId{1};
  input.size_px = {w, h};
  input.pixel_format = PixelFormat::rgba16_float;
  input.encoding = {ColorPrimaries::display_p3, TransferFunction::extended_srgb,
                    AlphaMode::opaque, 0};
  input.rgba_half.resize(std::size_t(w) * h * 4);
  for (std::size_t i = 0; i < input.rgba_half.size(); i += 4) {
    input.rgba_half[i] = 0x3800;
    input.rgba_half[i + 1] = 0x4000;
    input.rgba_half[i + 2] = 0x3c00;
    input.rgba_half[i + 3] = 0x3c00;
  }
  input.source_normalizer = macos_source_normalizer();
  auto segment = check(SourceColorInterpreter::interpret(
      std::move(input),
      {DisplayId{1}, {0, 0, w, h}, 1, {w, h}, DisplayDynamicRange::hdr}));
  auto frame = std::make_shared<const FrozenDesktop>(
      FrozenDesktop{FrameId{1}, 1, {0, 0, w, h}, {std::move(segment)}});
  auto native = frame->canonical_segments[0].linear_source;
  auto full = check(MacMetalEdrPresenter::create()),
       card = check(MacMetalEdrPresenter::create());
  MacMetalOverlayRequest request;
  request.width_px = w;
  request.height_px = h;
  request.source_is_linear = true;
  request.linear_source = native;
  request.source_frame_key = 1;
  request.magnifier = {true, {3000, 1700}, {0, 24, 440, 440}, 8, 1};
  MacMetalCardRequest local{
      request, {440, 680}, std::vector<std::uint8_t>(440 * 680 * 4)};
  std::vector<double> full_cpu, full_gpu, card_cpu, card_gpu;
  for (int i = 0; i < 70; ++i) {
    request.magnifier.center.x = 2800 + i;
    local.source = request;
    auto a = Clock::now();
    auto baseline = check(full->render_offscreen(request, true, false));
    auto ac = ms(a);
    auto b = Clock::now();
    auto candidate = check(card->render_card_offscreen(local, false));
    auto bc = ms(b);
    if (i >= 10) {
      full_cpu.push_back(ac);
      full_gpu.push_back(baseline.gpu_elapsed_ms);
      card_cpu.push_back(bc);
      card_gpu.push_back(candidate.gpu_elapsed_ms);
    }
  }
  auto gpu = std::shared_ptr<MacMetalExportPixelProcessor>(
      check(MacMetalExportPixelProcessor::create()));
  CleanContentCache cache(gpu);
  QtExportTaskExecutor executor(app);
  AsyncCleanContentCache async(executor, gpu);
  std::vector<double> cache_build, cache_hit, ready, async_submit,
      async_complete;
  for (int i = 0; i < 30; ++i) {
    AnnotationRenderPlan plan{
        static_cast<DocumentRevision>(i + 1), {w, h}, {}, {0, 0, w, h}};
    AnnotationCoverageLayer layer{ObjectId{1},
                                  AnnotationKind::rectangle,
                                  {300, 200, 5000, 2900},
                                  static_cast<std::uint32_t>(0xff6600 + i),
                                  {},
                                  "bench"};
    for (int y = 200; y < 3100; ++y) {
      if (y < 204 || y >= 3096)
        layer.spans.push_back({y, 300, std::vector<std::uint8_t>(5000, 192)});
      else {
        layer.spans.push_back({y, 300, std::vector<std::uint8_t>(4, 192)});
        layer.spans.push_back({y, 5296, std::vector<std::uint8_t>(4, 192)});
      }
    }
    plan.ordered_layers.push_back(std::move(layer));
    auto immutable =
        std::make_shared<const AnnotationRenderPlan>(std::move(plan));
    auto a = Clock::now();
    auto result = check(cache.get(frame, DisplayId{1}, immutable));
    auto ac = ms(a);
    auto b = Clock::now();
    check(cache.get(frame, DisplayId{1}, immutable));
    auto bc = ms(b);
    auto c = Clock::now();
    for (const auto &span : result->owned)
      if (span.native_samples) {
        check(span.native_samples->wait_until_ready());
        break;
      }
    auto cc = ms(c);
    bool completed = false;
    CleanContentRef async_result;
    auto d = Clock::now();
    async.preview(frame, DisplayId{1}, immutable, [&](auto value) {
      async_result = check(std::move(value));
      completed = true;
    });
    const auto dc = ms(d);
    while (!completed && ms(d) < 5000)
      QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    if (!completed)
      return 4;
    for (const auto &span : async_result->owned)
      if (span.native_samples) {
        check(span.native_samples->wait_until_ready());
        break;
      }
    const auto total = ms(d);
    // Both routes must produce the same sparse sample count and source
    // identity.
    if (async_result->owned.size() != result->owned.size() ||
        async_result->source != frame)
      return 5;
    if (i >= 5) {
      cache_build.push_back(ac);
      cache_hit.push_back(bc);
      ready.push_back(cc);
      async_submit.push_back(dc);
      async_complete.push_back(total);
    }
  }
  std::ofstream out(argv[1]);
  out << "{\"scope\":\"synthetic 6144x3456 native Linear P3; full vs 440x680 "
         "card; no visible latency\",";
  stats(out, "full_cpu", full_cpu);
  out << ',';
  stats(out, "full_gpu", full_gpu);
  out << ',';
  stats(out, "card_cpu", card_cpu);
  out << ',';
  stats(out, "card_gpu", card_gpu);
  out << ',';
  stats(out, "clean_cache_build", cache_build);
  out << ',';
  stats(out, "clean_cache_hit", cache_hit);
  out << ',';
  stats(out, "clean_gpu_ready", ready);
  out << ',';
  stats(out, "async_gui_submit", async_submit);
  out << ',';
  stats(out, "async_total_complete", async_complete);
  out << "}\n";
  return out ? 0 : 3;
}
