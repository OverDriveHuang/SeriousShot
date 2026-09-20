#include "platform/windows/windows_d3d_presenter.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include "test_support.hpp"
#include <future>
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
  display.source_white=white;
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
    display.source_white={false,80};
    auto p=WindowsD3DPreviewPresenter::create(hwnd,display);HDRSHOT_CHECK(p.has_value());
    std::promise<Result<PresentReceipt,Error>> done;auto ready=done.get_future();
    p.value()->present(request(),[&](auto result){done.set_value(std::move(result));});
    HDRSHOT_CHECK(ready.wait_for(std::chrono::seconds(3))==std::future_status::ready);
    HDRSHOT_CHECK(ready.get().has_value());
    HDRSHOT_CHECK(!IsWindowVisible(hwnd));
  }
  DestroyWindow(hwnd);
}
}
int main() {return test::run({{"FP16 linear mask and extended values",linear_mask_and_extended_values},
    {"HDR white units and P3 primaries",white_and_primary_matrix},{"frame contract and cancellation",frame_contract_and_cancellation},
    {"Mac selection border geometry and white",selection_border_matches_mac_geometry},
    {"first frame committed before window exposure",first_frame_commits_while_window_hidden}});}
