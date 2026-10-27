#include "application/capture_progress_watchdog.hpp"
#include "test_support.hpp"
#include <thread>
#include <barrier>
using namespace hdrshot;
using namespace std::chrono_literals;
namespace {
struct Fixture {
  CaptureSteadyTime now{};
  CaptureOperationKey key{{1}, {2}, 3};
  CaptureProgressWatchdog watch{key, [this] { return now; }};
  void at(int ms) { now = CaptureSteadyTime{} + std::chrono::milliseconds(ms); }
  auto image(int id) { return watch.receive({key, DisplayId{static_cast<std::uint64_t>(id)}, CaptureNativeEventKind::image_received, {}}); }
};
void boundary() {
  Fixture f; f.watch.start({{1}}); f.at(1999);
  HDRSHOT_CHECK(!f.watch.check_deadline(1).snapshot.terminal()); f.at(2000);
  HDRSHOT_CHECK(f.image(1).snapshot.phase == CaptureProgressWatchdog::Phase::timed_out);
  HDRSHOT_CHECK(f.watch.stop_requested());
  HDRSHOT_CHECK(f.watch.snapshot().error->safe_context.at("reason") == "capture_progress_timeout");
}
void sliding_window() {
  Fixture f; f.watch.start({{1},{2},{3}});
  f.at(1500); f.image(1); f.at(3000); f.image(2); f.at(4500); f.image(3);
  HDRSHOT_CHECK(f.watch.snapshot().phase == CaptureProgressWatchdog::Phase::processing);
  f.at(50000); HDRSHOT_CHECK(!f.watch.check_deadline(1).snapshot.terminal());
  HDRSHOT_CHECK(f.watch.finish({}).snapshot.phase == CaptureProgressWatchdog::Phase::succeeded);
  Fixture held; held.watch.start({{1},{2},{3}}); held.at(1500); held.image(1); held.at(3000); held.image(2);
  held.at(4999); HDRSHOT_CHECK(!held.watch.check_deadline(3).snapshot.terminal());
  held.at(5000); HDRSHOT_CHECK(held.watch.check_deadline(3).snapshot.phase == CaptureProgressWatchdog::Phase::timed_out);
}
void stale_and_early_failure() {
  Fixture f; f.watch.start({{1},{2}}); f.at(1500); f.image(1);
  const auto epoch = f.watch.snapshot().epoch;
  f.at(2500); f.image(1); f.image(9);
  auto wrong = f.key; ++wrong.display_generation;
  f.watch.receive({wrong, {2}, CaptureNativeEventKind::image_received, {}});
  HDRSHOT_CHECK(f.watch.snapshot().epoch == epoch);
  f.watch.check_deadline(1); HDRSHOT_CHECK(!f.watch.stop_requested());
  Error error{ErrorCode::capture_failed,"native",Retryability::same_input,{{"nativeCode","-9"}}};
  f.watch.receive({f.key,{2},CaptureNativeEventKind::failed,error}); f.at(9999);
  f.watch.check_deadline(epoch); HDRSHOT_CHECK(f.watch.snapshot().error->safe_context.at("nativeCode") == "-9");
}
void registration_before_delayed_timer() {
  Fixture f; f.watch.start({{1},{2}}); f.at(1999); f.image(1); f.at(2001);
  f.watch.check_deadline(1); HDRSHOT_CHECK(!f.watch.stop_requested());
  f.image(2); HDRSHOT_CHECK(!f.watch.snapshot().deadline);
  Fixture late; late.watch.start({{1}}); late.at(2001); late.image(1);
  HDRSHOT_CHECK(late.watch.stop_requested());
}
void concurrent_boundary_single_terminal() {
  for(int iteration=0;iteration<300;++iteration) {
    Fixture f; f.watch.start({{1}}); f.at(2000); std::barrier ready(3); std::atomic_int winners{};
    std::thread native([&] { ready.arrive_and_wait(); if(f.image(1).changed) ++winners; });
    std::thread timer([&] { ready.arrive_and_wait(); if(f.watch.check_deadline(1).changed) ++winners; });
    ready.arrive_and_wait(); native.join(); timer.join();
    HDRSHOT_CHECK(winners==1 && f.watch.snapshot().phase==CaptureProgressWatchdog::Phase::timed_out);
  }
}

}
int main() { return test::run({{"concurrent boundary",concurrent_boundary_single_terminal},{"boundary",boundary},{"sliding window",sliding_window},
  {"stale and error",stale_and_early_failure},{"native registration",registration_before_delayed_timer}}); }
