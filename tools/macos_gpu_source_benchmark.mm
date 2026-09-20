#include "application/clean_content_cache.hpp"
#include "domain/frame/frame_pipeline.hpp"
#include "platform/macos/macos_gpu_source.hpp"
#include "platform/macos/macos_linear_storage.hpp"
#include "platform/macos/macos_metal_export_pixel_processor.hpp"
#include "platform/macos/metal_edr_presenter.hpp"
#import <Foundation/Foundation.h>
#import <mach/mach.h>
#include <sys/resource.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>

using namespace hdrshot;
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::time_point start) {
  return std::chrono::duration<double,std::milli>(Clock::now()-start).count();
}
std::uint64_t footprint() {
  task_vm_info_data_t info{}; mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count);
  return info.phys_footprint;
}
template<class T> T checked(Result<T,Error> value) {
  if (!value) {
    std::cerr << value.error().module << ':' << to_string(value.error().code);
    for (const auto& [k,v] : value.error().safe_context) std::cerr << ' ' << k << '=' << v;
    std::cerr << '\n'; std::exit(1);
  }
  return std::move(value.value());
}
double median(std::vector<double> values) {
  std::sort(values.begin(),values.end()); return values[values.size()/2];
}

int main(int argc,char** argv) { @autoreleasepool {
  if (argc != 4) { std::cerr << "usage: cpu|native 1|2 empty|aa\n"; return 2; }
  const std::string mode=argv[1]; const int displays=std::atoi(argv[2]);
  const bool native=mode=="native", aa=std::string(argv[3])=="aa";
  if ((mode!="native"&&mode!="cpu") || displays<1 || displays>2 ||
      (std::string(argv[3])!="empty" && !aa)) return 2;
  constexpr int width=6144,height=3456;
  const auto setup=Clock::now();
  auto creation=MacMetalExportPixelProcessor::create();
  if (!creation) { std::cerr << "Metal unavailable\n"; return 77; }
  auto gpu=std::shared_ptr<MacMetalExportPixelProcessor>(std::move(creation.value()));
  auto normalizer=macos_source_normalizer();
  std::vector<std::unique_ptr<MacMetalEdrPresenter>> presenters;
  std::vector<NativeCaptureFrame> inputs;
  std::vector<AnnotationRenderPlan> plans;
  for (int d=0;d<displays;++d) {
    presenters.push_back(checked(MacMetalEdrPresenter::create()));
    NativeCaptureFrame input{DisplayId{static_cast<std::uint64_t>(d+1)},{width,height},PixelFormat::rgba16_float,
        {ColorPrimaries::display_p3,TransferFunction::extended_srgb,AlphaMode::opaque,0},{}};
    input.rgba_half.resize(static_cast<std::size_t>(width)*height*4);
    for (std::size_t i=0;i<input.rgba_half.size();i+=4) {
      input.rgba_half[i]=static_cast<std::uint16_t>(0x3000+(i%4096));
      input.rgba_half[i+1]=0x3c00; input.rgba_half[i+2]=0x4000; input.rgba_half[i+3]=0x3c00;
    }
    if (native) input.source_normalizer=normalizer; else input.linear_storage=macos_linear_storage();
    inputs.push_back(std::move(input));
    AnnotationRenderPlan plan{1,{width,height},{},{0,0,width,height}};
    if (aa) {
      AnnotationCoverageLayer layer{ObjectId{1},AnnotationKind::rectangle,{500,400,1000,600},0xff6600,{},"bench"};
      for (int y=400;y<1000;++y) {
        if (y<404||y>=996) layer.spans.push_back({y,500,std::vector<std::uint8_t>(1000,192)});
        else { layer.spans.push_back({y,500,std::vector<std::uint8_t>(4,192)});
          layer.spans.push_back({y,1496,std::vector<std::uint8_t>(4,192)}); }
      }
      plan.ordered_layers.push_back(std::move(layer));
    }
    plans.push_back(std::move(plan));
  }
  const double setup_ms=milliseconds(setup);
  const auto before=footprint();
  std::atomic<bool> sampling{true};
  std::atomic<std::uint64_t> peak{before};
  std::thread sampler([&] {
    while (sampling.load(std::memory_order_relaxed)) {
      const auto value=footprint();
      auto previous=peak.load(std::memory_order_relaxed);
      while (value>previous && !peak.compare_exchange_weak(previous,value,std::memory_order_relaxed)) {}
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  std::vector<FrozenDesktopRef> sources;
  std::vector<std::unique_ptr<CleanContentCache>> caches;
  std::vector<MacMetalOverlayRequest> requests;
  const auto first_begin=Clock::now();
  double source_submit_ms=0,clean_submit_ms=0,first_gpu_ms=0;
  for (int d=0;d<displays;++d) { @autoreleasepool {
    auto begin=Clock::now(); const auto id=inputs[d].display_id;
    auto segment=checked(SourceColorInterpreter::interpret(std::move(inputs[d]),
        {id,{double(d*width),0,width,height},1,{width,height},DisplayDynamicRange::hdr}));
    source_submit_ms+=milliseconds(begin);
    FrozenDesktop desktop{FrameId{static_cast<std::uint64_t>(d+1)},1,{0,0,width,height},{}};
    desktop.canonical_segments.push_back(std::move(segment));
    sources.push_back(std::make_shared<const FrozenDesktop>(std::move(desktop)));
    caches.push_back(std::make_unique<CleanContentCache>(gpu));
    begin=Clock::now(); auto clean=checked(caches[d]->get(sources[d],id,plans[d]));
    clean_submit_ms+=milliseconds(begin);
    const auto& frame=sources[d]->canonical_segments[0];
    requests.push_back({width,height,nullptr,{300,200,5000,2900},.35F,2.03F,2,MacMetalSurfaceRange::edr,
        static_cast<std::uint64_t>(d+1),true,
        native ? nullptr : std::shared_ptr<const LinearFloatPixels>(sources[d],&frame.rgba_float),
        clean,frame.linear_source});
  }}
  const auto submitted=footprint();
  for (int d=0;d<displays;++d) { @autoreleasepool {
    first_gpu_ms+=checked(presenters[d]->render_offscreen(requests[d],true,false)).gpu_elapsed_ms;
  }}
  const double first_ms=milliseconds(first_begin);
  const auto first_memory=footprint();
  const auto first_peak=std::max(peak.load(),first_memory);
  std::vector<double> redraw_cpu,redraw_gpu,edit_cpu,edit_gpu;
  for (int i=0;i<70;++i) { @autoreleasepool {
    auto begin=Clock::now(); double measured_gpu=0;
    for (int d=0;d<displays;++d) {
      requests[d].selection_px.x=300+i;
      measured_gpu+=checked(presenters[d]->render_offscreen(requests[d],true,false)).gpu_elapsed_ms;
    }
    if (i>=20) {redraw_cpu.push_back(milliseconds(begin));redraw_gpu.push_back(measured_gpu);}
  }}
  const auto steady=footprint();
  if (aa) for (int i=0;i<40;++i) { @autoreleasepool {
    auto begin=Clock::now(); double measured_gpu=0;
    for (int d=0;d<displays;++d) {
      plans[d].source_document_revision++;
      plans[d].ordered_layers[0].color_srgb_rgb=(i%2) ? 0xff6600 : 0x00aaff;
      requests[d].clean_content=checked(caches[d]->get(sources[d],DisplayId{static_cast<std::uint64_t>(d+1)},plans[d]));
      measured_gpu+=checked(presenters[d]->render_offscreen(requests[d],true,false)).gpu_elapsed_ms;
    }
    if (i>=10) {edit_cpu.push_back(milliseconds(begin));edit_gpu.push_back(measured_gpu);}
  }}
  double range_ms=0,png_pixels_ms=0,jpeg_pixels_ms=0;
  { @autoreleasepool {
    const PixelRect rect{300,200,5000,2900};
    auto roi=checked(FrameCropper::view_display(*sources[0],DisplayId{1},{1,rect}));
    auto plan=checked(requests[0].clean_content->roi_plan(rect,1));
    auto begin=Clock::now(); auto range=checked(gpu->probe(roi,plan,{})); range_ms=milliseconds(begin);
    auto output_plan=checked(OutputClassifier::classify({range}));
    begin=Clock::now(); auto png=checked(gpu->process({&roi,&plan,output_plan,PqDiffuseWhite::nits_203,HdrPqPrecision::bits_10}));
    png_pixels_ms=milliseconds(begin);
    // Release final output before exercising the independent JPEG route.
    png.rgb_u16.clear(); png.rgb_u16.shrink_to_fit();
    begin=Clock::now(); auto jpeg=checked(gpu->render({&roi,&plan,203})); jpeg_pixels_ms=milliseconds(begin);
  }}
  rusage usage{};getrusage(RUSAGE_SELF,&usage);
  sampling=false;sampler.join();
  std::uint64_t passes=0; for (const auto& source:sources) passes+=source->canonical_segments[0].software_linearization_passes;
  std::cout << "{\"mode\":\""<<mode<<"\",\"displays\":"<<displays<<",\"aa\":"<<aa
      <<",\"setup_ms\":"<<setup_ms<<",\"source_submit_ms\":"<<source_submit_ms
      <<",\"clean_submit_ms\":"<<clean_submit_ms<<",\"first_ms\":"<<first_ms<<",\"first_gpu_ms\":"<<first_gpu_ms
      <<",\"redraw_cpu_median_ms\":"<<median(redraw_cpu)<<",\"redraw_gpu_median_ms\":"<<median(redraw_gpu)
      <<",\"edit_cpu_median_ms\":"<<(aa?median(edit_cpu):0)<<",\"edit_gpu_median_ms\":"<<(aa?median(edit_gpu):0)
      <<",\"range_ms\":"<<range_ms<<",\"png_pixels_ms\":"<<png_pixels_ms<<",\"jpeg_pixels_ms\":"<<jpeg_pixels_ms
      <<",\"before_bytes\":"<<before<<",\"submitted_bytes\":"<<submitted<<",\"first_bytes\":"<<first_memory
      <<",\"steady_bytes\":"<<steady<<",\"first_peak_footprint_bytes\":"<<first_peak
      <<",\"lifetime_peak_footprint_bytes\":"<<peak.load()<<",\"peak_rss_bytes\":"<<usage.ru_maxrss
      <<",\"linearization_passes\":"<<passes<<"}\n";
  return 0;
}}
