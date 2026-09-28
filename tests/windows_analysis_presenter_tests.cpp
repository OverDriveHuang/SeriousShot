#include "domain/analysis/math.hpp"
#include "platform/cpu/cpu_analysis_port.hpp"
#include "platform/windows/windows_analysis_backend.hpp"
#include "test_support.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <limits>
#include <thread>
#include <windows.h>
using namespace hdrshot;
namespace {
analysis::Input fixture() {
  analysis::FloatImage pixels;
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 8; ++x)
      pixels.push_back({float(x) / 2, float(y) / 2, .25F, 1});
  auto source = make_cpu_analysis_source({8, 4}, std::move(pixels));
  HDRSHOT_CHECK(source.has_value());
  return {source.value(), 1, true};
}
analysis::SourceView view() {
  analysis::SourceView v;
  v.target_size = {16, 8};
  v.scale = 2;
  return v;
}
void compare_reference() {
  auto input = fixture();
  auto cpu = make_cpu_analysis_port();
  for (const bool false_color : {false, true})
    for (const double sigma : {0., 1., 8.}) {
      auto v = view();
      v.false_color = false_color;
      v.settings.blur_sigma_px = sigma;
      v.mask = {true, analysis::MaskShape::ellipse, {1, 0, 5, 3}};
      analysis::ReportPlan plan;
      plan.revision = 1;
      plan.source_rect = {0, 0, 16, 8};
      plan.source_view = v;
      plan.underlay.size = plan.overlay.size = {16, 8};
      plan.underlay.rgba.resize(16 * 8 * 4);
      plan.overlay.rgba.resize(16 * 8 * 4);
      auto reference = cpu->compose_report(input, plan);
      auto actual = windows_analysis_render_offscreen(input, v);
      HDRSHOT_CHECK(reference.has_value());
      HDRSHOT_CHECK(actual.has_value());
      auto a = actual.value()->read_region({0, 0, 16, 8});
      auto b = reference.value()->read_region({0, 0, 16, 8});
      HDRSHOT_CHECK(a.has_value() && b.has_value());
      for (std::size_t i = 0; i < a.value().size(); ++i) {
        HDRSHOT_CHECK(std::isfinite(a.value()[i]));
        HDRSHOT_CHECK_NEAR(a.value()[i], b.value()[i], 3e-5);
      }
    }
}
void transparent_marks_and_hdr() {
  auto input = fixture();
  auto v = view();
  auto baseline = windows_analysis_render_offscreen(input, v);
  HDRSHOT_CHECK(baseline.has_value());
  auto marks = std::make_shared<analysis::UiImage>();
  marks->size = v.target_size;
  marks->rgba.resize(16 * 8 * 4);
  marks->rgba[0] = 255;
  marks->rgba[3] = 255; // solid red in sRGB, decoded before mixing
  marks->rgba[4] = 255;
  marks->rgba[7] = 128;
  v.operation_overlay = marks;
  auto actual = windows_analysis_render_offscreen(input, v);
  HDRSHOT_CHECK(actual.has_value());
  auto a = actual.value()->read_region({0, 0, 16, 8}).value();
  auto b = baseline.value()->read_region({0, 0, 16, 8}).value();
  auto red = analysis_math::ui_rgb_to_linear_p3(0xff0000u);
  HDRSHOT_CHECK_NEAR(a[0], red.x, 2e-6);
  HDRSHOT_CHECK_NEAR(a[1], red.y, 2e-6);
  HDRSHOT_CHECK_NEAR(a[2], red.z, 2e-6);
  HDRSHOT_CHECK_NEAR(a[4], b[4] * (127.F / 255) + red.x * (128.F / 255), 2e-6);
  for (std::size_t i = 8; i < a.size(); ++i)
    HDRSHOT_CHECK_NEAR(a[i], b[i], 1e-7);
  HDRSHOT_CHECK(a[(7 * 16 + 15) * 4] >
                3); // float HDR survives transparent marks
}
void validation_and_sdr() {
  auto input = fixture();
  auto v = view();
  v.scale = 0;
  HDRSHOT_CHECK(!windows_analysis_render_offscreen(input, v));
  v = view();
  auto invalid = std::make_shared<analysis::UiImage>();
  invalid->size = v.target_size;
  v.operation_overlay = invalid;
  HDRSHOT_CHECK(!windows_analysis_render_offscreen(input, v));
  v = view();
  v.settings.working_space = analysis::WorkingSpace::display_p3_sdr;
  auto result = windows_analysis_render_offscreen(input, v);
  HDRSHOT_CHECK(result.has_value());
  auto pixels = result.value()->read_region({0, 0, 16, 8});
  HDRSHOT_CHECK(pixels.has_value());
  for (float c : pixels.value())
    HDRSHOT_CHECK(std::isfinite(c) && c >= 0 && c <= 1);
}
void worker_latest_and_close() {
  auto result = make_windows_analysis_presenter();
  HDRSHOT_CHECK(result.has_value());
  auto presenter = result.value();
  auto input = fixture();
  auto v = view();
  HDRSHOT_CHECK(!presenter->present(input, v, 0));
  auto errors = std::make_shared<std::atomic<int>>(0);
  set_windows_analysis_presenter_error_callback(*presenter,
                                                [errors](Error) { ++*errors; });
  // A deliberately invalid non-null handle exercises asynchronous failure and
  // coalescing without a visible window or input from the user.
  for (int i = 0; i < 100; ++i) {
    v.offset_x = i;
    HDRSHOT_CHECK(presenter->present(input, v, 1).has_value());
  }
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  WindowsAnalysisPresentationStatus status;
  do {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    status = windows_analysis_presentation_status(*presenter);
  } while ((status.pending || status.worker_active || !status.failed) &&
           std::chrono::steady_clock::now() < deadline);
  HDRSHOT_CHECK(status.requested == 100);
  HDRSHOT_CHECK(status.failed > 0);
  HDRSHOT_CHECK(!status.pending && !status.worker_active);
  HDRSHOT_CHECK(status.submitted + status.dropped == 100);
  set_windows_analysis_presenter_error_callback(*presenter, {});
  for (int i = 0; i < 100; ++i)
    presenter->present(input, v, 1);
  result.value().reset();
  auto started = std::chrono::steady_clock::now();
  presenter.reset();
  HDRSHOT_CHECK(std::chrono::steady_clock::now() - started <
                std::chrono::milliseconds(100));
}
void callback_can_reenter_and_revoke_delivery() {
  struct CallbackMailbox {
    std::promise<void> delivered;
    std::atomic<int> calls{0};
    std::atomic<bool> saw_failed_status{false};
  };
  auto created = make_windows_analysis_presenter();
  HDRSHOT_CHECK(created.has_value());
  auto presenter = created.value();
  std::weak_ptr<AnalysisPresenterPort> weak = presenter;
  auto input = fixture();
  auto v = view();
  auto mailbox = std::make_shared<CallbackMailbox>();
  auto first = mailbox->delivered.get_future();
  set_windows_analysis_presenter_error_callback(*presenter, [weak, mailbox](Error) {
    const auto call = ++mailbox->calls;
    if (auto active = weak.lock()) {
      const auto status = windows_analysis_presentation_status(*active);
      mailbox->saw_failed_status = status.failed > 0;
      set_windows_analysis_presenter_error_callback(*active, {});
    }
    if (call == 1)
      mailbox->delivered.set_value();
  });
  HDRSHOT_CHECK(presenter->present(input, v, 1).has_value());
  HDRSHOT_CHECK(first.wait_for(std::chrono::seconds(5)) ==
                std::future_status::ready);
  HDRSHOT_CHECK(mailbox->calls == 1);
  HDRSHOT_CHECK(mailbox->saw_failed_status.load());
  HDRSHOT_CHECK(presenter->present(input, v, 1).has_value());
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (windows_analysis_presentation_status(*presenter).failed < 2 &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  HDRSHOT_CHECK(windows_analysis_presentation_status(*presenter).failed >= 2);
  HDRSHOT_CHECK(mailbox->calls == 1);
}
} // namespace
int main() {
  return hdrshot::test::run(
      {{"D3D Source and False Color match independent CPU composition",
        compare_reference},
       {"straight-alpha marks retain HDR pixels", transparent_marks_and_hdr},
       {"invalid views fail and SDR display clips", validation_and_sdr},
       {"latest pending failure and nonblocking close",
        worker_latest_and_close},
       {"error callback reentry and revocation",
        callback_can_reenter_and_revoke_delivery}});
}
