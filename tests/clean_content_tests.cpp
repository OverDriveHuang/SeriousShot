#include "application/clean_content_cache.hpp"
#include "domain/frame/frame_pipeline.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "domain/output/source_range_probe.hpp"
#include "domain/output/output_row_producer.hpp"
#include "domain/output/ultra_hdr_input_renderer.hpp"
#include "test_support.hpp"
#include <future>
#ifdef __APPLE__
#include "platform/macos/macos_metal_export_pixel_processor.hpp"
#include "platform/macos/macos_linear_storage.hpp"
#include "platform/macos/metal_edr_presenter.hpp"
#endif
using namespace hdrshot;
namespace {
FrozenDesktopRef fixture() {
  NativeCaptureFrame native{DisplayId{1}, {8, 4}, PixelFormat::rgba16_float,
      {ColorPrimaries::display_p3, TransferFunction::extended_srgb, AlphaMode::opaque, 0}, {}};
  for (int i = 0; i < 32; ++i) native.rgba_half.insert(native.rgba_half.end(), {0x3c00, 0x4000, 0x3800, 0});
  #ifdef __APPLE__
  native.linear_storage = macos_linear_storage();
  #endif
  auto segment = SourceColorInterpreter::interpret(std::move(native),
      {DisplayId{1}, {0,0,8,4}, 1, {8,4}, DisplayDynamicRange::hdr});
  HDRSHOT_CHECK(segment.has_value());
  FrozenDesktop desktop{FrameId{1},1,{0,0,8,4},{}};
  desktop.canonical_segments.push_back(std::move(segment.value()));
  return std::make_shared<const FrozenDesktop>(std::move(desktop));
}
AnnotationRenderPlan plan() {
  return {1, {6, 2}, {{ObjectId{1}, AnnotationKind::text, {1,0,4,1}, 0x00ff00,
      {{0,1,{64,128,192,255}}}, "text"}}, {1,1,6,2}};
}
void source_precision_and_roi() {
  const auto source = fixture();
  const auto& segment = source->canonical_segments[0];
  HDRSHOT_CHECK(segment.rgba_half.empty());
  HDRSHOT_CHECK(segment.rgba_float.size() == 128);
  HDRSHOT_CHECK(segment.pixel_format == PixelFormat::rgba32_float);
  HDRSHOT_CHECK(segment.encoding.transfer == TransferFunction::linear);
  HDRSHOT_CHECK(segment.rgba_float[3] == 1);
  HDRSHOT_CHECK_NEAR(segment.rgba_float[1], ExtendedP3Mapper::inverse_extended_srgb(2), 0);
  auto view = FrameCropper::view_display(*source, DisplayId{1}, {1,{1,1,6,2}});
  HDRSHOT_CHECK(view.has_value());
  HDRSHOT_CHECK(view.value().rgba_float.data() == segment.rgba_float.data());
  HDRSHOT_CHECK(view.value().first_sample_offset == 36);
  auto copy = FrameCropper::crop_display(*source, DisplayId{1}, {1,{1,1,6,2}});
  HDRSHOT_CHECK(copy && copy.value().rgba_float.size() == 48 && copy.value().rgba_half.empty());
}
void exhaustive_fp16_source_and_linear_input() {
  NativeCaptureFrame input{DisplayId{1},{0x4201,1},PixelFormat::rgba16_float,
      {ColorPrimaries::display_p3,TransferFunction::extended_srgb,AlphaMode::opaque,0},{}};
  for(unsigned i=0;i<=0x4200;++i) input.rgba_half.insert(input.rgba_half.end(),
      {static_cast<std::uint16_t>(i),static_cast<std::uint16_t>(i),static_cast<std::uint16_t>(i),0});
  const DisplaySnapshot display{DisplayId{1},{0,0,0x4201,1},1,{0x4201,1},DisplayDynamicRange::hdr};
  auto linear = SourceColorInterpreter::interpret(std::move(input),display);
  HDRSHOT_CHECK(linear.has_value());
  int maximum_sdr_code_delta=0; double maximum_half_linear_delta=0;
  for(unsigned i=0;i<=0x4200;++i) {
    const auto encoded=ExtendedP3Mapper::decode_binary16(static_cast<std::uint16_t>(i)).value();
    const auto value=linear.value().rgba_float[i*4];
    HDRSHOT_CHECK(value==ExtendedP3Mapper::inverse_extended_srgb(encoded));
    const auto half=ExtendedP3Mapper::decode_binary16(ExtendedP3Mapper::encode_binary16(value)).value();
    maximum_half_linear_delta=std::max(maximum_half_linear_delta,std::abs(double(value-half)));
    if(i<=0x3c00) maximum_sdr_code_delta=std::max(maximum_sdr_code_delta,std::abs(
        int(ExtendedP3Mapper::quantize_unorm16(encoded))-
        int(ExtendedP3Mapper::quantize_unorm16(ExtendedP3Mapper::encode_extended_srgb(value)))));
  }
  HDRSHOT_CHECK(maximum_sdr_code_delta<=1);
  std::cout<<"SDR roundtrip max RGB16 codes="<<maximum_sdr_code_delta
           <<" hypothetical linear half max EDR delta="<<maximum_half_linear_delta<<'\n';
  NativeCaptureFrame windows{DisplayId{1},{1,1},PixelFormat::rgba16_float,
      {ColorPrimaries::display_p3,TransferFunction::linear,AlphaMode::opaque,0},{0x3c00,0x4000,0xbc00,0x3c00}};
  const auto* pointer=windows.rgba_half.data();
  auto retained=SourceColorInterpreter::interpret(std::move(windows),{DisplayId{1},{0,0,1,1},1,{1,1},DisplayDynamicRange::hdr});
  HDRSHOT_CHECK(retained && retained.value().rgba_half.data()==pointer);
  HDRSHOT_CHECK(retained.value().rgba_float.empty() && retained.value().software_linearization_passes==0);
}
void multisegment_and_detached_export_lifetime() {
  auto one=fixture(); FrozenDesktop desktop=*one;
  auto two=desktop.canonical_segments[0];two.display_id=DisplayId{2};
  two.rgba_float.assign(two.rgba_float.size(),0.25F);desktop.canonical_segments.push_back(std::move(two));
  auto source=std::make_shared<const FrozenDesktop>(std::move(desktop));
  std::weak_ptr<const FrozenDesktop> lifetime=source;
  CleanContentRef detached;
  {
    CleanContentCache cache;
    auto first=cache.get(source,DisplayId{1},plan());auto second=cache.get(source,DisplayId{2},plan());
    HDRSHOT_CHECK(first && second && first.value()->owned!=second.value()->owned);
    detached=first.value();
    HDRSHOT_CHECK(!cache.get(source,DisplayId{99},plan()));
  }
  source.reset();HDRSHOT_CHECK(!lifetime.expired());
  auto worker=std::async(std::launch::async,[detached] { return detached->roi_plan({1,1,6,2},100); });
  detached.reset();auto result=worker.get();HDRSHOT_CHECK(result.has_value());
  // std::future's callable has released its last source owner after get().
  HDRSHOT_CHECK(lifetime.expired());
}

void reuse_selection_revisions_and_lifetime() {
  auto source = fixture(); auto render = plan();
  CleanContentCache cache;
  auto first = cache.get(source, DisplayId{1}, render); HDRSHOT_CHECK(first.has_value());
  auto again = cache.get(source, DisplayId{1}, render); HDRSHOT_CHECK(again.value() == first.value());
  auto larger = AnnotationRenderPlanner::rebase(render, {0,0,8,4}); HDRSHOT_CHECK(larger.has_value());
  larger.value().source_document_revision = 9; // UI object selection revision
  auto selected = cache.get(source, DisplayId{1}, larger.value()); HDRSHOT_CHECK(selected.value() == first.value());
  HDRSHOT_CHECK(cache.counters().content_builds == 1);
  HDRSHOT_CHECK(cache.counters().composed_pixels == 4);
  HDRSHOT_CHECK(cache.counters().cache_hits == 2);
  render.ordered_layers[0].color_srgb_rgb = 0xff0000; // same document revision, transient content differs
  auto changed = cache.get(source, DisplayId{1}, render); HDRSHOT_CHECK(changed && changed.value() != first.value());
  HDRSHOT_CHECK(changed.value()->content_revision == 2);
  render.ordered_layers.clear(); ++render.source_document_revision;
  auto removed = cache.get(source, DisplayId{1}, render); HDRSHOT_CHECK(removed && removed.value()->owned.empty());
  auto restored = cache.get(source, DisplayId{1}, plan()); HDRSHOT_CHECK(restored.has_value());
  HDRSHOT_CHECK(restored.value()->owned == first.value()->owned);
  std::weak_ptr<const FrozenDesktop> lifetime = source; source.reset();
  HDRSHOT_CHECK(!lifetime.expired()); // old exports own their source after session caller release
  HDRSHOT_CHECK(first.value()->owned != changed.value()->owned);
}
void ownership_and_output_match_reference() {
  auto source = fixture(); const auto render = plan(); CleanContentCache cache;
  auto content = cache.get(source, DisplayId{1}, render); HDRSHOT_CHECK(content.has_value());
  auto clean = content.value()->roi_plan(render.source_selection_rect_px, render.source_document_revision);
  auto original = AnnotationRenderPlanner::build_pixel_plan(render);
  auto view = FrameCropper::view_display(*source, DisplayId{1}, {1,render.source_selection_rect_px});
  HDRSHOT_CHECK(clean && original && view);
  HDRSHOT_CHECK(clean.value().source_visible_spans == original.value().source_visible_spans);
  for (const auto& span : clean.value().annotation_owned_spans)
    for (const auto& sample : span.edge_samples) HDRSHOT_CHECK(sample[3] == 0);
  auto fit = SourceRangeProbe::probe(view.value(), clean.value()); HDRSHOT_CHECK(fit && !fit.value().fits_sdr);
  for (auto intent : {EncodingIntent::wide_gamut_sdr, EncodingIntent::hdr_pq})
    for (auto precision : {HdrPqPrecision::bits_10, HdrPqPrecision::bits_12, HdrPqPrecision::bits_16})
      for (double white : {100.,203.}) {
        const OutputPlan output{intent == EncodingIntent::hdr_pq ? OutputClass::hdr : OutputClass::wide_gamut_sdr, intent,16,"test"};
        auto a = OutputRowProducer::produce_all(view.value(), original.value(), output, white, precision);
        auto b = OutputRowProducer::produce_all(view.value(), clean.value(), output, white, precision);
        HDRSHOT_CHECK(a && b && a.value().rgb_u16 == b.value().rgb_u16);
      }
  CpuUltraHdrInputRenderer renderer;
  auto a = renderer.render({&view.value(), &original.value(), kUltraHdrReferenceWhiteNits});
  auto b = renderer.render({&view.value(), &clean.value(), kUltraHdrReferenceWhiteNits});
  HDRSHOT_CHECK(a && b && a.value().rgba_half == b.value().rgba_half);
  HDRSHOT_CHECK(a.value().source_visible_maximum_linear_component == b.value().source_visible_maximum_linear_component);
}
void owned_hdr_edges_remain_excluded_from_classification() {
  FrozenDesktop desktop=*fixture();
  auto& data=desktop.canonical_segments[0].rgba_float;std::fill(data.begin(),data.end(),0.25F);
  for (int x=2;x<6;++x) for (int c=0;c<3;++c) data[static_cast<std::size_t>((8+x)*4+c)]=8.0F;
  auto source=std::make_shared<const FrozenDesktop>(std::move(desktop));CleanContentCache cache;
  auto content=cache.get(source,DisplayId{1},plan());HDRSHOT_CHECK(content.has_value());
  auto pixels=content.value()->roi_plan({1,1,6,2},1);
  auto view=FrameCropper::view_display(*source,DisplayId{1},{1,{1,1,6,2}});
  auto fit=SourceRangeProbe::probe(view.value(),pixels.value());HDRSHOT_CHECK(fit && fit.value().fits_sdr);
  HDRSHOT_CHECK(content.value()->owned[0].edge_samples[0][0]>1.0F);
  HDRSHOT_CHECK(!content.value()->roi_plan({-1,0,6,2},1));
}
struct Failure : CleanCompositionPort {
  bool fail{},malformed{}; CpuCleanComposition cpu;
  Result<std::vector<std::array<float,4>>, Error> compose(std::span<const CleanCompositionSample> s) override {
    if (fail) return Result<std::vector<std::array<float,4>>, Error>::failure(
      {ErrorCode::invalid_input,"test",Retryability::never,{}});
    auto result=cpu.compose(s);
    if(malformed && result && !result.value().empty())result.value()[0][3]=0.5F;
    return result;
  }
};
void failed_revision_does_not_publish() {
  auto port = std::make_shared<Failure>(); CleanContentCache cache(port); auto source = fixture(); auto render = plan();
  auto first = cache.get(source, DisplayId{1}, render); HDRSHOT_CHECK(first.has_value());
  port->fail = true; render.ordered_layers[0].color_srgb_rgb = 0;
  HDRSHOT_CHECK(!cache.get(source, DisplayId{1}, render));
  HDRSHOT_CHECK(cache.counters().content_builds == 1);
  port->fail=false;port->malformed=true;
  HDRSHOT_CHECK(!cache.get(source,DisplayId{1},render));
  HDRSHOT_CHECK(cache.counters().content_builds == 1);
  HDRSHOT_CHECK(cache.get(source, DisplayId{1}, plan()).value() == first.value());
}
#ifdef __APPLE__
void metal_clean_and_preview() {
  auto native = MacMetalExportPixelProcessor::create(); HDRSHOT_CHECK(native.has_value());
  auto gpu = std::shared_ptr<MacMetalExportPixelProcessor>(std::move(native.value()));
  auto source = fixture(); const auto render = plan(); CleanContentCache cpu, metal(gpu);
  auto a = cpu.get(source, DisplayId{1}, render); auto b = metal.get(source, DisplayId{1}, render); HDRSHOT_CHECK(a && b);
  for (std::size_t j=0;j<a.value()->owned.size();++j)
    for (std::size_t i=0;i<a.value()->owned[j].edge_samples.size();++i)
      for (std::size_t c=0;c<4;++c) HDRSHOT_CHECK_NEAR(a.value()->owned[j].edge_samples[i][c], b.value()->owned[j].edge_samples[i][c], 1e-6);
  auto presenter = MacMetalEdrPresenter::create(); HDRSHOT_CHECK(presenter.has_value());
  MacMetalOverlayRequest request{8,4,nullptr,{0,0,8,4},1,1,0,MacMetalSurfaceRange::edr,1,true,
      std::shared_ptr<const LinearFloatPixels>(source, &source->canonical_segments[0].rgba_float),b.value()};
  auto shown = presenter.value()->render_offscreen(request, true); HDRSHOT_CHECK(shown.has_value());
  HDRSHOT_CHECK(shown.value().source_aliases_cpu);
  for (const auto& span : b.value()->owned) for (int i=0;i<span.length;++i) for (std::size_t c=0;c<3;++c) {
    const auto expected = ExtendedP3Mapper::decode_binary16(ExtendedP3Mapper::encode_binary16(span.edge_samples[static_cast<std::size_t>(i)][c]));
    HDRSHOT_CHECK_NEAR(shown.value().rgba_linear_display_p3[(static_cast<std::size_t>(span.y)*8+static_cast<std::size_t>(span.x+i))*4+c], expected.value(), 0.002);
  }
  request.selection_px={2,1,4,2};request.ui_border_width_px=1;request.outside_linear_dim_factor=0.35F;
  const auto compute=presenter.value()->render_offscreen(request,false);
  const auto fused=presenter.value()->render_offscreen(request,true);
  HDRSHOT_CHECK(compute && fused && compute.value().rgba_linear_display_p3==fused.value().rgba_linear_display_p3);
  auto pixels = b.value()->roi_plan(render.source_selection_rect_px,1);
  auto view = FrameCropper::view_display(*source,DisplayId{1},{1,render.source_selection_rect_px});
  OutputPlan sdr{OutputClass::wide_gamut_sdr,EncodingIntent::wide_gamut_sdr,16,"test"};
  auto sdr_cpu=OutputRowProducer::produce_all(view.value(),pixels.value(),sdr,100.,HdrPqPrecision::bits_16);
  auto sdr_gpu=gpu->process({&view.value(),&pixels.value(),sdr,PqDiffuseWhite::nits_100,HdrPqPrecision::bits_16});
  HDRSHOT_CHECK(sdr_cpu && sdr_gpu);
  for(std::size_t i=0;i<sdr_cpu.value().rgb_u16.size();++i)
    HDRSHOT_CHECK(std::abs(int(sdr_cpu.value().rgb_u16[i])-int(sdr_gpu.value().rgb_u16[i]))<=1);
  CpuUltraHdrInputRenderer jpeg_cpu;
  auto jpeg_reference=jpeg_cpu.render({&view.value(),&pixels.value(),kUltraHdrReferenceWhiteNits});
  auto jpeg_actual=gpu->render({&view.value(),&pixels.value(),kUltraHdrReferenceWhiteNits});
  HDRSHOT_CHECK(jpeg_reference && jpeg_actual);
  for(std::size_t i=0;i<jpeg_reference.value().rgba_half.size();++i)
    HDRSHOT_CHECK(std::abs(int(jpeg_reference.value().rgba_half[i])-int(jpeg_actual.value().rgba_half[i]))<=1);
  HDRSHOT_CHECK(jpeg_reference.value().source_visible_maximum_linear_component.has_value() &&
                jpeg_actual.value().source_visible_maximum_linear_component.has_value());
  HDRSHOT_CHECK_NEAR(*jpeg_reference.value().source_visible_maximum_linear_component,
                    *jpeg_actual.value().source_visible_maximum_linear_component,1e-6);
  for (auto precision : {HdrPqPrecision::bits_10, HdrPqPrecision::bits_12, HdrPqPrecision::bits_16})
    for (auto white : {PqDiffuseWhite::nits_100,PqDiffuseWhite::nits_203}) {
      OutputPlan output{OutputClass::hdr,EncodingIntent::hdr_pq,16,"test"};
      auto reference = OutputRowProducer::produce_all(view.value(),pixels.value(),output,pq_diffuse_white_nits(white),precision);
      auto actual = gpu->process({&view.value(),&pixels.value(),output,white,precision}); HDRSHOT_CHECK(reference && actual);
      // GPU pow and CPU reference may straddle an effective PQ code boundary.
      const int step = precision == HdrPqPrecision::bits_10 ? 65 : precision == HdrPqPrecision::bits_12 ? 17 : 2;
      for (std::size_t i=0;i<reference.value().rgb_u16.size();++i)
        HDRSHOT_CHECK(std::abs(int(reference.value().rgb_u16[i])-int(actual.value().rgb_u16[i])) <= step);
    }
}
#endif
}
int main() {
  std::vector<test::TestCase> cases{{"FP32 source ownership and ROI",source_precision_and_roi},
    { "exhaustive source precision and existing linear bypass",exhaustive_fp16_source_and_linear_input},
    {"multi-display detached export lifetime",multisegment_and_detached_export_lifetime},
    {"content cache selection revision undo lifetime",reuse_selection_revisions_and_lifetime},
    {"ownership and SDR HDR JPEG reference",ownership_and_output_match_reference},
    {"owned HDR AA remains excluded from classification",owned_hdr_edges_remain_excluded_from_classification},
    {"failure does not publish",failed_revision_does_not_publish}};
#ifdef __APPLE__
  if (MacMetalExportPixelProcessor::create()) cases.push_back({"actual Metal clean preview output",metal_clean_and_preview});
  else std::cout << "Metal unavailable: GPU case not run\n";
#endif
  return test::run(cases);
}
