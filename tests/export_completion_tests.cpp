#include "application/export_completion_coordinator.hpp"
#include "test_support.hpp"
#include <deque>
#include <map>
#include <optional>
using namespace hdrshot;
namespace {
Error failure(ErrorCode code) { return {code, "fake", Retryability::after_user_action, {}}; }
struct Executor : ExportTaskExecutorPort {
  std::deque<std::function<void()>> bg, ui;
  bool is_main{true};
  void background(std::function<void()> work) override { bg.push_back(std::move(work)); }
  void main_thread(std::function<void()> work) override { ui.push_back(std::move(work)); }
  void run_background() {
    is_main = false;
    while (!bg.empty()) { auto work = std::move(bg.front()); bg.pop_front(); work(); }
    is_main = true;
  }
  void run_main() {
    while (!ui.empty()) { auto work = std::move(ui.front()); ui.pop_front(); work(); }
  }
};
struct Clipboard : ClipboardPort {
  Executor& executor;
  int writes{};
  bool reject{};
  explicit Clipboard(Executor& e) : executor(e) {}
  Result<ClipboardReceipt, Error> write(const ClipboardWriteRequest& r) override {
    HDRSHOT_CHECK(executor.is_main);
    ++writes;
    if (reject) return Result<ClipboardReceipt, Error>::failure({ErrorCode::clipboard_rejected,
        "fake", Retryability::same_input, {{"reason", "publication_not_visible"}}});
    return Result<ClipboardReceipt, Error>::success({r.bytes.size(), r.mime_types});
  }
};
struct Files : FileStorePort {
  bool directory_ok{true}, fail_write{};
  int aborts{}, prepare_calls{};
  std::map<std::string, std::vector<std::uint8_t>> saved;
  struct Sink : AtomicFileSink {
    Files& files; std::string path; std::vector<std::uint8_t> bytes;
    Sink(Files& f, std::string p) : files(f), path(std::move(p)) {}
    Result<std::size_t, Error> write(std::span<const std::uint8_t> data) override {
      if (files.fail_write) return Result<std::size_t, Error>::failure(failure(ErrorCode::storage_full));
      bytes.insert(bytes.end(), data.begin(), data.end());
      return Result<std::size_t, Error>::success(data.size());
    }
    Result<FileReceipt, Error> commit() override {
      files.saved[path] = bytes;
      return Result<FileReceipt, Error>::success({path, bytes.size()});
    }
    void abort() noexcept override { ++files.aborts; }
  };
  Result<bool, Error> prepare_directory(const std::string&) override {
    ++prepare_calls;
    return directory_ok ? Result<bool, Error>::success(true) :
        Result<bool, Error>::failure(failure(ErrorCode::path_not_writable));
  }
  Result<std::unique_ptr<AtomicFileSink>, Error> open_atomic(const OpenAtomicFileRequest& r) override {
    if (!r.overwrite && saved.contains(r.exact_path))
      return Result<std::unique_ptr<AtomicFileSink>, Error>::failure(failure(ErrorCode::path_already_exists));
    return Result<std::unique_ptr<AtomicFileSink>, Error>::success(std::make_unique<Sink>(*this, r.exact_path));
  }
  Result<FileReceipt, Error> write(const WriteFileRequest&) override {
    return Result<FileReceipt, Error>::failure(failure(ErrorCode::invalid_input));
  }
};
struct Dialog : FileDialogPort {
  int calls{}; bool cancel{};
  ChooseSavePathRequest request;
  Result<ChooseSavePathOutcome, Error> choose_save_path(const ChooseSavePathRequest& r) override {
    ++calls; request = r;
    if (cancel) return Result<ChooseSavePathOutcome, Error>::success(UserCancelled{});
    return Result<ChooseSavePathOutcome, Error>::success(ChosenPath{"/chosen/image.png"});
  }
};
struct Clock : ClockPort {
  LocalDateTime now_local() const override { return {2026, 9, 5, 10, 0, 0}; }
};
struct Diagnostics : DiagnosticsPort {
  std::vector<DiagnosticEvent> events;
  bool fail{}, detailed{};
  bool detailed_logging() const override { return detailed; }
  Result<DiagnosticReceipt, Error> record(const DiagnosticEvent& event) override {
    events.push_back(event);
    return fail ? Result<DiagnosticReceipt, Error>::failure(failure(ErrorCode::path_not_writable)) :
        Result<DiagnosticReceipt, Error>::success({events.size()});
  }
};
struct JpegFixture : UltraHdrInputRendererPort, UltraHdrEncoderPort {
  double maximum{0.5};
  Result<LinearDisplayP3HalfImage, Error> render(const UltraHdrInputRenderRequest& r) override {
    return Result<LinearDisplayP3HalfImage, Error>::success({r.source->size_px,
        std::vector<std::uint16_t>(static_cast<std::size_t>(r.source->size_px.width * r.source->size_px.height) * 4U, 0x3800),
        kUltraHdrReferenceWhiteNits, maximum, maximum});
  }
  Result<EncodedUltraHdrJpeg, Error> encode(const UltraHdrEncodeRequest&) override {
    // A fake port result, not a JPEG fidelity test.
    return Result<EncodedUltraHdrJpeg, Error>::success({{0xff, 0xd8, 0xff, 0xd9}, jpeg_output_kind(maximum)});
  }
};
ExportSnapshot snapshot() {
  auto frame = std::make_shared<FrozenDesktop>(FrozenDesktop{
      FrameId{1}, 1, LogicalRect{0,0,4,4}, {CanonicalFrameSegment{
        DisplayId{1}, LogicalRect{0,0,4,4}, 1, PixelSize{4,4}, PixelFormat::rgba16_float,
        {ColorPrimaries::display_p3, TransferFunction::extended_srgb, AlphaMode::straight, 0},
        DisplayDynamicRange::sdr, std::vector<std::uint16_t>(64, 0x3800)}}});
  return {SessionId{1}, OperationId{2}, frame, SelectionSnapshot{1, PixelRect{0,0,4,4}},
      AnnotationDocument::empty().snapshot(), nullptr, "/default", DisplayId{1}};
}
struct Fixture {
  Executor executor; Files files; Dialog dialog; Clock clock; Clipboard clipboard{executor};
  Diagnostics diagnostics; JpegFixture jpeg;
  std::vector<bool> dialog_visibility;
  ExportCompletionCoordinator coordinator{
      {files, dialog, clipboard, clock, executor, nullptr, nullptr, &jpeg, &jpeg},
      [this](bool active, DisplayId) { dialog_visibility.push_back(active); }, {}, &diagnostics};
  std::optional<ExportCompletionCoordinator::Outcome> result;
  auto done() { return [this](auto r) { HDRSHOT_CHECK(executor.is_main); result = std::move(r); }; }
};
void copy_returns_before_work_and_publishes_on_main() {
  Fixture f;
  HDRSHOT_CHECK(f.coordinator.submit(UiCommand::copy_and_close, snapshot(), f.done()));
  HDRSHOT_CHECK(!f.result && f.clipboard.writes == 0 && f.executor.bg.size() == 1);
  f.executor.run_background();
  HDRSHOT_CHECK(!f.result && f.clipboard.writes == 0);
  f.executor.run_main();
  HDRSHOT_CHECK(f.result && f.result->has_value() && f.clipboard.writes == 1);
  HDRSHOT_CHECK(f.files.prepare_calls == 0 && f.dialog.calls == 0);
}
void invalid_directory_cancel_keeps_editor() {
  Fixture f; f.files.directory_ok = false; f.dialog.cancel = true;
  HDRSHOT_CHECK(!f.coordinator.submit(UiCommand::save_default, snapshot(), f.done()));
  HDRSHOT_CHECK(f.dialog.calls == 1 && f.executor.bg.empty() && f.files.saved.empty());
  HDRSHOT_CHECK(f.result && std::holds_alternative<UserCancelled>(f.result->value().destination));
  HDRSHOT_CHECK(f.dialog_visibility == std::vector<bool>({true, false}));
}
void invalid_directory_uses_chosen_destination() {
  Fixture f; f.files.directory_ok = false;
  HDRSHOT_CHECK(f.coordinator.submit(UiCommand::save_default, snapshot(), f.done()));
  HDRSHOT_CHECK(f.dialog.calls == 1 && !f.result && f.files.saved.empty());
  f.executor.run_background(); f.executor.run_main();
  HDRSHOT_CHECK(f.result && f.result->has_value());
  HDRSHOT_CHECK(f.files.saved.contains("/chosen/image.png"));
}
void default_save_collisions_do_not_overwrite() {
  Fixture f;
  for (int i=0;i<2;++i) {
    HDRSHOT_CHECK(f.coordinator.submit(UiCommand::save_default, snapshot(), f.done()));
    f.executor.run_background(); f.executor.run_main();
    HDRSHOT_CHECK(f.result && f.result->has_value());
  }
  HDRSHOT_CHECK(f.dialog.calls == 0 && f.files.saved.size() == 2);
  HDRSHOT_CHECK(f.files.saved.contains("/default/SeriousShot_2026-09-05_10-00-00_1.png"));
}
void failed_write_is_reported_after_acceptance() {
  Fixture f; f.files.fail_write = true;
  HDRSHOT_CHECK(f.coordinator.submit(UiCommand::save_as, snapshot(), f.done()));
  HDRSHOT_CHECK(!f.result);
  f.executor.run_background(); f.executor.run_main();
  HDRSHOT_CHECK(f.result && !f.result->has_value());
  HDRSHOT_CHECK(f.result->error().code == ErrorCode::storage_full);
  HDRSHOT_CHECK(f.files.aborts == 1 && f.files.saved.empty());
}
void one_terminal_record_uses_actual_output_range() {
  for (const bool hdr : {false, true}) for (const bool jpeg : {false, true}) {
    Fixture f;
    auto shot = snapshot();
    if (hdr) {
      auto desktop = std::make_shared<FrozenDesktop>(*shot.frozen_desktop);
      desktop->canonical_segments.front().rgba_half.assign(64, 0x4000);
      desktop->canonical_segments.front().display_dynamic_range = DisplayDynamicRange::hdr;
      shot.frozen_desktop = desktop;
    }
    if (jpeg) shot.save_format = SaveFormat::ultra_hdr_jpeg;
    f.jpeg.maximum = hdr ? 2.0 : 0.5;
    HDRSHOT_CHECK(f.coordinator.submit(UiCommand::copy_and_close, shot, f.done()));
    HDRSHOT_CHECK(f.diagnostics.events.empty());
    f.executor.run_background();
    HDRSHOT_CHECK(f.diagnostics.events.empty());
    f.executor.run_main();
    HDRSHOT_CHECK(f.result && f.result->has_value());
    HDRSHOT_CHECK(f.diagnostics.events.size() == 1);
    const auto& event = f.diagnostics.events.front();
    HDRSHOT_CHECK(event.stage == "complete" && event.command == "copy" && event.outcome == "success");
    HDRSHOT_CHECK(event.safe_context.at("format") == (jpeg ? "JPEG" : "PNG"));
    HDRSHOT_CHECK(event.safe_context.at("range") == (hdr ? "HDR" : "SDR"));
    HDRSHOT_CHECK(std::stoll(event.safe_context.at("elapsedMs")) >= 0);
  }
}
void terminal_failure_cancel_and_logging_failure() {
  Fixture copy_failed; copy_failed.clipboard.reject = true;
  HDRSHOT_CHECK(copy_failed.coordinator.submit(UiCommand::copy_and_close, snapshot(), copy_failed.done()));
  copy_failed.executor.run_background(); copy_failed.executor.run_main();
  HDRSHOT_CHECK(copy_failed.result && !copy_failed.result->has_value());
  HDRSHOT_CHECK(copy_failed.diagnostics.events.size() == 1);
  HDRSHOT_CHECK(copy_failed.diagnostics.events[0].error_code == "clipboard_rejected");
  HDRSHOT_CHECK(copy_failed.diagnostics.events[0].safe_context.at("reason") == "publication_not_visible");
  Fixture failed; failed.files.fail_write = true;
  HDRSHOT_CHECK(failed.coordinator.submit(UiCommand::save_as, snapshot(), failed.done()));
  failed.executor.run_background(); failed.executor.run_main();
  HDRSHOT_CHECK(failed.diagnostics.events.size() == 1);
  const auto& event = failed.diagnostics.events.front();
  HDRSHOT_CHECK(event.outcome == "failure" && event.command == "save_as");
  HDRSHOT_CHECK(event.error_code == "storage_full" && event.safe_context.at("range") == "unknown");
  Fixture cancelled; cancelled.dialog.cancel = true;
  HDRSHOT_CHECK(!cancelled.coordinator.submit(UiCommand::save_as, snapshot(), cancelled.done()));
  HDRSHOT_CHECK(cancelled.diagnostics.events.size() == 1);
  HDRSHOT_CHECK(cancelled.diagnostics.events.front().outcome == "cancelled");
  HDRSHOT_CHECK(cancelled.diagnostics.events.front().safe_context.at("range") == "unknown");
  Fixture rejected;
  HDRSHOT_CHECK(!rejected.coordinator.submit(UiCommand::cancel_capture, snapshot(), rejected.done()));
  HDRSHOT_CHECK(rejected.diagnostics.events.size() == 1);
  HDRSHOT_CHECK(rejected.diagnostics.events.front().outcome == "failure");
  Fixture log_failure; log_failure.diagnostics.fail = true;
  HDRSHOT_CHECK(log_failure.coordinator.submit(UiCommand::save_default, snapshot(), log_failure.done()));
  log_failure.executor.run_background(); log_failure.executor.run_main();
  HDRSHOT_CHECK(log_failure.result && log_failure.result->has_value());
  HDRSHOT_CHECK(log_failure.files.saved.size() == 1);
}
void detailed_export_stages_and_error_origin() {
  Fixture f; f.diagnostics.detailed = true; f.clipboard.reject = true;
  HDRSHOT_CHECK(f.coordinator.submit(UiCommand::copy_and_close, snapshot(), f.done()));
  f.executor.run_background(); f.executor.run_main();
  std::vector<std::string> stages;
  for (const auto& event : f.diagnostics.events) {
    stages.push_back(event.stage);
    HDRSHOT_CHECK(event.session_id == SessionId{1} && event.operation_id == OperationId{2});
    HDRSHOT_CHECK(event.safe_context.at("format") == "PNG");
    HDRSHOT_CHECK(event.safe_context.at("width") == "4");
  }
  HDRSHOT_CHECK(stages == std::vector<std::string>({"queued", "encode_started", "publish", "complete"}));
  const auto& last = f.diagnostics.events.back();
  HDRSHOT_CHECK(last.error_origin.has_value());
  HDRSHOT_CHECK(std::string(last.error_origin->function_name()).find("Clipboard::write") != std::string::npos);
  HDRSHOT_CHECK(last.error_code == "clipboard_rejected");
  HDRSHOT_CHECK(last.safe_context.at("reason") == "publication_not_visible");
}
}  // namespace
int main() {
  return test::run({
    {"detailed export stages preserve failure origin", detailed_export_stages_and_error_origin},
    {"copy acceptance and main-thread publish", copy_returns_before_work_and_publishes_on_main},
    {"invalid directory cancel keeps editor", invalid_directory_cancel_keeps_editor},
    {"invalid directory opens save-as", invalid_directory_uses_chosen_destination},
    {"default collisions never overwrite", default_save_collisions_do_not_overwrite},
    {"background failure receipt", failed_write_is_reported_after_acceptance},
    {"one terminal record and actual range", one_terminal_record_uses_actual_output_range},
    {"failure cancellation and unavailable log", terminal_failure_cancel_and_logging_failure}});
}
