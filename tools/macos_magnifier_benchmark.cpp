// Synthetic, lock-screen-safe measurement. No capture/window/presentation
// claim.
#include "application/annotation_edit_service.hpp"
#include "application/async_clean_content_cache.hpp"
#include "application/clean_content_cache.hpp"
#include "domain/annotation/brush_stroke_builder.hpp"
#include "domain/frame/frame_pipeline.hpp"
#include "platform/macos/macos_gpu_source.hpp"
#include "platform/macos/macos_metal_export_pixel_processor.hpp"
#include "platform/macos/metal_edr_presenter.hpp"
#include "platform/qt/qt_export_task_executor.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <charconv>
#include <string_view>
#include <cstdlib>
#include <set>
#include <fstream>
#include <iostream>
#include <mach/mach.h>
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
  if(v.empty()) { out << '\"' << name << "\":{\"n\":0,\"raw_ms\":[]}";return; }
  const auto raw=v;
  std::sort(v.begin(), v.end());
  out << '"' << name << "\":{\"n\":" << v.size()
      << ",\"p50_ms\":" << v[v.size() / 2]
      << ",\"p95_ms\":" << v[v.size() * 95 / 100] << ",\"max_ms\":" << v.back()
      << ",\"raw_ms\":[";
  for(std::size_t i=0;i<raw.size();++i) { if(i) out<<',';out<<raw[i]; }
  out << "]}";
}
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  if (argc != 2 && argc != 5)
    return 2;
  const int w = argc==5?std::stoi(argv[2]):6144, h=argc==5?std::stoi(argv[3]):3456;
  const double brush_scale=argc==5?std::stod(argv[4]):2.;
  if(w<320 || h<240 || !std::isfinite(brush_scale)||brush_scale<=0) return 2;
  const bool clip_only=std::getenv("HDRSHOT_BRUSH_CLIP_ONLY")!=nullptr;
  const bool extended_edits=clip_only || std::getenv("HDRSHOT_BRUSH_EXTENDED_EDITS")!=nullptr;
  const auto setup_start=Clock::now();
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
      {DisplayId{1}, {0, 0, double(w), double(h)}, 1, {w, h}, DisplayDynamicRange::hdr}));
  auto frame = std::make_shared<const FrozenDesktop>(
      FrozenDesktop{FrameId{1}, 1, {0, 0, double(w), double(h)}, {std::move(segment)}});
  auto native = frame->canonical_segments[0].linear_source;
  check(native->wait_until_ready());
  const auto source_setup_ms=ms(setup_start);
  auto full = check(MacMetalEdrPresenter::create()),
       card = check(MacMetalEdrPresenter::create());
  MacMetalOverlayRequest request;
  request.width_px = w;
  request.height_px = h;
  request.source_is_linear = true;
  request.linear_source = native;
  request.source_frame_key = 1;
  request.magnifier = {true, {3000, 1700}, {0, 24, 440, 440}, 8, 1};
  if(argc==5) request.magnifier={}; // brush-only canvas/scale matrix
  MacMetalCardRequest local{
      request, {440, 680}, std::vector<std::uint8_t>(440 * 680 * 4)};
  std::vector<double> full_cpu, full_gpu, card_cpu, card_gpu;
  for (int i = 0; i < (argc==2?70:0); ++i) {
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
  for (int i = 0; i < (argc==2?30:0); ++i) {
    AnnotationRenderPlan plan{static_cast<DocumentRevision>(i + 1),
                              {w, h},
                              {},
                              {0, 0, w, h},
                              AnnotationScale::from(1).value()};
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
  out << "{\"scope\":\"synthetic native Linear P3; default full vs 440x680 "
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
  // Production builder -> shared transaction -> native clean -> Metal render.
  // This measures an offscreen chain and never labels GUI enqueue time as FPS.
#ifdef HDRSHOT_BRUSH_BENCHMARK_ADMISSION
  const std::vector<int> diameters{2,4,24,72,144,192};
#else
  const std::vector<int> diameters{2,4,24,72};
#endif
  std::vector<int> requested_widths;
  if(const auto* filter=std::getenv("HDRSHOT_BRUSH_WIDTHS")) {
    std::string_view remaining(filter);
    if(remaining.empty()) return 2;
    while(true) {
      const auto comma=remaining.find(',');const auto token=remaining.substr(0,comma);
      int width=0;const auto parsed=std::from_chars(token.data(),token.data()+token.size(),width);
      if(parsed.ec!=std::errc{} || parsed.ptr!=token.data()+token.size() ||
         std::find(diameters.begin(),diameters.end(),width)==diameters.end() ||
         std::find(requested_widths.begin(),requested_widths.end(),width)!=requested_widths.end()) return 2;
      requested_widths.push_back(width);
      if(comma==std::string_view::npos) break;
      remaining.remove_prefix(comma+1);if(remaining.empty()) return 2;
    }
  }
  out << ",\"source_setup_ms\":" << source_setup_ms << ",\"extended_edits\":" << extended_edits << ",\"brush_scale\":" << brush_scale << ",\"brush_width\":" << w << ",\"brush_height\":" << h;
  for (int diameter : diameters) {
    if(!requested_widths.empty() && std::find(requested_widths.begin(),requested_widths.end(),diameter)==requested_widths.end()) continue;
    if(extended_edits && diameter>72) continue;
    auto scale = check(AnnotationScale::from(brush_scale));
    AnnotationEditContext context{{1, {0, 0, w, h}}, scale};
    auto document = AnnotationDocument::empty();
    auto committed = check(prepare_selection_change(
        document, nullptr, context.selection, context));
    std::vector<double> brush_prepare, brush_clean, brush_gpu, brush_release, brush_total, move_ms, resize_ms, undo_ms, redo_ms, fractional_move_ms, recolor_ms, diameter_ms, clip_expand_ms, nochange_ms;
    std::vector<double> pixel_plan, sample_prepare, compose_submit, native_wait,
        full_rebuild;
    std::size_t max_samples = 0, max_growths = 0, max_owned=0, max_upload_bytes=0, max_gpu_metadata_bytes=0;
    std::size_t clip_added_min=std::numeric_limits<std::size_t>::max(),clip_exposed_frames=0;
    for (int stroke = 0; stroke < 3; ++stroke) {
      BrushStrokeBuilder builder;
      StrokePoint first{w*.08, h*(.15+stroke*.08)};
      const std::uint32_t stroke_colors[]{0xFF4D67,0x36A3FF,0x25C06D};
      check(builder.begin({std::uint64_t(stroke + 100)},
                          {extended_edits?stroke_colors[stroke]:0xFF4D67, std::uint16_t(diameter)}, first,
                          context.selection.desktop_rect, scale));
      StrokePoint final = first;
      std::vector<StrokePoint> trajectory{first};
      for (int frame_index = 0; frame_index < 24; ++frame_index) {
        std::vector<StrokePoint> points;
        for (int sample = 0; sample < 8; ++sample) {
          double t = (frame_index * 8 + sample) / 191.;
          final = {w*(.5+.35*std::sin(t*8)),h*(.3+stroke*.07+.17*std::sin(t*19))};
          points.push_back(final);
        }
        trajectory.insert(trajectory.end(),points.begin(),points.end());
        const auto total_start=Clock::now();
        auto start = total_start;
        check(builder.append(points));
        auto prefix = check(builder.snapshot());
        auto plan = check(prepare_annotation_preview(
            committed.document, committed.plan.get(), prefix, context));
        brush_prepare.push_back(ms(start));
        start = Clock::now();
        auto clean = check(cache.get(frame, {1}, plan));
        const auto wait_start = Clock::now();
        for (const auto &span : clean->owned)
          if (span.native_samples) {
            check(span.native_samples->wait_until_ready());
            break;
          }
        native_wait.push_back(ms(wait_start));
        brush_clean.push_back(ms(start));
        const auto counters = cache.counters();
        pixel_plan.push_back(counters.pixel_plan_ms);
        sample_prepare.push_back(counters.sample_prepare_ms);
        compose_submit.push_back(counters.compose_submit_ms);
        max_samples = std::max(max_samples, counters.last_sample_count);
        std::size_t owned=0;std::set<std::uint32_t> palette;
        for(const auto& span:clean->owned) { owned+=std::size_t(span.length);if(!span.native_samples && span.edge_samples.empty()) palette.insert(span.color_srgb_rgb); }
        max_owned=std::max(max_owned,owned);
        max_upload_bytes=std::max(max_upload_bytes,counters.last_sample_count*sizeof(PositionedCleanSample));
        max_gpu_metadata_bytes=std::max(max_gpu_metadata_bytes,clean->owned.size()*16+std::size_t(h)*8+std::max(std::size_t(1),palette.size())*16);
        max_growths = std::max(max_growths, counters.sample_vector_growths);
        request.clean_content = clean;
        auto rendered = check(full->render_offscreen(request, true, false));
        brush_gpu.push_back(rendered.gpu_elapsed_ms);
        brush_total.push_back(ms(total_start));
        if(builder.counters().segments_processed==0 || clean->owned.empty()) return 4;
      }
      auto start = Clock::now();
      auto completed = check(builder.finish(final));
      auto reuse = completed.reusable;
      committed = check(prepare_annotation_edit(
          committed.document, committed.plan.get(),
          CreateAnnotation{completed.object}, context, &reuse));
      if(!extended_edits) brush_release.push_back(ms(start));
      start = Clock::now();
      check(AnnotationRenderPlanner::rasterize_shape(completed.object, {w, h},
                                                     scale));
      full_rebuild.push_back(ms(start));
      if(extended_edits) {
        const auto settle=[&](const PreparedAnnotationEdit& edit) {
          auto clean=check(cache.get(frame,{1},edit.plan));request.clean_content=clean;
          check(full->render_offscreen(request,true,false));
          return clean;
        };
        const auto edit=[&](const AnnotationCommand& command,std::vector<double>& values) {
          auto begin=Clock::now();
          committed=check(prepare_annotation_edit(committed.document,committed.plan.get(),command,context));
          settle(committed);values.push_back(ms(begin));
        };
        for(int repeat=0;repeat<10;++repeat) {
          if(!clip_only) {
          BrushStrokeBuilder releasing;
          check(releasing.begin({std::uint64_t(1000+repeat)},std::get<BrushStyle>(completed.object.style),trajectory.front(),context.selection.desktop_rect,scale));
          check(releasing.append(std::span(trajectory).subspan(1)));
          auto release_start=Clock::now();
          auto released=check(releasing.finish(trajectory.back()));
          auto released_edit=check(prepare_annotation_edit(AnnotationDocument::empty(),nullptr,CreateAnnotation{released.object},context,&released.reusable));
          settle(released_edit);
          brush_release.push_back(ms(release_start));
          auto geometry=*completed.object.brush_geometry;geometry.origin_y=repeat%2?5.:10.;
          edit(SetBrushGeometry{completed.object.id,geometry},move_ms);
          geometry.origin_x=.25+repeat*.01;
          edit(SetBrushGeometry{completed.object.id,geometry},fractional_move_ms);
          geometry.scale_y=.95+repeat*.001;
          edit(SetBrushGeometry{completed.object.id,geometry},resize_ms);
          edit(RestyleAnnotation{completed.object.id,BrushStyle{stroke_colors[(stroke+repeat+1)%3],std::uint16_t(diameter)}},recolor_ms);
          edit(RestyleAnnotation{completed.object.id,BrushStyle{stroke_colors[(stroke+repeat+1)%3],std::uint16_t(repeat%2?24:72)}},diameter_ms);
          edit(UndoAnnotation{},undo_ms);edit(RedoAnnotation{},redo_ms);
          auto begin=Clock::now();auto previous=check(cache.get(frame,{1},committed.plan));
          auto same=check(cache.get(frame,{1},committed.plan));if(previous!=same) return 6;
          settle(committed);nochange_ms.push_back(ms(begin));
          }
          // Centerline bounds retain every input point but necessarily clip
          // nonzero-radius ink. Expanding must expose actual new coverage.
          AnnotationEditContext clipped{{1,completed.object.bounds},scale};
          auto narrow=check(prepare_annotation_edit(AnnotationDocument::empty(),nullptr,CreateAnnotation{completed.object},clipped));
          const auto narrow_clean=settle(narrow);
          const auto begin=Clock::now();
          auto expanded=check(prepare_selection_change(narrow.document,narrow.plan.get(),context.selection,context));
          const auto expanded_clean=settle(expanded);clip_expand_ms.push_back(ms(begin));
          const auto owned_count=[](const auto& clean) { std::size_t n=0;for(const auto& span:clean->owned)n+=std::size_t(span.length);return n; };
          const auto before=owned_count(narrow_clean),after=owned_count(expanded_clean);
          if(after<=before) { std::cerr<<"ROI expansion exposed no ink"<<'\n';return 7; }
          clip_added_min=std::min(clip_added_min,after-before);++clip_exposed_frames;
        }
      } else {
      start=Clock::now();
      auto geometry = *completed.object.brush_geometry;
      geometry.origin_y += 10;
      committed = check(prepare_annotation_edit(
          committed.document, committed.plan.get(),
          SetBrushGeometry{completed.object.id, geometry}, context));
      move_ms.push_back(ms(start));start=Clock::now();
      geometry.scale_y = .95;
      committed = check(prepare_annotation_edit(
          committed.document, committed.plan.get(),
          SetBrushGeometry{completed.object.id, geometry}, context));
      resize_ms.push_back(ms(start));start=Clock::now();
      committed = check(prepare_annotation_edit(
          committed.document, committed.plan.get(), UndoAnnotation{}, context));
      undo_ms.push_back(ms(start));start=Clock::now();
      committed = check(prepare_annotation_edit(
          committed.document, committed.plan.get(), RedoAnnotation{}, context));
      redo_ms.push_back(ms(start));
      }
    }
    const auto prefix = "brush_" + std::to_string(diameter) + "_";
    out << ',';
    stats(out, (prefix + "prepare").c_str(), brush_prepare);
    out << ',';
    stats(out, (prefix + "clean_ready").c_str(), brush_clean);
    out << ',';
    stats(out, (prefix + "metal_gpu").c_str(), brush_gpu);
    out << ',';
    stats(out, (prefix + "release").c_str(), brush_release);
    for (const auto &metric : {std::pair{"total_gpu_complete", &brush_total},
                               {"move", &move_ms},{"resize", &resize_ms},{"undo", &undo_ms},{"redo", &redo_ms},
                               {"fractional_move", &fractional_move_ms},{"recolor", &recolor_ms},{"diameter", &diameter_ms},
                               {"clip_expand", &clip_expand_ms},{"nochange", &nochange_ms},
                               {"pixel_plan", &pixel_plan},
                               {"sample_prepare", &sample_prepare},
                               {"compose_submit", &compose_submit},
                               {"native_wait", &native_wait},
                               {"full_rebuild", &full_rebuild}}) {
      out << ',';
      stats(out, (prefix + metric.first).c_str(), *metric.second);
    }
    out << ",\"" << prefix << "max_samples\":" << max_samples << ",\"" << prefix
        << "max_vector_growths\":" << max_growths
        << ",\"" << prefix << "max_owned_pixels\":" << max_owned
        << ",\"" << prefix << "effective_work_upload_bytes\":" << max_upload_bytes
        << ",\"" << prefix << "gpu_metadata_bytes\":" << max_gpu_metadata_bytes
        << ",\"" << prefix << "native_sample_gpu_bytes\":" << max_samples*16
        << ",\"" << prefix << "clip_exposed_frames\":" << clip_exposed_frames
        << ",\"" << prefix << "clip_added_owned_min\":" << (clip_exposed_frames?clip_added_min:0);
  }
  mach_task_basic_info_data_t memory{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&memory), &count) == KERN_SUCCESS)
    out << ",\"resident_bytes_end\":" << memory.resident_size;
  out << "}\n";
  return out ? 0 : 3;
}
