#include "application/headless_capture.hpp"
#include "test_support.hpp"

#include <limits>
#include <optional>
#include <utility>

namespace {
using namespace hdrshot;

auto parse(std::initializer_list<std::string_view> args) {
  return parse_capture_command(std::span(args.begin(), args.size()));
}
CaptureCommand capture_command() { return parse({"--capture"}).value(); }

DisplaySnapshotSet display_set() {
  return {5, {
      {DisplayId{7}, {0, 0, 100, 80}, 2, {200, 160}},
      {DisplayId{9}, {-120, 10, 120, 100}, 1.5, {180, 150}},
  }};
}

class Catalog final : public DisplayCatalogPort {
 public:
  bool hold{};
  bool fail{};
  Completion pending;
  void snapshot_displays(const SnapshotDisplaysRequest&, Completion done) override {
    if (hold) { pending = std::move(done); return; }
    if (fail) {
      done(Result<DisplaySnapshotSet, Error>::failure(
          {ErrorCode::permission_denied, "Catalog", Retryability::after_user_action, {}}));
    } else done(Result<DisplaySnapshotSet, Error>::success(display_set()));
  }
};

class Capture final : public CapturePort {
 public:
  int calls{}, cancellations{};
  bool hold{}, fail{}, wrong_target{}, wrong_generation{}, wrong_operation{}, wrong_pixels{};
  CaptureBatchRequest request;
  Completion pending;
  NativeFrameBatch frame() const {
    const auto id = request.targets[0];
    const auto d = id == DisplayId{7} ? display_set().displays[0] : display_set().displays[1];
    std::vector<std::uint16_t> values(
        static_cast<std::size_t>(d.capture_size_px.width * d.capture_size_px.height) * 4, 0x3800);
    for (std::size_t i = 3; i < values.size(); i += 4) values[i] = 0x3c00;
    if (wrong_pixels) values.pop_back();
    return {request.session_id,
        wrong_operation ? OperationId{99} : request.operation_id,
        request.display_generation + (wrong_generation ? 1 : 0),
        {{wrong_target ? DisplayId{77} : id, d.capture_size_px, PixelFormat::rgba16_float,
          {ColorPrimaries::display_p3, TransferFunction::extended_srgb, AlphaMode::straight, 0},
          std::move(values)}}};
  }
  void capture(const CaptureBatchRequest& r, Completion done) override {
    ++calls;
    request = r;
    if (hold) { pending = std::move(done); return; }
    if (fail) done(Result<NativeFrameBatch, Error>::failure(
        {ErrorCode::capture_failed, "Capture", Retryability::same_input, {}}));
    else done(Result<NativeFrameBatch, Error>::success(frame()));
  }
  void cancel(SessionId, OperationId) override { ++cancellations; }
};

void parser_modes_and_coordinates() {
  HDRSHOT_CHECK(parse({}).value().mode == CaptureCommandMode::gui);
  HDRSHOT_CHECK(parse({"--help"}).value().mode == CaptureCommandMode::help);
  HDRSHOT_CHECK(parse({"-h"}).value().mode == CaptureCommandMode::help);
  HDRSHOT_CHECK(parse({"--list-displays"}).value().mode == CaptureCommandMode::list_displays);
  HDRSHOT_CHECK(!capture_command().display_id);
  HDRSHOT_CHECK(!capture_command().desktop_rect_points);
  const auto command = parse({"--capture", "-100.5", "10", "-1", "70.5", "--display", "9"});
  HDRSHOT_CHECK(command.has_value());
  HDRSHOT_CHECK(command.value().display_id == DisplayId{9});
  HDRSHOT_CHECK_NEAR(command.value().desktop_rect_points->width, 99.5, 0);
  HDRSHOT_CHECK_NEAR(command.value().desktop_rect_points->height, 60.5, 0);
  HDRSHOT_CHECK(capture_command_help().find("primary display") != std::string_view::npos);
}

void parser_rejects_invalid_input() {
  for (const auto args : {
      std::initializer_list<std::string_view>{"--display", "7"},
      {"--capture", "--display"}, {"--capture", "--display", "0"},
      {"--capture", "--display", "-1"}, {"--capture", "--display", "1.0"},
      {"--capture", "--display", "18446744073709551616"},
      {"--capture", "--display", "1", "--display", "1"},
      {"--capture", "0", "0", "1"}, {"--capture", "0", "0", "1", "1", "2"},
      {"--capture", "NaN", "0", "1", "1"}, {"--capture", "0", "0", "inf", "1"},
      {"--capture", "0", "0", "0", "1"}, {"--capture", "1", "0", "0", "1"},
      {"--capture", "0", "2", "1", "1"}, {"--capture", "0", "0", "1px", "1"},
      {"--capture", "--format", "png"}, {"--list-displays", "--display", "7"},
      {"--capture", "-1e308", "0", "1e308", "1"}}) {
    HDRSHOT_CHECK(!parse(args));
  }
}

void target_primary_explicit_and_full_screen() {
  const auto first = resolve_capture_target(capture_command(), DisplayId{7}, display_set());
  HDRSHOT_CHECK(first && first.value().display.id == DisplayId{7});
  HDRSHOT_CHECK((first.value().selection_px == PixelRect{0, 0, 200, 160}));
  auto command = capture_command();
  command.display_id = DisplayId{9};
  const auto second = resolve_capture_target(command, DisplayId{7}, display_set());
  HDRSHOT_CHECK(second && second.value().display.id == DisplayId{9});
  HDRSHOT_CHECK((second.value().selection_px == PixelRect{0, 0, 180, 150}));
  command.display_id = DisplayId{77};
  HDRSHOT_CHECK(!resolve_capture_target(command, DisplayId{7}, display_set()));
  HDRSHOT_CHECK(!resolve_capture_target(capture_command(), DisplayId{0}, display_set()));
  const auto locked = resolve_capture_target(capture_command(), DisplayId{7}, {5, {}});
  HDRSHOT_CHECK(!locked && locked.error().code == ErrorCode::capture_failed);
}

void clip_desktop_rect_and_round_outwards() {
  auto command = parse({"--capture", "--display", "9", "-125", "9", "5", "200"}).value();
  auto target = resolve_capture_target(command, DisplayId{7}, display_set());
  HDRSHOT_CHECK(target.has_value());
  HDRSHOT_CHECK((target.value().selection_px == PixelRect{0, 0, 180, 150}));
  command = parse({"--capture", "--display", "9", "-119.6", "10.4", "-118.1", "11.9"}).value();
  target = resolve_capture_target(command, DisplayId{7}, display_set());
  HDRSHOT_CHECK(target.has_value());
  HDRSHOT_CHECK((target.value().selection_px == PixelRect{0, 0, 3, 3}));
  // Same desktop rectangle touches two screens: export only the chosen screen.
  command = parse({"--capture", "-10", "20", "10", "40"}).value();
  target = resolve_capture_target(command, DisplayId{7}, display_set());
  HDRSHOT_CHECK((target.value().selection_px == PixelRect{0, 40, 20, 40}));
  command.display_id = DisplayId{9};
  target = resolve_capture_target(command, DisplayId{7}, display_set());
  HDRSHOT_CHECK((target.value().selection_px == PixelRect{165, 15, 15, 30}));
}

void reject_empty_intersection_and_invalid_geometry() {
  auto command = parse({"--capture", "100", "0", "101", "1"}).value();
  HDRSHOT_CHECK(!resolve_capture_target(command, DisplayId{7}, display_set()));
  command = parse({"--capture", "-100", "0", "-1", "1"}).value();
  HDRSHOT_CHECK(!resolve_capture_target(command, DisplayId{7}, display_set()));
  command = capture_command();
  auto displays = display_set();
  displays.displays[0].point_pixel_scale = std::numeric_limits<double>::quiet_NaN();
  HDRSHOT_CHECK(!resolve_capture_target(command, DisplayId{7}, displays));
  command.desktop_rect_points = {0, 0, std::numeric_limits<double>::infinity(), 1};
  HDRSHOT_CHECK(!resolve_capture_target(command, DisplayId{7}, display_set()));
  command = parse({"--capture", "-1e200", "-1e200", "1e200", "1e200"}).value();
  HDRSHOT_CHECK((resolve_capture_target(command, DisplayId{7}, display_set()).value().selection_px ==
                 PixelRect{0, 0, 200, 160}));
}

void capture_freezes_settings_and_reuses_export() {
  auto catalog = std::make_shared<Catalog>();
  auto capture = std::make_shared<Capture>();
  HeadlessCapture session(catalog, capture);
  SettingsSnapshot settings;
  settings.default_save_folder = "/unused/SeriousShot";
  settings.save_format = SaveFormat::ultra_hdr_jpeg;
  settings.pq_diffuse_white = PqDiffuseWhite::nits_100;
  settings.hdr_pq_precision = HdrPqPrecision::bits_12;
  settings.ultra_hdr_jpeg_quality = UltraHdrJpegQuality::maximum;
  std::optional<Result<ExportSnapshot, Error>> result;
  capture->hold = true;
  session.begin(parse({"--capture", "--display", "9", "-119", "11", "-110", "20"}).value(),
      DisplayId{7}, settings, [&](auto value) { result = std::move(value); });
  settings.save_format = SaveFormat::png_display_p3_dual_range;
  capture->pending(Result<NativeFrameBatch, Error>::success(capture->frame()));
  HDRSHOT_CHECK(result && *result);
  auto snapshot = result->value();
  HDRSHOT_CHECK(snapshot.save_format == SaveFormat::ultra_hdr_jpeg);
  HDRSHOT_CHECK(snapshot.pq_diffuse_white == PqDiffuseWhite::nits_100);
  HDRSHOT_CHECK(snapshot.hdr_pq_precision == HdrPqPrecision::bits_12);
  HDRSHOT_CHECK(snapshot.ultra_hdr_jpeg_quality == UltraHdrJpegQuality::maximum);
  HDRSHOT_CHECK(snapshot.default_folder == "/unused/SeriousShot");
  HDRSHOT_CHECK(snapshot.target_display_id == DisplayId{9});
  HDRSHOT_CHECK((snapshot.selection.desktop_rect == PixelRect{1, 1, 14, 14}));
  HDRSHOT_CHECK(capture->request.exclude_own_windows);
  HDRSHOT_CHECK(capture->request.targets.size() == 1);
  snapshot.save_format = SaveFormat::png_display_p3_dual_range;
  auto png = ExportWorkflow::prepare(snapshot);
  HDRSHOT_CHECK(png.has_value());
  HDRSHOT_CHECK(png.value().artifact.mime_type == "image/png");
  const auto& bytes = png.value().artifact.bytes;
  HDRSHOT_CHECK(bytes.size() > 33 && bytes[0] == 137 && bytes[1] == 'P');
  HDRSHOT_CHECK(bytes[19] == 14 && bytes[23] == 14); // Actual cropped IHDR dimensions.
  HDRSHOT_CHECK(bytes[24] == 16 && bytes[25] == 2); // RGB16, never RGBA/8-bit.
}

void capture_rejects_failures_and_wrong_associations() {
  for (int fault = 0; fault != 7; ++fault) {
    auto catalog = std::make_shared<Catalog>();
    auto capture = std::make_shared<Capture>();
    catalog->fail = fault == 0;
    capture->fail = fault == 1;
    capture->wrong_target = fault == 2;
    capture->wrong_generation = fault == 3;
    capture->wrong_operation = fault == 4;
    capture->wrong_pixels = fault == 5;
    auto command = capture_command();
    if (fault == 6) command.display_id = DisplayId{77};
    HeadlessCapture session(catalog, capture);
    std::optional<Result<ExportSnapshot, Error>> result;
    session.begin(command, DisplayId{7}, {}, [&](auto r) { result = std::move(r); });
    HDRSHOT_CHECK(result && !*result);
    if (fault == 0 || fault == 6) HDRSHOT_CHECK(capture->calls == 0);
  }
}

void cancellation_ignores_late_catalog_and_capture() {
  auto catalog = std::make_shared<Catalog>();
  auto capture = std::make_shared<Capture>();
  HeadlessCapture session(catalog, capture);
  int completed = 0;
  auto cancelled = [&](auto r) { HDRSHOT_CHECK(!r); ++completed; };
  catalog->hold = true;
  session.begin(capture_command(), DisplayId{7}, {}, cancelled);
  session.cancel();
  catalog->pending(Result<DisplaySnapshotSet, Error>::success(display_set()));
  HDRSHOT_CHECK(completed == 1 && capture->calls == 0);
  catalog->hold = false;
  capture->hold = true;
  session.begin(capture_command(), DisplayId{7}, {}, cancelled);
  auto old_callback = capture->pending;
  const auto old_frame = capture->frame();
  // Reject overlapping begin without cancelling the accepted capture.
  session.begin(capture_command(), DisplayId{7}, {}, cancelled);
  session.cancel();
  capture->hold = false;
  session.begin(capture_command(), DisplayId{7}, {}, [&](auto r) {
    HDRSHOT_CHECK(r.has_value()); ++completed;
  });
  old_callback(Result<NativeFrameBatch, Error>::success(old_frame));
  HDRSHOT_CHECK(completed == 4 && capture->cancellations == 2);
}

void destruction_ignores_late_completion() {
  auto catalog = std::make_shared<Catalog>();
  auto capture = std::make_shared<Capture>();
  catalog->hold = true;
  int completed = 0;
  {
    HeadlessCapture session(catalog, capture);
    session.begin(capture_command(), DisplayId{7}, {}, [&](auto r) {
      HDRSHOT_CHECK(!r); ++completed;
    });
  }
  catalog->pending(Result<DisplaySnapshotSet, Error>::success(display_set()));
  HDRSHOT_CHECK(completed == 1 && capture->calls == 0);
}
}  // namespace

int main() {
  return hdrshot::test::run({
      {"CLI modes and finite desktop coordinates", parser_modes_and_coordinates},
      {"Malformed commands do not fall through to GUI", parser_rejects_invalid_input},
      {"Default primary and explicit display full screen", target_primary_explicit_and_full_screen},
      {"Clip per display and round to physical pixels", clip_desktop_rect_and_round_outwards},
      {"Empty intersection and invalid geometry rejected", reject_empty_intersection_and_invalid_geometry},
      {"Immutable settings and real RGB16 export", capture_freezes_settings_and_reuses_export},
      {"Capture errors and association validation", capture_rejects_failures_and_wrong_associations},
      {"Cancellation and late/overlapping operations", cancellation_ignores_late_catalog_and_capture},
      {"Destroyed command cannot publish later", destruction_ignores_late_completion},
  });
}
