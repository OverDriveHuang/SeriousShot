#include "platform/windows/windows_capture.hpp"
#include "domain/frame/frame_pipeline.hpp"
#include "test_support.hpp"
#include <future>
#include <iostream>
#include <string_view>
#include <winrt/base.h>
using namespace hdrshot;
namespace {
void immutable_catalog() {
  WindowsDisplayInfo display;display.snapshot.id=DisplayId{17};display.snapshot.capture_size_px={200,100};
  auto source=std::make_shared<const std::vector<WindowsDisplayInfo>>(std::vector{display});
  WindowsDisplayCatalogPort catalog(source);
  DisplayGeneration first=0;
  catalog.snapshot_displays({},[&](auto result) {HDRSHOT_CHECK(result.has_value());
    HDRSHOT_CHECK(result.value().displays==std::vector{display.snapshot});first=result.value().generation;});
  catalog.snapshot_displays({},[&](auto result) {HDRSHOT_CHECK(result.value().generation>first);});
}
void invalid_and_missing_target_complete() {
  WindowsCapturePort port(std::make_shared<const std::vector<WindowsDisplayInfo>>());
  bool completed=false;
  port.capture({},[&](auto result) {HDRSHOT_CHECK(!result);completed=true;});
  HDRSHOT_CHECK(completed);
  std::promise<Result<NativeFrameBatch,Error>> done;auto ready=done.get_future();
  port.capture({SessionId{1},OperationId{2},3,{DisplayId{17}}},[&](auto result){done.set_value(std::move(result));});
  HDRSHOT_CHECK(ready.wait_for(std::chrono::seconds(2))==std::future_status::ready);
  const auto result=ready.get();HDRSHOT_CHECK(!result);
  HDRSHOT_CHECK(result.error().safe_context.at("reason")=="target_display_missing");
}
}
int main(int argc, char** argv) {
  // Explicit hardware mode; ordinary CTest remains deterministic and private
  // normalized source is not additionally read back and no pixels are written.
  if(argc==2 && std::string_view(argv[1])=="--real-capture") {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    return test::run({{"real frozen capture provenance",[] {
      const auto enumerated=windows_enumerate_displays();
      HDRSHOT_CHECK(enumerated.has_value() && !enumerated.value().empty());
      const auto displays=std::make_shared<const std::vector<WindowsDisplayInfo>>(enumerated.value());
      WindowsCapturePort port(displays,WindowsCaptureOptions{false,false,1.0});
      std::cout<<"Windows build="<<windows_build_number()<<" diagnostic options: gain_enabled=0 gain=1 white_bypass=0\n";
      for(const auto& display:*displays) {
        auto done=std::make_shared<std::promise<Result<NativeFrameBatch,Error>>>(); auto ready=done->get_future();
        const auto start=std::chrono::steady_clock::now();
        port.capture({SessionId{1},OperationId{display.snapshot.id.value},1,{display.snapshot.id}},
            [done](auto result) { done->set_value(std::move(result)); });
        HDRSHOT_CHECK(ready.wait_for(std::chrono::seconds(15))==std::future_status::ready);
        auto result=ready.get();
        if(!result) for(const auto& [key,value]:result.error().safe_context) std::cout<<key<<'='<<value<<' ';
        HDRSHOT_CHECK(result.has_value() && result.value().frames.size()==1);
        auto& frame=result.value().frames.front();
        HDRSHOT_CHECK(frame.capture_sdr_tolerance==display.source_white.hdr_active);
        const auto source=frame.linear_source;
        const auto interpreted=SourceColorInterpreter::interpret(std::move(frame),display.snapshot);
        HDRSHOT_CHECK(interpreted && interpreted.value().linear_source==source);
        HDRSHOT_CHECK(interpreted.value().capture_sdr_tolerance==display.source_white.hdr_active);
        std::cout<<"display="<<display.snapshot.id.value<<" frozen_mode="<<display.advanced_color_mode
            <<" hdr="<<display.source_white.hdr_active<<" system_white="<<display.source_white.sdr_white_nits
            <<" tolerance="<<interpreted.value().capture_sdr_tolerance<<" size="<<display.snapshot.capture_size_px.width
            <<'x'<<display.snapshot.capture_size_px.height<<" capture_publish_ms="
            <<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count()<<'\n';
      }
    }}});
  }
  return test::run({{"immutable per-session display catalog",immutable_catalog},
    {"capture errors always complete",invalid_and_missing_target_complete}});}
