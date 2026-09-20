#include "application/analysis_session.hpp"
#include "application/analysis_workflow.hpp"
#include "domain/annotation/annotation_pixel_plan_validator.hpp"
#include "platform/cpu/cpu_analysis_port.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace hdrshot;
using namespace std::chrono_literals;
using AnalysisResult = Result<analysis::ResultRef, Error>;
using SourceResult = Result<LinearSourceRef, Error>;

LinearSourceRef source(PixelSize size = {2, 1}, float value = 0.25F) {
  auto result = make_cpu_analysis_source(
      size, analysis::FloatImage(static_cast<std::size_t>(size.width) *
                                     static_cast<std::size_t>(size.height),
                                 {value, value, value, 1.0F}));
  HDRSHOT_CHECK(result.has_value());
  return std::move(result.value());
}

Error injected_error() {
  return {ErrorCode::presenter_failed,
          "InjectedAnalysisExecutor",
          Retryability::same_input,
          {{"reason", "injected_failure"}}};
}

template <typename T>
void check_error(const Result<T, Error> &result, const char *module,
                 const char *reason) {
  HDRSHOT_CHECK(!result);
  HDRSHOT_CHECK(result.error().module == module);
  HDRSHOT_CHECK(result.error().safe_context.at("reason") == reason);
}

struct Call {
  char kind{};
  std::uint64_t revision{};
  friend bool operator==(const Call &, const Call &) = default;
};

// The first call can be held until all competing requests have been accepted.
// No test depends on a sleep being long enough for a worker to start.
struct WorkerControl {
  std::mutex mutex;
  std::condition_variable changed;
  bool block_first{true}, released{};
  std::uint64_t throw_analysis{}, throw_report{};
  std::vector<Call> calls;
  std::vector<analysis::Request> requests;
  std::vector<analysis::ReportPlan> reports;

  void enter(Call call) {
    std::unique_lock lock(mutex);
    calls.push_back(call);
    changed.notify_all();
    if (calls.size() == 1 && block_first)
      changed.wait(lock, [&] { return released; });
  }
  bool wait_for_calls(std::size_t count) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, 3s, [&] { return calls.size() >= count; });
  }
  void release() {
    {
      std::scoped_lock lock(mutex);
      released = true;
    }
    changed.notify_all();
  }
  std::vector<Call> recorded_calls() {
    std::scoped_lock lock(mutex);
    return calls;
  }
};

class ControlledExecutor final : public AnalysisPort {
public:
  explicit ControlledExecutor(std::shared_ptr<WorkerControl> control)
      : control_(std::move(control)) {}
  SourceResult prepare(const SelectionRoiView &,
                       const AnnotationPixelPlan &) override {
    return SourceResult::failure(injected_error());
  }
  AnalysisResult analyze(const analysis::Input &,
                         const analysis::Request &request) override {
    control_->enter({'a', request.revision});
    {
      std::scoped_lock lock(control_->mutex);
      control_->requests.push_back(request);
    }
    if (request.revision == control_->throw_analysis)
      throw std::runtime_error("private executor detail must not escape");
    auto result = std::make_shared<analysis::ResultData>();
    result->revision = request.revision;
    return AnalysisResult::success(std::move(result));
  }
  SourceResult compose_report(const analysis::Input &input,
                              const analysis::ReportPlan &plan) override {
    control_->enter({'r', plan.revision});
    {
      std::scoped_lock lock(control_->mutex);
      control_->reports.push_back(plan);
    }
    if (plan.revision == control_->throw_report)
      throw std::runtime_error("private report detail must not escape");
    return SourceResult::success(input.source);
  }

private:
  std::shared_ptr<WorkerControl> control_;
};

struct CompletionLog {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<Call> calls;
  std::vector<Error> errors;
  void record(Call call, std::optional<Error> error = {}) {
    {
      std::scoped_lock lock(mutex);
      calls.push_back(call);
      if (error)
        errors.push_back(std::move(*error));
    }
    changed.notify_all();
  }
  bool wait_for_calls(std::size_t count) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, 3s, [&] { return calls.size() >= count; });
  }
  std::vector<Call> recorded_calls() {
    std::scoped_lock lock(mutex);
    return calls;
  }
  std::vector<Error> recorded_errors() {
    std::scoped_lock lock(mutex);
    return errors;
  }
};

AnalysisSession::Completion completion(std::shared_ptr<CompletionLog> log,
                                       std::uint64_t revision) {
  return [log = std::move(log), revision](AnalysisResult result) {
    log->record({'a', revision},
                result ? std::nullopt : std::optional{result.error()});
  };
}
AnalysisSession::ReportCompletion
report_completion(std::shared_ptr<CompletionLog> log, std::uint64_t revision) {
  return [log = std::move(log), revision](SourceResult result) {
    log->record({'r', revision},
                result ? std::nullopt : std::optional{result.error()});
  };
}

struct SessionFixture {
  std::shared_ptr<WorkerControl> control = std::make_shared<WorkerControl>();
  std::shared_ptr<CompletionLog> log = std::make_shared<CompletionLog>();
  std::shared_ptr<ControlledExecutor> executor =
      std::make_shared<ControlledExecutor>(control);
  std::unique_ptr<AnalysisSession> session = std::make_unique<AnalysisSession>(
      analysis::Input{source(), 71, false}, executor);
  ~SessionFixture() {
    session->stop();
    control->release();
    // Also clean up safely when an assertion above throws.
    session->wait_for_stopped(3s);
  }
  bool request(std::uint64_t revision) {
    analysis::Request request;
    request.revision = revision;
    request.mask = {
        true, analysis::MaskShape::rectangle, {0, 0, double(revision), 1}};
    return session->request(std::move(request), completion(log, revision));
  }
  bool report(std::uint64_t revision) {
    analysis::ReportPlan plan;
    plan.revision = revision;
    return session->report(std::move(plan), report_completion(log, revision));
  }
  void finish() {
    session->stop();
    control->release();
    HDRSHOT_CHECK(session->wait_for_stopped(3s));
  }
};

void latest_pending_coalesces_and_suppresses_obsolete_completion() {
  SessionFixture fixture;
  HDRSHOT_CHECK(fixture.request(1));
  HDRSHOT_CHECK(fixture.control->wait_for_calls(1));
  HDRSHOT_CHECK(fixture.request(2));
  HDRSHOT_CHECK(fixture.request(3));
  analysis::Request newest;
  newest.revision = 4;
  newest.mask = {true, analysis::MaskShape::rectangle, {0, 0, 4, 1}};
  newest.samples.push_back({19, 1, 0, 5, false});
  HDRSHOT_CHECK(fixture.session->request(newest, completion(fixture.log, 4)));
  newest.samples.front().side = 99;
  HDRSHOT_CHECK(!fixture.request(3));
  fixture.control->release();
  HDRSHOT_CHECK(fixture.log->wait_for_calls(1));
  fixture.finish();
  HDRSHOT_CHECK((fixture.control->recorded_calls() ==
                 std::vector<Call>{{'a', 1}, {'a', 4}}));
  HDRSHOT_CHECK((fixture.log->recorded_calls() == std::vector<Call>{{'a', 4}}));
  HDRSHOT_CHECK(fixture.log->recorded_errors().empty());
  HDRSHOT_CHECK(fixture.control->requests.back().samples.front().side == 5);
}

void completed_same_content_is_delivered_during_continuous_hover() {
  SessionFixture fixture;
  analysis::Request request;
  request.revision = 1;
  request.samples = {{0, 0, 0, 1, true}};
  HDRSHOT_CHECK(fixture.session->request(request, completion(fixture.log, 1)));
  HDRSHOT_CHECK(fixture.control->wait_for_calls(1));
  request.revision = 2;
  request.samples[0].x = 1;
  HDRSHOT_CHECK(fixture.session->request(request, completion(fixture.log, 2)));
  fixture.control->release();
  HDRSHOT_CHECK(fixture.log->wait_for_calls(2));
  fixture.finish();
  HDRSHOT_CHECK(
      (fixture.log->recorded_calls() == std::vector<Call>{{'a', 1}, {'a', 2}}));
}

void accepted_reports_are_fifo_frozen_and_independent_of_latest_request() {
  SessionFixture fixture;
  HDRSHOT_CHECK(fixture.request(1));
  HDRSHOT_CHECK(fixture.control->wait_for_calls(1));
  analysis::ReportPlan plan;
  plan.revision = 31;
  plan.underlay = {{1, 1}, {10, 20, 30, 255}};
  plan.source_view.offset_x = 17.0;
  HDRSHOT_CHECK(
      fixture.session->report(plan, report_completion(fixture.log, 31)));
  plan.underlay.rgba.front() = 200;
  plan.source_view.offset_x = 99.0;
  HDRSHOT_CHECK(fixture.report(32));
  HDRSHOT_CHECK(fixture.report(33));
  HDRSHOT_CHECK(fixture.request(2));
  fixture.control->release();
  HDRSHOT_CHECK(fixture.log->wait_for_calls(4));
  fixture.finish();
  HDRSHOT_CHECK(
      (fixture.control->recorded_calls() ==
       std::vector<Call>{{'a', 1}, {'r', 31}, {'r', 32}, {'r', 33}, {'a', 2}}));
  HDRSHOT_CHECK((fixture.log->recorded_calls() ==
                 std::vector<Call>{{'r', 31}, {'r', 32}, {'r', 33}, {'a', 2}}));
  HDRSHOT_CHECK(fixture.control->reports.front().underlay.rgba.front() == 10);
  HDRSHOT_CHECK(fixture.control->reports.front().source_view.offset_x == 17.0);
}

void report_queue_is_bounded_without_dropping_accepted_work() {
  SessionFixture fixture;
  HDRSHOT_CHECK(fixture.request(1));
  HDRSHOT_CHECK(fixture.control->wait_for_calls(1));
  for (std::uint64_t revision = 10; revision < 18; ++revision)
    HDRSHOT_CHECK(fixture.report(revision));
  HDRSHOT_CHECK(!fixture.report(18));
  fixture.control->release();
  HDRSHOT_CHECK(fixture.log->wait_for_calls(9));
  fixture.finish();
  auto actual = fixture.log->recorded_calls();
  HDRSHOT_CHECK((actual.front() == Call{'a', 1}));
  for (std::size_t index = 1; index < actual.size(); ++index)
    HDRSHOT_CHECK((actual[index] == Call{'r', index + 9}));
}

void stop_is_nonblocking_discards_callbacks_and_releases_resources() {
  SessionFixture fixture;
  HDRSHOT_CHECK(fixture.request(1));
  HDRSHOT_CHECK(fixture.control->wait_for_calls(1));
  auto pending_lifetime = std::make_shared<int>(1);
  std::weak_ptr<int> pending_weak = pending_lifetime;
  analysis::Request pending;
  pending.revision = 2;
  HDRSHOT_CHECK(
      fixture.session->request(pending, [pending_lifetime](AnalysisResult) {}));
  pending_lifetime.reset();
  auto report_lifetime = std::make_shared<int>(2);
  std::weak_ptr<int> report_weak = report_lifetime;
  HDRSHOT_CHECK(
      fixture.session->report({}, [report_lifetime](SourceResult) {}));
  report_lifetime.reset();
  std::weak_ptr<ControlledExecutor> executor_weak = fixture.executor;
  fixture.executor.reset();
  const auto begin = std::chrono::steady_clock::now();
  fixture.session->stop();
  HDRSHOT_CHECK(std::chrono::steady_clock::now() - begin < 500ms);
  HDRSHOT_CHECK(!fixture.session->wait_for_stopped(0ms));
  HDRSHOT_CHECK(!executor_weak.expired());
  HDRSHOT_CHECK(pending_weak.expired());
  HDRSHOT_CHECK(report_weak.expired());
  HDRSHOT_CHECK(!fixture.request(3));
  HDRSHOT_CHECK(!fixture.report(9));
  fixture.finish();
  HDRSHOT_CHECK(executor_weak.expired());
  HDRSHOT_CHECK(fixture.log->recorded_calls().empty());
  HDRSHOT_CHECK(
      (fixture.control->recorded_calls() == std::vector<Call>{{'a', 1}}));
}

void stopped_session_releases_input_even_while_session_object_survives() {
  auto input_source = source();
  std::weak_ptr<const LinearSource> input_weak = input_source;
  AnalysisSession session({input_source, 5, true}, make_cpu_analysis_port());
  input_source.reset();
  HDRSHOT_CHECK(!input_weak.expired());
  session.stop();
  HDRSHOT_CHECK(session.wait_for_stopped(3s));
  HDRSHOT_CHECK(input_weak.expired());
  session.stop();
  HDRSHOT_CHECK(session.wait_for_stopped(0ms));
}

void executor_exceptions_are_labelled_and_do_not_kill_the_worker() {
  SessionFixture fixture;
  fixture.control->throw_analysis = 1;
  fixture.control->throw_report = 7;
  HDRSHOT_CHECK(fixture.request(1));
  HDRSHOT_CHECK(fixture.control->wait_for_calls(1));
  fixture.control->release();
  HDRSHOT_CHECK(fixture.log->wait_for_calls(1));
  HDRSHOT_CHECK(fixture.report(7));
  HDRSHOT_CHECK(fixture.log->wait_for_calls(2));
  HDRSHOT_CHECK(fixture.request(2));
  HDRSHOT_CHECK(fixture.log->wait_for_calls(3));
  fixture.finish();
  const auto errors = fixture.log->recorded_errors();
  HDRSHOT_CHECK(errors.size() == 2);
  for (const auto &error : errors) {
    HDRSHOT_CHECK(error.code == ErrorCode::state_inconsistent);
    HDRSHOT_CHECK(error.module == "AnalysisSession");
    HDRSHOT_CHECK(error.retryability == Retryability::after_recreate);
    HDRSHOT_CHECK(
        (error.safe_context ==
         std::map<std::string, std::string>{{"reason", "executor_exception"}}));
  }
  HDRSHOT_CHECK((fixture.log->recorded_calls().back() == Call{'a', 2}));
}

void throwing_completion_does_not_prevent_later_reports() {
  SessionFixture fixture;
  analysis::Request request;
  request.revision = 1;
  HDRSHOT_CHECK(fixture.session->request(request, [](AnalysisResult) {
    throw std::runtime_error("consumer exception");
  }));
  HDRSHOT_CHECK(fixture.control->wait_for_calls(1));
  HDRSHOT_CHECK(fixture.report(2));
  fixture.control->release();
  HDRSHOT_CHECK(fixture.log->wait_for_calls(1));
  fixture.finish();
  HDRSHOT_CHECK((fixture.log->recorded_calls() == std::vector<Call>{{'r', 2}}));
}

void null_executor_reports_failure_and_can_stop() {
  auto log = std::make_shared<CompletionLog>();
  AnalysisSession session({source(), 1, false}, {});
  analysis::Request request;
  request.revision = 1;
  HDRSHOT_CHECK(session.request(request, completion(log, 1)));
  HDRSHOT_CHECK(log->wait_for_calls(1));
  analysis::ReportPlan plan;
  plan.revision = 2;
  HDRSHOT_CHECK(session.report(plan, report_completion(log, 2)));
  HDRSHOT_CHECK(log->wait_for_calls(2));
  session.stop();
  HDRSHOT_CHECK(session.wait_for_stopped(3s));
  HDRSHOT_CHECK(log->recorded_errors().size() == 2);
}

ExportSnapshot snapshot(bool annotated = false, bool visible_hdr = false) {
  CanonicalFrameSegment segment;
  segment.display_id = DisplayId{11};
  segment.desktop_frame_points = {0, 0, 6, 4};
  segment.size_px = {6, 4};
  segment.pixel_format = PixelFormat::rgba32_float;
  segment.encoding = {ColorPrimaries::display_p3, TransferFunction::linear,
                      AlphaMode::opaque, 0.0};
  segment.display_dynamic_range = DisplayDynamicRange::hdr;
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 6; ++x)
      segment.rgba_float.insert(segment.rgba_float.end(),
                                {0.05F * static_cast<float>(x + 1),
                                 0.025F * static_cast<float>(y + 1), 0.25F, 1});
  segment.rgba_float[0] = 32; // Outside the selected ROI must never select HDR.
  if (annotated) {
    segment.rgba_float[(6 + 1) * 4] =
        8; // Partial AA retains some HDR in clean S.
    segment.rgba_float[(6 + 2) * 4] = 12; // Fully covered source is hidden.
  }
  if (visible_hdr)
    segment.rgba_float[(6 + 3) * 4 + 2] = 2;
  CanonicalFrameSegment other;
  other.display_id = DisplayId{12};
  other.desktop_frame_points = {6, 0, 2, 2};
  other.size_px = {2, 2};
  other.pixel_format = PixelFormat::rgba32_float;
  other.encoding = segment.encoding;
  other.display_dynamic_range = DisplayDynamicRange::hdr;
  other.linear_source = source({2, 2}, 16);
  auto desktop = std::make_shared<FrozenDesktop>(FrozenDesktop{
      FrameId{21}, 22, {0, 0, 8, 4}, {std::move(segment), std::move(other)}});
  ExportSnapshot result;
  result.session_id = SessionId{23};
  result.operation_id = OperationId{24};
  result.frozen_desktop = std::move(desktop);
  result.selection = {25, {1, 1, 3, 2}};
  result.annotations.revision = 26;
  result.target_display_id = DisplayId{11};
  result.default_folder = "/fixture/immutable-output-preference";
  result.save_format = SaveFormat::ultra_hdr_jpeg;
  result.pq_diffuse_white = PqDiffuseWhite::nits_100;
  result.hdr_pq_precision = HdrPqPrecision::bits_16;
  result.ultra_hdr_jpeg_quality = UltraHdrJpegQuality::compact;
  auto plan = std::make_shared<AnnotationRenderPlan>(AnnotationRenderPlan{
      result.annotations.revision, {3, 2}, {}, result.selection.desktop_rect});
  if (annotated) {
    plan->ordered_layers.push_back({ObjectId{27},
                                    AnnotationKind::text,
                                    {0, 0, 2, 1},
                                    0x00ff00,
                                    {{0, 0, {64, 255}}},
                                    "fixture-text"});
    result.annotations.objects.push_back({ObjectId{27},
                                          AnnotationKind::text,
                                          {1, 1, 2, 1},
                                          TextStyle{0x00ff00, 20},
                                          "fixture-text",
                                          std::nullopt,
                                          {}});
    CleanContentCache cache;
    auto clean =
        cache.get(result.frozen_desktop, result.target_display_id, *plan);
    HDRSHOT_CHECK(clean.has_value());
    result.clean_content = std::move(clean.value());
    result.clean_document_revision = result.annotations.revision;
  }
  result.annotation_render_plan = std::move(plan);
  return result;
}

class PrepareSpy final : public AnalysisPort {
public:
  enum class Behavior { normal, fail, null_source, wrong_size };
  Behavior behavior{Behavior::normal};
  int calls{};
  AnnotationPixelPlan received_plan;
  PixelSize received_size{};
  SourceResult prepare(const SelectionRoiView &view,
                       const AnnotationPixelPlan &plan) override {
    ++calls;
    received_size = view.size_px;
    received_plan = plan;
    if (behavior == Behavior::fail)
      return SourceResult::failure(injected_error());
    if (behavior == Behavior::null_source)
      return SourceResult::success({});
    if (behavior == Behavior::wrong_size)
      return SourceResult::success(source({1, 1}));
    return make_cpu_analysis_port()->prepare(view, plan);
  }
  AnalysisResult analyze(const analysis::Input &,
                         const analysis::Request &) override {
    return AnalysisResult::failure(injected_error());
  }
  SourceResult compose_report(const analysis::Input &,
                              const analysis::ReportPlan &) override {
    return SourceResult::failure(injected_error());
  }
};

void check_preferences(const ExportSnapshot &actual,
                       const ExportSnapshot &expected) {
  HDRSHOT_CHECK(actual.session_id == expected.session_id);
  HDRSHOT_CHECK(actual.operation_id == expected.operation_id);
  HDRSHOT_CHECK(actual.default_folder == expected.default_folder);
  HDRSHOT_CHECK(actual.save_format == expected.save_format);
  HDRSHOT_CHECK(actual.pq_diffuse_white == expected.pq_diffuse_white);
  HDRSHOT_CHECK(actual.hdr_pq_precision == expected.hdr_pq_precision);
  HDRSHOT_CHECK(actual.ultra_hdr_jpeg_quality ==
                expected.ultra_hdr_jpeg_quality);
  HDRSHOT_CHECK(actual.selection.revision == expected.selection.revision);
  HDRSHOT_CHECK(actual.annotations.revision == expected.annotations.revision);
  HDRSHOT_CHECK(actual.frozen_desktop->frame_id ==
                expected.frozen_desktop->frame_id);
  HDRSHOT_CHECK(actual.frozen_desktop->display_generation ==
                expected.frozen_desktop->display_generation);
}

void preparation_compacts_roi_preserves_settings_and_releases_desktop() {
  auto original = snapshot();
  std::weak_ptr<const FrozenDesktop> desktop_weak = original.frozen_desktop;
  std::weak_ptr<const LinearSource> other_display_weak =
      original.frozen_desktop->canonical_segments[1].linear_source;
  PrepareSpy executor;
  auto prepared = AnalysisWorkflow::prepare(original, executor);
  HDRSHOT_CHECK(prepared.has_value());
  HDRSHOT_CHECK(executor.calls == 1);
  HDRSHOT_CHECK((executor.received_size == PixelSize{3, 2}));
  HDRSHOT_CHECK(prepared.value().input.revision == original.operation_id.value);
  HDRSHOT_CHECK(!prepared.value().input.is_hdr);
  check_preferences(prepared.value().original, original);
  const auto &compact = prepared.value().original;
  HDRSHOT_CHECK((compact.selection.desktop_rect == PixelRect{0, 0, 3, 2}));
  HDRSHOT_CHECK(compact.frozen_desktop->canonical_segments.size() == 1);
  const auto &segment = compact.frozen_desktop->canonical_segments.front();
  HDRSHOT_CHECK(segment.linear_source == prepared.value().input.source);
  HDRSHOT_CHECK((segment.size_px == PixelSize{3, 2}));
  HDRSHOT_CHECK(segment.rgba_float.empty() && segment.rgba_half.empty());
  HDRSHOT_CHECK(segment.encoding.transfer == TransferFunction::linear);
  HDRSHOT_CHECK(segment.encoding.primaries == ColorPrimaries::display_p3);
  HDRSHOT_CHECK(compact.clean_content->source == compact.frozen_desktop);
  HDRSHOT_CHECK(compact.annotations.objects.empty());
  auto pixels = prepared.value().input.source->read_region({0, 0, 3, 2});
  HDRSHOT_CHECK(pixels && pixels.value().size() == 24);
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 3; ++x) {
      const auto index = static_cast<std::size_t>(y * 3 + x) * 4;
      HDRSHOT_CHECK_NEAR(pixels.value()[index],
                         0.05F * static_cast<float>(x + 2), 0);
      HDRSHOT_CHECK_NEAR(pixels.value()[index + 1],
                         0.025F * static_cast<float>(y + 2), 0);
      HDRSHOT_CHECK(pixels.value()[index + 3] == 1);
    }
  original = {};
  HDRSHOT_CHECK(desktop_weak.expired());
  HDRSHOT_CHECK(other_display_weak.expired());
}

void clean_annotations_are_in_analysis_and_keep_original_output_ownership() {
  auto original = snapshot(true);
  original.save_format = SaveFormat::png_display_p3_dual_range;
  PrepareSpy executor;
  auto prepared = AnalysisWorkflow::prepare(original, executor);
  HDRSHOT_CHECK(prepared.has_value());
  HDRSHOT_CHECK(!prepared.value().input.is_hdr);
  HDRSHOT_CHECK(!executor.received_plan.annotation_owned_spans.empty());
  HDRSHOT_CHECK(AnnotationPixelPlanValidator::valid(executor.received_plan));
  auto pixels = prepared.value().input.source->read_region({0, 0, 3, 2});
  HDRSHOT_CHECK(pixels && pixels.value()[0] > 1.0F);
  for (const auto &span : executor.received_plan.annotation_owned_spans) {
    HDRSHOT_CHECK(span.clean_composited);
    for (int x = 0; x < span.length; ++x)
      for (std::size_t c = 0; c < 3; ++c)
        HDRSHOT_CHECK_NEAR(
            pixels
                .value()[static_cast<std::size_t>(span.y * 3 + span.x + x) * 4 +
                         c],
            span.edge_samples[static_cast<std::size_t>(x)][c], 0);
  }
  const auto &compact = prepared.value().original;
  auto plan = compact.clean_content->roi_plan(compact.selection.desktop_rect,
                                              compact.annotations.revision);
  HDRSHOT_CHECK(plan.has_value());
  HDRSHOT_CHECK(plan.value() == executor.received_plan);
  auto view = FrameCropper::view_display(
      *compact.frozen_desktop, compact.target_display_id, compact.selection);
  HDRSHOT_CHECK(view.has_value());
  auto fit = SourceRangeProbe::probe(view.value(), plan.value());
  HDRSHOT_CHECK(fit && fit.value().fits_sdr);
  HDRSHOT_CHECK(fit.value().skipped_annotation_pixel_count == 2);
  auto cpu = make_cpu_analysis_port();
  analysis::Request request;
  request.revision = 100;
  request.scopes.wave_width = 8;
  request.scopes.wave_height = 8;
  request.scopes.vector_width = 8;
  request.scopes.vector_height = 8;
  request.scopes.histogram_bins = 16;
  auto analysis_result = cpu->analyze(prepared.value().input, request);
  HDRSHOT_CHECK(analysis_result && analysis_result.value()->valid_count == 6);
  auto before = ExportWorkflow::prepare(original);
  auto after = ExportWorkflow::prepare(compact);
  HDRSHOT_CHECK(before && after);
  HDRSHOT_CHECK(before.value().output_encoding.transfer ==
                TransferFunction::srgb);
  HDRSHOT_CHECK(after.value().output_encoding ==
                before.value().output_encoding);
  HDRSHOT_CHECK(after.value().artifact.bytes == before.value().artifact.bytes);
}

void source_visible_hdr_and_output_precision_survive_analysis_entry() {
  for (bool annotated : {false, true}) {
    auto original = snapshot(annotated, true);
    original.save_format = SaveFormat::png_display_p3_dual_range;
    PrepareSpy executor;
    auto prepared = AnalysisWorkflow::prepare(original, executor);
    HDRSHOT_CHECK(prepared && prepared.value().input.is_hdr);
    check_preferences(prepared.value().original, original);
    auto before = ExportWorkflow::prepare(original);
    auto after = ExportWorkflow::prepare(prepared.value().original);
    HDRSHOT_CHECK(before && after);
    HDRSHOT_CHECK(after.value().output_encoding.transfer ==
                  TransferFunction::pq);
    HDRSHOT_CHECK(after.value().output_encoding ==
                  before.value().output_encoding);
    HDRSHOT_CHECK(after.value().artifact.bytes ==
                  before.value().artifact.bytes);
  }
}

void missing_stale_and_unclean_inputs_fail_before_executor() {
  PrepareSpy executor;
  auto missing = snapshot();
  missing.frozen_desktop.reset();
  check_error(AnalysisWorkflow::prepare(missing, executor), "AnalysisWorkflow",
              "missing_selection_source");
  auto empty = snapshot();
  empty.selection.desktop_rect = {};
  check_error(AnalysisWorkflow::prepare(empty, executor), "AnalysisWorkflow",
              "missing_selection_source");
  for (int variant = 0; variant < 3; ++variant) {
    auto stale = snapshot();
    auto plan =
        std::make_shared<AnnotationRenderPlan>(*stale.annotation_render_plan);
    if (variant == 0)
      stale.annotation_render_plan.reset();
    if (variant == 1) {
      ++plan->source_document_revision;
      stale.annotation_render_plan = plan;
    }
    if (variant == 2) {
      ++plan->source_selection_rect_px.x;
      stale.annotation_render_plan = plan;
    }
    check_error(AnalysisWorkflow::prepare(stale, executor), "AnalysisWorkflow",
                "stale_annotation_render_plan");
  }
  for (int variant = 0; variant < 3; ++variant) {
    auto stale = snapshot(true);
    auto clean = std::make_shared<CleanContentSnapshot>(*stale.clean_content);
    if (variant == 0)
      clean->source = std::make_shared<FrozenDesktop>(*stale.frozen_desktop);
    if (variant == 1)
      clean->display_id = DisplayId{12};
    if (variant == 2)
      ++stale.clean_document_revision;
    stale.clean_content = std::move(clean);
    check_error(AnalysisWorkflow::prepare(stale, executor), "AnalysisWorkflow",
                "stale_clean_content");
  }
  auto unclean = snapshot(true);
  unclean.clean_content.reset();
  check_error(AnalysisWorkflow::prepare(unclean, executor), "AnalysisWorkflow",
              "clean_annotations_required");
  auto malformed = snapshot(true);
  auto clean = std::make_shared<CleanContentSnapshot>(*malformed.clean_content);
  clean->owned.front().edge_samples.front()[3] = 0.5F;
  malformed.clean_content = std::move(clean);
  check_error(AnalysisWorkflow::prepare(malformed, executor),
              "AnalysisWorkflow", "invalid_annotation_pixel_plan");
  auto missing_display = snapshot();
  missing_display.target_display_id = DisplayId{999};
  HDRSHOT_CHECK(!AnalysisWorkflow::prepare(missing_display, executor));
  HDRSHOT_CHECK(executor.calls == 0);
}

void range_probe_and_prepare_errors_do_not_publish_a_snapshot() {
  class Probe final : public SourceRangeProbePort {
  public:
    bool fail{true}, received_guarantee{true};
    int calls{};
    Result<RangeFitResult, Error>
    probe(const SelectionRoiView &, const AnnotationPixelPlan &,
          RangeProbeOptimization optimization) override {
      ++calls;
      received_guarantee = optimization.source_visible_within_sdr_guaranteed;
      if (fail)
        return Result<RangeFitResult, Error>::failure(injected_error());
      return Result<RangeFitResult, Error>::success(
          {false, 6, 0, 1, "injected_hdr"});
    }
  } probe;
  PrepareSpy executor;
  auto original = snapshot();
  auto result = AnalysisWorkflow::prepare(original, executor, &probe);
  HDRSHOT_CHECK(!result && result.error() == injected_error());
  HDRSHOT_CHECK(executor.calls == 0 && probe.calls == 1);
  HDRSHOT_CHECK(!probe.received_guarantee);
  probe.fail = false;
  result = AnalysisWorkflow::prepare(original, executor, &probe);
  HDRSHOT_CHECK(result && result.value().input.is_hdr);
  executor.behavior = PrepareSpy::Behavior::fail;
  result = AnalysisWorkflow::prepare(original, executor);
  HDRSHOT_CHECK(!result && result.error() == injected_error());
  for (auto behavior :
       {PrepareSpy::Behavior::null_source, PrepareSpy::Behavior::wrong_size}) {
    executor.behavior = behavior;
    check_error(AnalysisWorkflow::prepare(original, executor),
                "AnalysisWorkflow", "prepared_source_shape_mismatch");
  }
  HDRSHOT_CHECK(original.frozen_desktop->canonical_segments.size() == 2);
  HDRSHOT_CHECK((original.selection.desktop_rect == PixelRect{1, 1, 3, 2}));
}

void report_snapshot_preserves_preferences_and_reclassifies_visible_report_pixels() {
  auto original = snapshot(true);
  PrepareSpy executor;
  auto prepared = AnalysisWorkflow::prepare(original, executor);
  HDRSHOT_CHECK(prepared && !prepared.value().input.is_hdr);
  // The image is a composed report now: the original annotation exemption must
  // not hide its visible >1 values from the report's own HDR classification.
  auto report = AnalysisWorkflow::report_snapshot(
      prepared.value().original, prepared.value().input.source);
  HDRSHOT_CHECK(report.has_value());
  check_preferences(report.value(), original);
  HDRSHOT_CHECK(report.value().annotations.objects.empty());
  HDRSHOT_CHECK(report.value().annotation_render_plan->ordered_layers.empty());
  HDRSHOT_CHECK(report.value().clean_content->owned.empty());
  auto reprobed = AnalysisWorkflow::prepare(report.value(), executor);
  HDRSHOT_CHECK(reprobed && reprobed.value().input.is_hdr);
  auto resized = AnalysisWorkflow::report_snapshot(prepared.value().original,
                                                   source({7, 5}, 0.5F));
  HDRSHOT_CHECK(resized.has_value());
  HDRSHOT_CHECK(
      (resized.value().selection.desktop_rect == PixelRect{0, 0, 7, 5}));
  HDRSHOT_CHECK((resized.value().annotation_render_plan->output_size_px ==
                 PixelSize{7, 5}));
  HDRSHOT_CHECK(prepared.value().original.clean_content->owned.size() > 0);
  check_error(AnalysisWorkflow::report_snapshot(original, {}),
              "AnalysisWorkflow", "invalid_composed_report");
  check_error(AnalysisWorkflow::report_snapshot({}, source()),
              "AnalysisWorkflow", "invalid_composed_report");
}
} // namespace

int main() {
  return test::run({
      {"latest pending request coalesces and obsolete result is not published",
       latest_pending_coalesces_and_suppresses_obsolete_completion},
      {"same-content completions survive continuous hover",
       completed_same_content_is_delivered_during_continuous_hover},
      {"accepted reports retain FIFO and frozen values",
       accepted_reports_are_fifo_frozen_and_independent_of_latest_request},
      {"bounded report queue completes every accepted report",
       report_queue_is_bounded_without_dropping_accepted_work},
      {"stop does not join active executor and discards pending callbacks",
       stop_is_nonblocking_discards_callbacks_and_releases_resources},
      {"stopped session releases its immutable input",
       stopped_session_releases_input_even_while_session_object_survives},
      {"executor exceptions are labelled and worker recovers",
       executor_exceptions_are_labelled_and_do_not_kill_the_worker},
      {"throwing completion leaves subsequent report runnable",
       throwing_completion_does_not_prevent_later_reports},
      {"null executor produces failures and shuts down",
       null_executor_reports_failure_and_can_stop},
      {"analysis entry compacts ROI and preserves original preferences",
       preparation_compacts_roi_preserves_settings_and_releases_desktop},
      {"clean AA participates in analysis while original output ownership "
       "survives",
       clean_annotations_are_in_analysis_and_keep_original_output_ownership},
      {"source-visible HDR and PNG bytes survive analysis entry",
       source_visible_hdr_and_output_precision_survive_analysis_entry},
      {"missing stale and unclean inputs fail before preparation",
       missing_stale_and_unclean_inputs_fail_before_executor},
      {"range and preparation failures publish no snapshot",
       range_probe_and_prepare_errors_do_not_publish_a_snapshot},
      {"report snapshot uses report pixels and preserves output preferences",
       report_snapshot_preserves_preferences_and_reclassifies_visible_report_pixels},
  });
}
