#include "platform/windows/windows_capture.hpp"
#include "test_support.hpp"
#include <future>
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
int main() {return test::run({{"immutable per-session display catalog",immutable_catalog},
    {"capture errors always complete",invalid_and_missing_target_complete}});}
