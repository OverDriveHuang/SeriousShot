#include "platform/windows/windows_d3d_presenter.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "application/clean_content_cache.hpp"
#include "platform/windows/windows_icc_transform.hpp"
#include "platform/windows/windows_gpu_source.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <array>
#include <future>
#include <limits>
#include <mutex>
#define NOMINMAX
#include <windows.h>
using namespace hdrshot;
namespace {
PresentPreviewRequest request() {
  auto frame=std::make_shared<FrozenDesktop>();
  frame->frame_id=FrameId{1};frame->display_generation=7;
  CanonicalFrameSegment segment;
  segment.display_id=DisplayId{1};segment.size_px={8,4};
  segment.encoding=WindowsColor::linear_p3_encoding();
  for(int y=0;y<4;++y) for(int x=0;x<8;++x) {
    const float values[]{0.25F,1,2,4,-0.125F,0.5F,1,4};
    const auto h=ExtendedP3Mapper::encode_binary16(values[x]);
    segment.rgba_half.insert(segment.rgba_half.end(),{h,h,h,0x3c00});
  }
  frame->canonical_segments.push_back(std::move(segment));
  PresentPreviewRequest out;
  out.operation_id=OperationId{1};out.target_display_id=DisplayId{1};
  out.model.session_id=SessionId{2};out.model.frozen_desktop=frame;
  out.model.display_generation=7;out.model.selection={3,{2,1,4,2}};
  out.model.overlay_style.selection_border_width_px=0;
  return out;
}
std::shared_ptr<WindowsD3DPreviewPresenter> presenter(WindowsSourceWhite white={false,80}) {
  WindowsDisplayInfo display;display.snapshot.id=DisplayId{1};display.snapshot.capture_size_px={8,4};
  display.source_white=white;display.advanced_color_mode=white.hdr_active?2:1;
  auto result=WindowsD3DPreviewPresenter::create(nullptr,display);
  HDRSHOT_CHECK(result.has_value());return result.value();
}
void linear_mask_and_extended_values() {
  auto p=presenter();const auto result=p->render_offscreen(request());
  HDRSHOT_CHECK(result.has_value());
  auto at=[&](int x,int y) {return ExtendedP3Mapper::decode_binary16(result.value()[(y*8+x)*4]).value();};
  HDRSHOT_CHECK_NEAR(at(3,1),4,0.003);
  HDRSHOT_CHECK_NEAR(at(7,1),1.4,0.003);
  HDRSHOT_CHECK_NEAR(at(1,1),0.35,0.002);
  HDRSHOT_CHECK_NEAR(at(4,1),-0.125,0.001);
}
void white_and_primary_matrix() {
  auto p=presenter({true,203});auto r=request();
  auto frame=std::make_shared<FrozenDesktop>(*r.model.frozen_desktop);
  frame->canonical_segments[0].rgba_half[ (1*8+3)*4+1 ]=0;
  frame->canonical_segments[0].rgba_half[ (1*8+3)*4+2 ]=0;
  r.model.frozen_desktop=frame;
  const auto result=p->render_offscreen(r);HDRSHOT_CHECK(result.has_value());
  const auto expected=WindowsColor::linear_p3_to_scrgb({4,0,0},1);
  for(int c=0;c<3;++c) HDRSHOT_CHECK_NEAR(
      ExtendedP3Mapper::decode_binary16(result.value()[(1*8+3)*4+c]).value(),expected[c]*203/80,0.015);
}
void frame_contract_and_cancellation() {
  auto p=presenter();auto r=request();r.model.display_generation=8;
  HDRSHOT_CHECK(!p->render_offscreen(r));
  r=request();r.target_display_id=DisplayId{9};HDRSHOT_CHECK(!p->render_offscreen(r));
  r=request();p->cancel(r.model.session_id,r.operation_id);
  std::promise<Result<PresentReceipt,Error>> done;auto future=done.get_future();
  p->present(r,[&](auto result){done.set_value(std::move(result));});
  HDRSHOT_CHECK(future.wait_for(std::chrono::seconds(2))==std::future_status::ready);
  const auto result=future.get();HDRSHOT_CHECK(!result && result.error().code==ErrorCode::operation_cancelled);
  r.operation_id=OperationId{2};std::promise<Result<PresentReceipt,Error>> live;auto ready=live.get_future();
  p->present(r,[&](auto result){live.set_value(std::move(result));});
  HDRSHOT_CHECK(ready.wait_for(std::chrono::seconds(2))==std::future_status::ready);
  HDRSHOT_CHECK(ready.get().has_value());
}
void selection_border_matches_mac_geometry() {
  for(const auto white:{WindowsSourceWhite{false,80},WindowsSourceWhite{true,203}}) {
    auto p=presenter(white);auto r=request();r.model.overlay_style.selection_border_width_px=1;
    const auto result=p->render_offscreen(r);HDRSHOT_CHECK(result.has_value());
    const auto at=[&](int x,int y) {return ExtendedP3Mapper::decode_binary16(result.value()[(y*8+x)*4]).value();};
    const auto ui=white.hdr_active ? 2.03*203/80:1.0;
    HDRSHOT_CHECK_NEAR(at(1,0),ui,0.01); // one pixel outside the selection
    HDRSHOT_CHECK_NEAR(at(2,1),ui,0.01); // one pixel inside the selection
    HDRSHOT_CHECK_NEAR(at(6,3),ui,0.01);
    HDRSHOT_CHECK_NEAR(at(0,0),0.25*0.35*(white.hdr_active?203.0/80:1),0.002);
    r.model.selection.desktop_rect={};
    const auto empty=p->render_offscreen(r);HDRSHOT_CHECK(empty.has_value());
    HDRSHOT_CHECK_NEAR(ExtendedP3Mapper::decode_binary16(empty.value()[4]).value(),
        0.35*(white.hdr_active?203.0/80:1),0.002);
  }
}
void first_frame_commits_while_window_hidden() {
  const auto hwnd=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP,L"STATIC",L"SeriousShot hidden frame test",
      WS_POPUP,0,0,8,4,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
  HDRSHOT_CHECK(hwnd!=nullptr);
  {
    WindowsDisplayInfo display;display.snapshot.id=DisplayId{1};display.snapshot.capture_size_px={8,4};
    display.source_white={false,80};display.advanced_color_mode=1;
    auto p=WindowsD3DPreviewPresenter::create(hwnd,display);HDRSHOT_CHECK(p.has_value());
    std::promise<Result<PresentReceipt,Error>> done;auto ready=done.get_future();
    p.value()->present(request(),[&](auto result){done.set_value(std::move(result));});
    HDRSHOT_CHECK(ready.wait_for(std::chrono::seconds(3))==std::future_status::ready);
    HDRSHOT_CHECK(ready.get().has_value());
    HDRSHOT_CHECK(!IsWindowVisible(hwnd));
  }
  DestroyWindow(hwnd);
}

constexpr PixelSize magnifier_size{128,112};
using Rgb = std::array<double,3>;
// Nonuniform, exactly half-representable values: every adjacent texel differs
// in at least one channel. Includes negatives and values above SDR white.
Rgb fixture_pixel(int x,int y) {
  return {(x%19-3)/8.0,(y%17+1)/8.0,((x+3*y)%23)/8.0};
}
WindowsDisplayInfo magnifier_display(std::uint32_t mode) {
  WindowsDisplayInfo d;
  d.snapshot.id=DisplayId{1}; d.snapshot.capture_size_px=magnifier_size;
  d.source_white={mode==2,203}; d.advanced_color_mode=mode;
  if(mode==0) {
    WindowsIccProfile::Curve curve;
    curve.type=0; curve.parameters[0]=2.2F;
    // A channel permutation makes bypassing the Legacy ICC matrix visible.
    const std::array<float,9> rotate{0,1,0, 0,0,1, 1,0,0};
    const std::array<float,9> undo{0,0,1, 1,0,0, 0,1,0};
    d.icc_profile=std::make_shared<const WindowsIccProfile>(
        "synthetic-magnifier-rotated-gamma22",std::array{curve,curve,curve},rotate,undo);
  }
  return d;
}
PresentPreviewRequest magnifier_request() {
  auto r=request();
  auto frame=std::make_shared<FrozenDesktop>(*r.model.frozen_desktop);
  auto& segment=frame->canonical_segments.front();
  segment.size_px=magnifier_size; segment.rgba_half.clear();
  for(int y=0;y<magnifier_size.height;++y) for(int x=0;x<magnifier_size.width;++x) {
    for(double v:fixture_pixel(x,y)) segment.rgba_half.push_back(ExtendedP3Mapper::encode_binary16(static_cast<float>(v)));
    segment.rgba_half.push_back(0x3c00);
  }
  r.model.frozen_desktop=frame;
  r.model.selection.desktop_rect={};
  r.model.overlay_style.outside_linear_dim_factor=0.25F;
  return r;
}
Rgb display_oracle(Rgb p3,std::uint32_t mode) {
  if(mode==0) {
    Rgb device{p3[2],p3[0],p3[1]};
    for(auto& v:device) {
      const double code=std::pow(std::clamp(v,0.0,1.0),1.0/2.2);
      v=code<=0.04045 ? code/12.92:std::pow((code+0.055)/1.055,2.4);
    }
    return device;
  }
  // FP64 oracle from P3/sRGB xy primaries and D65, independent of the
  // product's WindowsColor helpers and display-transform reference method.
  constexpr double matrix[3][3]{
      {1.2249401762805598,-0.2249401762805598,0},
      {-0.0420569547096882,1.0420569547096882,0},
      {-0.0196375545903344,-0.0786360455506319,1.0982736001409663}};
  Rgb out{};
  for(int c=0;c<3;++c) for(int j=0;j<3;++j) out[c]+=matrix[c][j]*p3[j];
  if(mode==2) for(auto& v:out) v*=203.0/80;
  return out;
}
void check_pixel(const std::vector<std::uint16_t>& actual,int x,int y,Rgb expected) {
  const auto offset=static_cast<std::size_t>(y*magnifier_size.width+x)*4U;
  for(int c=0;c<3;++c) {
    const double value=ExtendedP3Mapper::decode_binary16(actual[offset+c]).value();
    HDRSHOT_CHECK(std::isfinite(value));
    // D3D11.3 functional spec 3.2.2 requires round-to-zero for float32 ->
    // float16 render-target stores, allowing almost one full half ULP.
    // https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
    // Add 16 FP32 epsilons for the matrix/scale/TRC arithmetic before storage;
    // this also covers a value just below an exactly representable half.
    int exponent=0;std::frexp(std::abs(expected[c]),&exponent);
    const double half_ulp=expected[c]==0 ? std::ldexp(1.0,-24):
        std::max(std::ldexp(1.0,-24),std::ldexp(1.0,exponent-11));
    const double float_error=16*std::numeric_limits<float>::epsilon()*std::max(1.0,std::abs(expected[c]));
    HDRSHOT_CHECK_NEAR(value,expected[c],half_ulp+float_error);
  }
  HDRSHOT_CHECK(actual[offset+3]==0x3c00);
}
void magnifier_all_pixels_modes_zoom_and_corners() {
  for(std::uint32_t mode:{0U,1U,2U}) {
    auto created=WindowsD3DPreviewPresenter::create(nullptr,magnifier_display(mode));
    HDRSHOT_CHECK(created.has_value()); const auto p=created.value();
    auto r=magnifier_request();
    const auto original=r.model.frozen_desktop->canonical_segments.front().rgba_half;
    for(int zoom:{4,8,12,16}) for(PixelPoint center:{PixelPoint{63,61},
        PixelPoint{0,0},PixelPoint{127,0},PixelPoint{0,111},PixelPoint{127,111}}) {
      r.model.magnifier={true,center,{7,11,7*zoom,5*zoom},zoom,1};
      const auto output=p->render_offscreen(r); HDRSHOT_CHECK(output.has_value());
      // Construct a complete reference picture by copying each source texel
      // into a zoom x zoom tile, then paint the independent marker geometry.
      std::vector<Rgb> expected(static_cast<std::size_t>(magnifier_size.width)*magnifier_size.height);
      auto put=[&](int x,int y,Rgb v) {expected[static_cast<std::size_t>(y*magnifier_size.width+x)]=v;};
      for(int y=0;y<magnifier_size.height;++y) for(int x=0;x<magnifier_size.width;++x) {
        auto rgb=fixture_pixel(x,y); for(auto& v:rgb) v*=0.25; put(x,y,rgb);
      }
      for(int row=-2;row<=2;++row) for(int col=-3;col<=3;++col) {
        const int sx=center.x+col,sy=center.y+row;
        const auto rgb=sx>=0 && sx<128 && sy>=0 && sy<112 ? fixture_pixel(sx,sy):Rgb{.025,.025,.025};
        for(int dy=0;dy<zoom;++dy) for(int dx=0;dx<zoom;++dx)
          put(7+(col+3)*zoom+dx,11+(row+2)*zoom+dy,rgb);
      }
      const double white=mode==2?2.03:1.0; const Rgb marker{white,white,white};
      for(int t=0;t<zoom;++t) {
        put(7+3*zoom+t,11+2*zoom,marker); put(7+3*zoom+t,11+3*zoom-1,marker);
        put(7+3*zoom,11+2*zoom+t,marker); put(7+4*zoom-1,11+2*zoom+t,marker);
      }
      const int mid_x=7*zoom/2,mid_y=5*zoom/2;
      for(int y=0;y<mid_y-zoom;++y) put(7+mid_x,11+y,marker);
      for(int y=mid_y+zoom+1;y<5*zoom;++y) put(7+mid_x,11+y,marker);
      for(int x=0;x<mid_x-zoom;++x) put(7+x,11+mid_y,marker);
      for(int x=mid_x+zoom+1;x<7*zoom;++x) put(7+x,11+mid_y,marker);
      for(int y=0;y<magnifier_size.height;++y) for(int x=0;x<magnifier_size.width;++x)
        check_pixel(output.value(),x,y,display_oracle(expected[static_cast<std::size_t>(y*magnifier_size.width+x)],mode));
    }
    HDRSHOT_CHECK(r.model.frozen_desktop->canonical_segments.front().rgba_half==original);
    const auto counters=p->resource_counters();
    HDRSHOT_CHECK(counters.source_texture_uploads==1 && counters.clean_content_uploads==1);
  }
}
void magnifier_uses_unannotated_undimmed_source() {
  auto created=WindowsD3DPreviewPresenter::create(nullptr,magnifier_display(2));
  HDRSHOT_CHECK(created.has_value()); const auto p=created.value();
  auto r=magnifier_request();
  auto clean=std::make_shared<CleanContentSnapshot>();
  clean->source=r.model.frozen_desktop; clean->display_id=DisplayId{1}; clean->content_revision=99;
  clean->owned.push_back({61,63,1,ObjectId{99},0xff0000,{{10.F,0.F,0.F,0.F}},true});
  r.model.clean_content=clean;
  r.model.selection.desktop_rect={0,0,128,112};
  auto output=p->render_offscreen(r); HDRSHOT_CHECK(output.has_value());
  check_pixel(output.value(),63,61,display_oracle({10,0,0},2));
  for(int zoom:{4,8,12,16}) {
    // The source point stays outside the empty selection and under an opaque
    // annotation; the magnifier's center interior must still show the source.
    r.model.selection.desktop_rect={};
    r.model.magnifier={true,{63,61},{7,11,5*zoom,5*zoom},zoom,1};
    output=p->render_offscreen(r); HDRSHOT_CHECK(output.has_value());
    check_pixel(output.value(),7+2*zoom+1,11+2*zoom+1,display_oracle(fixture_pixel(63,61),2));
    check_pixel(output.value(),127,111,display_oracle({fixture_pixel(127,111)[0]*.25,
        fixture_pixel(127,111)[1]*.25,fixture_pixel(127,111)[2]*.25},2));
  }
  r.model.magnifier.visible=false;
  output=p->render_offscreen(r); HDRSHOT_CHECK(output.has_value());
  check_pixel(output.value(),63,61,display_oracle({2.5,0,0},2));
  const auto counters=p->resource_counters();
  HDRSHOT_CHECK(counters.source_texture_uploads==1 && counters.clean_content_uploads==1);
}
class CountingSource final:public LinearSource {
 public:
  explicit CountingSource(LinearSourceRef source):source_(std::move(source)) {}
  PixelSize size_px() const override {return source_->size_px();}
  std::size_t byte_count() const override {return source_->byte_count();}
  Result<bool,Error> wait_until_ready() const override {return source_->wait_until_ready();}
  Result<LinearFloatPixels,Error> read_region(PixelRect rect) const override {
    {const std::scoped_lock lock(mutex_);reads_.push_back(rect);}
    return source_->read_region(rect);
  }
  std::vector<PixelRect> reads() const {const std::scoped_lock lock(mutex_);return reads_;}
 private:
  LinearSourceRef source_;
  mutable std::mutex mutex_;
  mutable std::vector<PixelRect> reads_;
};
void magnifier_native_cache_readback_and_frame_replacement() {
  auto r=magnifier_request();
  // A real D3D normalized RGBA32F source, wrapped only to count CPU boundaries.
  const auto raw=r.model.frozen_desktop->canonical_segments.front().rgba_half;
  auto native=windows_normalize_scrgb_source(magnifier_size,raw,1.0F,1.0);
  HDRSHOT_CHECK(native.has_value());
  const auto counting=std::make_shared<CountingSource>(native.value());
  auto frame=std::make_shared<FrozenDesktop>(*r.model.frozen_desktop);
  frame->canonical_segments.front().rgba_half.clear();
  frame->canonical_segments.front().linear_source=counting;
  r.model.frozen_desktop=frame;
  auto created=WindowsD3DPreviewPresenter::create(nullptr,magnifier_display(1));
  HDRSHOT_CHECK(created.has_value()); const auto p=created.value();
  HDRSHOT_CHECK(p->render_offscreen(r).has_value());
  const auto baseline=p->resource_counters();
  HDRSHOT_CHECK(baseline.source_texture_uploads==1 && baseline.clean_content_uploads==1);
  HDRSHOT_CHECK((counting->reads()==std::vector<PixelRect>{{0,0,128,112}}));
  for(int i=0;i<20;++i) {
    r.model.magnifier={true,{i,111-i},{7,11,40,40},8,static_cast<std::uint64_t>(i+1)};
    const auto output=p->render_offscreen(r); HDRSHOT_CHECK(output.has_value());
    // Normalization is sRGB-linear -> P3-linear. The display conversion is
    // its inverse, so the original raw RGB is an independent closed-form oracle.
    check_pixel(output.value(),24,28,fixture_pixel(i,111-i));
  }
  r.model.magnifier.visible=false;
  HDRSHOT_CHECK(p->render_offscreen(r).has_value());
  HDRSHOT_CHECK(counting->reads().size()==1);
  HDRSHOT_CHECK(p->resource_counters().source_texture_uploads==baseline.source_texture_uploads);
  HDRSHOT_CHECK(p->resource_counters().clean_content_uploads==baseline.clean_content_uploads);
  // A new capture frame has one historical full-frame read/upload. This is
  // distinct from pointer movement, which must do neither.
  auto replacement=std::make_shared<FrozenDesktop>(*frame); replacement->frame_id=FrameId{2};
  r.model.frozen_desktop=replacement;
  HDRSHOT_CHECK(p->render_offscreen(r).has_value());
  HDRSHOT_CHECK(counting->reads().size()==2);
  HDRSHOT_CHECK(p->resource_counters().source_texture_uploads==2);
  auto new_native=windows_normalize_scrgb_source(magnifier_size,raw,1.0F,0.5);
  HDRSHOT_CHECK(new_native.has_value());
  auto new_counting=std::make_shared<CountingSource>(new_native.value());
  auto rebound=std::make_shared<FrozenDesktop>(*replacement);
  rebound->canonical_segments.front().linear_source=new_counting;
  r.model.frozen_desktop=rebound;
  r.model.magnifier={true,{63,61},{7,11,40,40},8,22};
  const auto rebound_output=p->render_offscreen(r); HDRSHOT_CHECK(rebound_output.has_value());
  auto half_rgb=fixture_pixel(63,61);for(auto& v:half_rgb)v*=.5;
  check_pixel(rebound_output.value(),24,28,half_rgb);
  HDRSHOT_CHECK(counting->reads().size()==2 && new_counting->reads().size()==1);
  HDRSHOT_CHECK(p->resource_counters().source_texture_uploads==3);
}
void magnifier_invalid_parameters_fail_before_source_read() {
  auto r=magnifier_request();
  auto native=windows_normalize_scrgb_source(magnifier_size,
      r.model.frozen_desktop->canonical_segments.front().rgba_half,1.0F,1.0);
  HDRSHOT_CHECK(native.has_value());
  const auto counting=std::make_shared<CountingSource>(native.value());
  auto frame=std::make_shared<FrozenDesktop>(*r.model.frozen_desktop);
  frame->canonical_segments.front().rgba_half.clear(); frame->canonical_segments.front().linear_source=counting;
  r.model.frozen_desktop=frame;
  auto created=WindowsD3DPreviewPresenter::create(nullptr,magnifier_display(1));
  HDRSHOT_CHECK(created.has_value()); const auto p=created.value();
  const MagnifierPreview valid{true,{63,61},{7,11,40,40},8,1};
  std::vector<MagnifierPreview> invalid;
  for(int zoom:{0,1,5,32,std::numeric_limits<int>::max()}) {auto m=valid;m.zoom=zoom;invalid.push_back(m);}
  for(PixelPoint center:{PixelPoint{-1,0},PixelPoint{0,-1},PixelPoint{128,0},PixelPoint{0,112}}) {
    auto m=valid;m.center=center;invalid.push_back(m);
  }
  for(PixelRect rect:{PixelRect{-1,0,40,40},PixelRect{0,-1,40,40},PixelRect{0,0,0,40},
      PixelRect{0,0,40,-1},PixelRect{90,11,40,40},PixelRect{7,90,40,40},PixelRect{7,11,41,40},
      PixelRect{7,11,32,40},PixelRect{7,11,40,32},PixelRect{std::numeric_limits<int>::max(),0,40,40}}) {
    auto m=valid;m.viewport=rect;invalid.push_back(m);
  }
  for(const auto& m:invalid) {
    r.model.magnifier=m;
    const auto output=p->render_offscreen(r); HDRSHOT_CHECK(!output);
    HDRSHOT_CHECK(output.error().module=="WindowsD3DPreview");
    HDRSHOT_CHECK(output.error().safe_context.at("reason")=="invalid_magnifier");
  }
  HDRSHOT_CHECK(counting->reads().empty());
  HDRSHOT_CHECK(p->resource_counters().source_texture_uploads==0);
  // Hidden parameters are intentionally ignored; stale hidden geometry must
  // neither divide by zero nor prevent the normal preview from drawing.
  r.model.magnifier=invalid.front();r.model.magnifier.visible=false;
  HDRSHOT_CHECK(p->render_offscreen(r).has_value());
  HDRSHOT_CHECK(counting->reads().size()==1);
}
}
int main() {return test::run({{"FP16 linear mask and extended values",linear_mask_and_extended_values},
    {"HDR white units and P3 primaries",white_and_primary_matrix},{"frame contract and cancellation",frame_contract_and_cancellation},
    {"Mac selection border geometry and white",selection_border_matches_mac_geometry},
    {"first frame committed before window exposure",first_frame_commits_while_window_hidden},
    {"magnifier all pixels, modes, zoom and four corners",magnifier_all_pixels_modes_zoom_and_corners},
    {"magnifier unannotated undimmed source",magnifier_uses_unannotated_undimmed_source},
    {"magnifier native cache and frame replacement",magnifier_native_cache_readback_and_frame_replacement},
    {"magnifier invalid parameters before readback",magnifier_invalid_parameters_fail_before_source_read}});}
