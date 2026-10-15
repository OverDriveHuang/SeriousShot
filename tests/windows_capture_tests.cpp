#include "platform/windows/windows_capture.hpp"
#include "platform/windows/windows_capture_operations.hpp"
#include "platform/windows/windows_d3d.hpp"
#include "domain/frame/frame_pipeline.hpp"
#include "test_support.hpp"
#include <future>
#include <iostream>
#include <string_view>
#include <winrt/base.h>
#include <barrier>
#include <thread>
#include <stdexcept>
#ifdef HDRSHOT_TEST_CAPTURE_LOG_SINK
#include "platform/qt/qt_session_diagnostics_port.hpp"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#endif
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
  HDRSHOT_CHECK(!result.error().safe_context.contains("hresult"));
}
Error injected_first_call() {
  try {
    windows_capture_call("InjectedCreateForMonitor", [] { winrt::throw_hresult(E_ACCESSDENIED); });
  } catch (const WindowsCaptureCallFailure& error) { return error.error; }
  throw std::runtime_error("injection did not fail");
}
Error injected_second_call() {
  try {
    windows_capture_call("InjectedStartCapture", [] { winrt::throw_hresult(E_ACCESSDENIED); });
  } catch (const WindowsCaptureCallFailure& error) { return error.error; }
  throw std::runtime_error("injection did not fail");
}
void native_call_origin_and_integer_identity() {
  const auto first = injected_first_call();
  const auto second = injected_second_call();
  HDRSHOT_CHECK(first.safe_context.at("hresult") == "2147942405");
  HDRSHOT_CHECK(second.safe_context.at("hresult") == first.safe_context.at("hresult"));
  HDRSHOT_CHECK(first.origin.line() != second.origin.line());
  HDRSHOT_CHECK(std::string_view(first.origin.function_name()).find("injected_first_call") != std::string_view::npos);
  auto transported = std::async(std::launch::async, [first] { return first; }).get();
  HDRSHOT_CHECK(transported.origin.line() == first.origin.line());
  HDRSHOT_CHECK(transported.safe_context.at("api") == "InjectedCreateForMonitor");
  const auto status = windows_capture_status("InjectedReturnedStatus", [] { return E_ACCESSDENIED; });
  const auto status_error = status.error("returned_status", status.value);
  HDRSHOT_CHECK(status_error.origin.line() == status.origin.line());
  HDRSHOT_CHECK(status_error.safe_context.at("api") == "InjectedReturnedStatus");
  HDRSHOT_CHECK(!windows_capture_error("frame_timeout").safe_context.contains("hresult"));
  HDRSHOT_CHECK(!windows_d3d_error("fixture", "invalid_input").safe_context.contains("hresult"));
}
void frame_state_keeps_real_error_frame_and_terminal_decision() {
  WindowsCaptureFrameState<int> failure;
  failure.receive([]() -> int { winrt::throw_hresult(E_ACCESSDENIED); });
  failure.receive([]() -> int { winrt::throw_hresult(E_FAIL); });
  failure.receive([] { return 4; });
  const auto first = failure.wait(nullptr, 1000);
  HDRSHOT_CHECK(!first && first.error().safe_context.at("hresult") == "2147942405");
  HDRSHOT_CHECK(first.error().safe_context.at("reason") == "frame_acquisition_failed");
  HDRSHOT_CHECK(first.error().safe_context.at("api") == "TryGetNextFrame");
  WindowsCaptureFrameState<int> frame;
  frame.receive([] { return 7; });
  frame.receive([]() -> int { winrt::throw_hresult(E_ACCESSDENIED); });
  HDRSHOT_CHECK(frame.wait(nullptr, 1000).value() == 7);
  WindowsCaptureFrameState<int> timed_out;
  timed_out.receive([] { return 0; });
  const auto timeout = timed_out.wait(nullptr, 0);
  HDRSHOT_CHECK(!timeout && timeout.error().safe_context.at("reason") == "frame_timeout");
  HDRSHOT_CHECK(!timeout.error().safe_context.contains("hresult"));
  timed_out.receive([]() -> int { winrt::throw_hresult(E_ACCESSDENIED); });
  HDRSHOT_CHECK(timed_out.wait(nullptr, 0).error().safe_context.at("reason") == "frame_closed");
  WindowsCaptureFrameState<int> cancelled;
  cancelled.receive([] { return 5; });
  const std::atomic_bool cancel{true};
  const auto cancellation = cancelled.wait(&cancel, 1000);
  HDRSHOT_CHECK(!cancellation && cancellation.error().code == ErrorCode::operation_cancelled);
  HDRSHOT_CHECK(!cancellation.error().safe_context.contains("hresult"));
  WindowsCaptureFrameState<int> internal;
  internal.receive([]() -> int { throw std::bad_alloc{}; });
  const auto internal_error = internal.wait(nullptr, 1000);
  HDRSHOT_CHECK(!internal_error && internal_error.error().safe_context.at("reason") == "frame_callback_failure");
  HDRSHOT_CHECK(!internal_error.error().safe_context.contains("hresult"));
  WindowsCaptureFrameState<int> closed;
  closed.close();
  HDRSHOT_CHECK(closed.wait(nullptr, 1000).error().safe_context.at("reason") == "frame_closed");
}
void frame_callbacks_after_sealing_cannot_replace_timeout() {
  WindowsCaptureFrameState<int> state;
  std::barrier entered{2}, release{2};
  std::thread callback([&] {
    state.receive([&]() -> int {
      entered.arrive_and_wait();
      release.arrive_and_wait();
      winrt::throw_hresult(E_ACCESSDENIED);
    });
  });
  entered.arrive_and_wait();
  const auto timeout = state.wait(nullptr, 0);
  release.arrive_and_wait();
  callback.join();
  HDRSHOT_CHECK(timeout.error().safe_context.at("reason") == "frame_timeout");
  HDRSHOT_CHECK(state.wait(nullptr, 0).error().safe_context.at("reason") == "frame_closed");
  WindowsCaptureFrameState<int> waiting;
  std::promise<void> entering;
  auto entered_wait = entering.get_future();
  auto waiter = std::async(std::launch::async, [&] {
    entering.set_value();
    return waiting.wait(nullptr, 5000);
  });
  entered_wait.wait();
  waiting.receive([]() -> int { winrt::throw_hresult(E_ACCESSDENIED); });
  HDRSHOT_CHECK(waiter.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  HDRSHOT_CHECK(waiter.get().error().safe_context.at("reason") == "frame_acquisition_failed");
}
void cleanup_preserves_primary_and_reports_unique_failure() {
  WindowsCaptureCleanup cleanup;
  int attempts = 0;
  cleanup.attempt("InjectedFrame.Close", [&] { ++attempts; winrt::throw_hresult(E_ACCESSDENIED); });
  cleanup.attempt("InjectedSession.Close", [&] { ++attempts; winrt::throw_hresult(E_FAIL); });
  cleanup.attempt("InjectedPool.Close", [&] { ++attempts; });
  HDRSHOT_CHECK(attempts == 3 && cleanup.failed());
  const auto primary = injected_second_call();
  const auto failed = cleanup.complete(Result<int, Error>::failure(primary));
  HDRSHOT_CHECK(failed.error() == primary && failed.error().origin.line() == primary.origin.line());
  const auto only_cleanup = cleanup.complete(Result<int, Error>::success(1));
  HDRSHOT_CHECK(!only_cleanup && only_cleanup.error().safe_context.at("reason") == "capture_cleanup_failed");
  HDRSHOT_CHECK(only_cleanup.error().safe_context.at("api") == "InjectedFrame.Close");
  HDRSHOT_CHECK(only_cleanup.error().safe_context.at("hresult") == "2147942405");
  WindowsCaptureCleanup allocation;
  allocation.attempt("InjectedCloseAllocation", [] { throw std::bad_alloc{}; });
  allocation.attempt("StillAttempted", [&] { ++attempts; });
  HDRSHOT_CHECK(attempts == 4);
  HDRSHOT_CHECK(allocation.complete(Result<int, Error>::failure(primary)).error() == primary);
  HDRSHOT_CHECK(!allocation.complete(Result<int, Error>::success(1)).error().safe_context.contains("hresult"));
}
#ifdef HDRSHOT_TEST_CAPTURE_LOG_SINK
void injected_boundaries_survive_actual_sink() {
  QTemporaryDir temporary;
  const auto evidence = qEnvironmentVariable("HDRSHOT_CAPTURE_LOG_EVIDENCE_DIR");
  const auto root = evidence.isEmpty() ? temporary.path() : evidence;
  for (const bool detailed : {false, true}) {
    QtSessionDiagnosticsPort log(QDir(root).filePath(detailed ? "detailed" : "simple").toStdString());
    HDRSHOT_CHECK(log.start("synthetic_injection_only_not_fault_machine", "Windows").has_value());
    log.set_detailed_logging(detailed);
    const auto first = injected_first_call();
    const auto second = std::async(std::launch::async, [] { return injected_second_call(); }).get();
    WindowsCaptureFrameState<int> state;
    const auto timeout = state.wait(nullptr, 0);
    for (const auto* error : {&first, &second, &timeout.error()})
      record_diagnostic_stage(&log, SessionId{71}, OperationId{9}, "capture", "ready", "failure",
          {{"displayId", "17"}}, error);
    QFile file(QString::fromStdString(log.exact_path()));
    HDRSHOT_CHECK(file.open(QIODevice::ReadOnly));
    const auto bytes = file.readAll();
    HDRSHOT_CHECK(bytes.contains("synthetic_injection_only_not_fault_machine"));
    HDRSHOT_CHECK(bytes.count("hresult=0x80070005") == 2);
    HDRSHOT_CHECK(bytes.count("reason=frame_timeout") == 1);
    HDRSHOT_CHECK(bytes.contains(" at=") == detailed);
    if (detailed) {
      HDRSHOT_CHECK(bytes.contains("api=InjectedCreateForMonitor"));
      HDRSHOT_CHECK(bytes.contains("api=InjectedStartCapture"));
      HDRSHOT_CHECK(bytes.contains(QByteArray("windows_capture_tests.cpp:") + QByteArray::number(first.origin.line())));
      HDRSHOT_CHECK(bytes.contains(QByteArray("windows_capture_tests.cpp:") + QByteArray::number(second.origin.line())));
      HDRSHOT_CHECK(bytes.contains("displayId=17"));
    }
    if (!evidence.isEmpty()) std::cout << "SYNTHETIC injection evidence: " << log.exact_path() << '\n';
  }
}
#endif
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
        HDRSHOT_CHECK(frame.capture_frozen_sdr==!display.source_white.hdr_active);
        const auto source=frame.linear_source;
        const auto interpreted=SourceColorInterpreter::interpret(std::move(frame),display.snapshot);
        HDRSHOT_CHECK(interpreted && interpreted.value().linear_source==source);
        HDRSHOT_CHECK(interpreted.value().capture_sdr_tolerance==display.source_white.hdr_active);
        HDRSHOT_CHECK(interpreted.value().capture_frozen_sdr==!display.source_white.hdr_active);
        std::cout<<"display="<<display.snapshot.id.value<<" frozen_mode="<<display.advanced_color_mode
            <<" hdr="<<display.source_white.hdr_active<<" system_white="<<display.source_white.sdr_white_nits
            <<" tolerance="<<interpreted.value().capture_sdr_tolerance<<" size="<<display.snapshot.capture_size_px.width
            <<'x'<<display.snapshot.capture_size_px.height<<" capture_publish_ms="
            <<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count()<<'\n';
      }
    }}});
  }
  return test::run({
#ifdef HDRSHOT_TEST_CAPTURE_LOG_SINK
    {"synthetic native boundaries through actual sink",injected_boundaries_survive_actual_sink},
#endif
    {"native call origin survives future and returned status",native_call_origin_and_integer_identity},
    {"frame error, accepted frame, timeout and cancellation",frame_state_keeps_real_error_frame_and_terminal_decision},
    {"late callback cannot replace sealed decision",frame_callbacks_after_sealing_cannot_replace_timeout},
    {"independent cleanup preserves primary",cleanup_preserves_primary_and_reports_unique_failure},
    {"immutable per-session display catalog",immutable_catalog},
    {"capture errors always complete",invalid_and_missing_target_complete}});}
