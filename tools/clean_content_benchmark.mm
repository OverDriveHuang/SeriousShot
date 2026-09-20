#include "application/clean_content_cache.hpp"
#include "platform/macos/macos_linear_storage.hpp"
#include "domain/frame/frame_pipeline.hpp"
#include "platform/macos/macos_metal_export_pixel_processor.hpp"
#include "platform/macos/metal_edr_presenter.hpp"
#import <Foundation/Foundation.h>
#import <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/resource.h>
#include <chrono>
#include <iostream>
#include <numeric>
#include <cstdlib>
#ifdef HDRSHOT_T21_BASELINE
#include "legacy_metal.hpp"
#endif
using namespace hdrshot;
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point a) { return std::chrono::duration<double,std::milli>(Clock::now()-a).count(); }
std::uint64_t footprint() {
  task_vm_info_data_t info{}; mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count);
  return info.phys_footprint;
}
int main(int argc, char** argv) { @autoreleasepool {
  const int displays = argc>1 ? std::atoi(argv[1]) : 1;
  const bool baseline = argc>2 && std::string(argv[2]) == "legacy";
  if(displays<1 || displays>2 || argc>3 || (argc>2 && !baseline && std::string(argv[2])!="current")) {
    std::cerr<<"Usage: benchmark [1|2] [current|legacy]\n";return 2;
  }
#ifndef HDRSHOT_T21_BASELINE
  if(baseline) { std::cerr<<"Legacy adapter not configured\n";return 2; }
#endif
  constexpr int width=6144, height=3456;
  auto gpu_result = MacMetalExportPixelProcessor::create();
  if (!gpu_result) { std::cerr<<"Metal unavailable\n"; return 77; }
  auto gpu = std::shared_ptr<MacMetalExportPixelProcessor>(std::move(gpu_result.value()));
  std::vector<FrozenDesktopRef> sources;
  std::vector<std::unique_ptr<MacMetalEdrPresenter>> presenters;
  std::vector<std::unique_ptr<CleanContentCache>> caches;
  std::vector<MacMetalOverlayRequest> requests;
  double normalization_ms=0, first_content_ms=0, first_preview_ms=0, repeated_ms=0, gpu_ms=0;
  double repeated_content_ms=0,repeated_roi_ms=0,uncached_plan_ms=0;
  std::uint64_t conversion_peak=0, aliased_frames=0;
#ifdef HDRSHOT_T21_BASELINE
  std::vector<std::unique_ptr<legacy::MacMetalEdrPresenter>> old_presenters;
  std::vector<legacy::MacMetalOverlayRequest> old_requests;
#endif
  for (int d=0; d<displays; ++d) {
    NativeCaptureFrame input{DisplayId{static_cast<std::uint64_t>(d+1)}, {width,height}, PixelFormat::rgba16_float,
        {ColorPrimaries::display_p3,TransferFunction::extended_srgb,AlphaMode::opaque,0}, {}};
    input.rgba_half.resize(static_cast<std::size_t>(width)*height*4);
    for (std::size_t i=0;i<input.rgba_half.size();i+=4) {
      input.rgba_half[i]=static_cast<std::uint16_t>(0x3000+(i%4096));
      input.rgba_half[i+1]=0x3c00;input.rgba_half[i+2]=0x4000;input.rgba_half[i+3]=0x3c00;
    }
#ifdef HDRSHOT_T21_BASELINE
    if (baseline) {
      auto old = legacy::MacMetalEdrPresenter::create(); if(!old) return 2;
      old_presenters.push_back(std::move(old.value()));
      old_requests.push_back({width,height,std::make_shared<const std::vector<std::uint16_t>>(std::move(input.rgba_half)),
          {300,200,5000,2900},.35F,2.03F,2,legacy::MacMetalSurfaceRange::edr,static_cast<std::uint64_t>(d+1),false});
      continue;
    }
#endif
    input.linear_storage=macos_linear_storage();
    auto begin=Clock::now();
    auto linear=SourceColorInterpreter::interpret(std::move(input),{DisplayId{static_cast<std::uint64_t>(d+1)},
        {double(d*width),0,width,height},1,{width,height},DisplayDynamicRange::hdr});
    normalization_ms+=ms(begin); if(!linear) return 3;
    FrozenDesktop desktop{FrameId{static_cast<std::uint64_t>(d+1)},1,{0,0,width,height},{}};
    desktop.canonical_segments.push_back(std::move(linear.value()));
    sources.push_back(std::make_shared<const FrozenDesktop>(std::move(desktop)));
    conversion_peak=std::max(conversion_peak,footprint());
    const auto& storage=sources.back()->canonical_segments[0].rgba_float;
    mach_vm_address_t region=reinterpret_cast<std::uintptr_t>(storage.data());
    mach_vm_size_t region_size=0;vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t object=MACH_PORT_NULL;
    const auto vm_status=mach_vm_region(mach_task_self(),&region,&region_size,VM_REGION_BASIC_INFO_64,
        reinterpret_cast<vm_region_info_t>(&info),&count,&object);
    if(object!=MACH_PORT_NULL)mach_port_deallocate(mach_task_self(),object);
    std::cerr<<"source capacity="<<storage.capacity()*4<<" alignmentRemainder="
      <<reinterpret_cast<std::uintptr_t>(storage.data())%65536<<" regionBytes="<<region_size
      <<" regionOffset="<<(reinterpret_cast<std::uintptr_t>(storage.data())-region)<<" vmStatus="<<vm_status<<'\n';

    auto presenter=MacMetalEdrPresenter::create(); if(!presenter) return 4;
    presenters.push_back(std::move(presenter.value())); caches.push_back(std::make_unique<CleanContentCache>(gpu));
    AnnotationRenderPlan render{1,{width,height},{},{0,0,width,height}};
    AnnotationCoverageLayer layer{ObjectId{1},AnnotationKind::rectangle,{500,400,1000,600},0xff6600,{},"bench"};
    for(int y=400;y<1000;++y) {
      if(y<404||y>=996) layer.spans.push_back({y,500,std::vector<std::uint8_t>(1000,192)});
      else {layer.spans.push_back({y,500,std::vector<std::uint8_t>(4,192)});layer.spans.push_back({y,1496,std::vector<std::uint8_t>(4,192)});}
    }
    render.ordered_layers.push_back(std::move(layer));
    auto immutable_render=std::make_shared<const AnnotationRenderPlan>(render);
    begin=Clock::now(); auto clean=caches.back()->get(sources.back(),DisplayId{static_cast<std::uint64_t>(d+1)},immutable_render);
    first_content_ms+=ms(begin); if(!clean) return 5;
    requests.push_back({width,height,nullptr,{300,200,5000,2900},.35F,2.03F,2,MacMetalSurfaceRange::edr,
        static_cast<std::uint64_t>(d+1),true,
        std::shared_ptr<const LinearFloatPixels>(sources.back(),&sources.back()->canonical_segments[0].rgba_float),clean.value()});
    // Repeated selection/save queries must not composite again.
    for(int i=0;i<20;++i) {
      begin=Clock::now();auto hit=caches.back()->get(sources.back(),DisplayId{static_cast<std::uint64_t>(d+1)},immutable_render);
      repeated_content_ms+=ms(begin);if(!hit)return 6;
      begin=Clock::now();auto roi=hit.value()->roi_plan({0,0,width,height},1);
      repeated_roi_ms+=ms(begin);if(!roi)return 6;
      begin=Clock::now();auto uncached=AnnotationRenderPlanner::build_pixel_plan(render);
      uncached_plan_ms+=ms(begin);if(!uncached)return 6;
    }
  }
  for(int iteration=0;iteration<221;++iteration) {
    const auto begin=Clock::now();
    for(int d=0;d<displays;++d) { @autoreleasepool {
#ifdef HDRSHOT_T21_BASELINE
      if (baseline) {
        // Local baseline adapter retains T20 compute+render shaders and uses an offscreen target.
        old_requests[static_cast<std::size_t>(d)].selection_px.x=300+iteration;
        auto result=old_presenters[static_cast<std::size_t>(d)]->render_offscreen(old_requests[static_cast<std::size_t>(d)]);
        if(!result)return 7;
        if(iteration>100)gpu_ms+=result.value().gpu_elapsed_ms;
        continue;
      }
#endif
      requests[static_cast<std::size_t>(d)].selection_px.x=300+iteration;
      auto result=presenters[static_cast<std::size_t>(d)]->render_offscreen(requests[static_cast<std::size_t>(d)],true,false);
      if(!result)return 8;
      if(iteration>100)gpu_ms+=result.value().gpu_elapsed_ms;
      if(iteration==0 && result.value().source_aliases_cpu) ++aliased_frames;
    }}
    if(iteration==0)first_preview_ms=ms(begin); else if(iteration>100) repeated_ms+=ms(begin);
  }
  rusage usage{};getrusage(RUSAGE_SELF,&usage);
  std::uint64_t builds=0,hits=0,pixels=0,passes=0;std::size_t clean_bytes=0;
  for(const auto& cache:caches){auto c=cache->counters();builds+=c.content_builds;hits+=c.cache_hits;pixels+=c.composed_pixels;clean_bytes+=c.retained_sample_bytes;}
  for(const auto& source:sources)passes+=source->canonical_segments[0].software_linearization_passes;
  std::cout<<"{\"displays\":"<<displays<<",\"legacy\":"<<(baseline?"true":"false")
    <<",\"normalization_ms\":"<<normalization_ms<<",\"first_content_ms\":"<<first_content_ms
    <<",\"first_preview_ms\":"<<first_preview_ms<<",\"repeat_wall_ms\":"<<repeated_ms/120
    <<",\"repeat_gpu_ms\":"<<gpu_ms/120<<",\"stable_footprint_bytes\":"<<footprint()
    <<",\"peak_rss_bytes\":"<<usage.ru_maxrss<<",\"aliased_frames\":"<<aliased_frames<<",\"normalization_passes\":"<<passes
    <<",\"content_builds\":"<<builds<<",\"cache_hits\":"<<hits<<",\"composed_pixels\":"<<pixels
    <<",\"repeat_content_ms\":"<<repeated_content_ms/20<<",\"repeat_roi_ms\":"<<repeated_roi_ms/20
    <<",\"uncached_pixel_plan_ms\":"<<uncached_plan_ms/20<<",\"clean_bytes\":"<<clean_bytes<<"}\n";
  return 0;
}}
