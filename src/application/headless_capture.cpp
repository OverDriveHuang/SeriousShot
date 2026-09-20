#include "application/headless_capture.hpp"

#include "domain/frame/frame_pipeline.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <locale>
#include <mutex>
#include <sstream>
#include <utility>

namespace hdrshot {
namespace {
Error command_error(const char* reason, ErrorCode code = ErrorCode::invalid_input) {
  return {code, "HeadlessCapture", Retryability::never, {{"reason", reason}}};
}

bool parse_number(std::string_view text, double& value) {
  std::istringstream input{std::string(text)};
  input.imbue(std::locale::classic());
  input >> std::noskipws >> value;
  return input && input.eof() && std::isfinite(value);
}
}  // namespace

std::string_view capture_command_help() {
  return
      "SeriousShot\n"
      "  SeriousShot                         Start the resident app/settings\n"
      "  SeriousShot --list-displays          List IDs, primary and bounds\n"
      "  SeriousShot --capture [--display ID] [x1 y1 x2 y2]\n"
      "  SeriousShot --help\n\n"
      "Coordinates are desktop logical points (right/down positive).\n"
      "No display ID: primary display. No coordinates: that entire display.\n"
      "Out-of-bounds rectangles are clipped to that display; empty ones fail.\n"
      "Output keeps physical resolution and reads the app's saved settings.\n"
      "Success: full saved path on stdout, exit 0. Usage: exit 2. Failure: exit 1.\n";
}

Result<CaptureCommand, Error> parse_capture_command(
    const std::span<const std::string_view> arguments) {
  const auto fail = [](const char* reason) {
    return Result<CaptureCommand, Error>::failure(command_error(reason));
  };
  CaptureCommand command;
  if (arguments.empty()) return Result<CaptureCommand, Error>::success(command);
  if (arguments.size() == 1 && (arguments[0] == "--help" || arguments[0] == "-h")) {
    command.mode = CaptureCommandMode::help;
    return Result<CaptureCommand, Error>::success(command);
  }
  if (arguments.size() == 1 && arguments[0] == "--list-displays") {
    command.mode = CaptureCommandMode::list_displays;
    return Result<CaptureCommand, Error>::success(command);
  }
  if (arguments[0] != "--capture") return fail("expected_capture_or_list_displays");
  command.mode = CaptureCommandMode::capture;
  std::array<double, 4> coordinates{};
  std::size_t count = 0;
  for (std::size_t i = 1; i < arguments.size(); ++i) {
    if (arguments[i] == "--display") {
      if (command.display_id || ++i >= arguments.size()) return fail("invalid_display_argument");
      std::uint64_t id{};
      const auto text = arguments[i];
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || id == 0)
        return fail("invalid_display_id");
      command.display_id = DisplayId{id};
    } else {
      if (count == coordinates.size() || !parse_number(arguments[i], coordinates[count]))
        return fail("expected_four_finite_coordinates");
      ++count;
    }
  }
  if (count != 0) {
    if (count != 4) return fail("expected_four_finite_coordinates");
    const double width = coordinates[2] - coordinates[0];
    const double height = coordinates[3] - coordinates[1];
    if (!(width > 0 && height > 0) || !std::isfinite(width) || !std::isfinite(height))
      return fail("empty_or_reversed_rectangle");
    command.desktop_rect_points = LogicalRect{coordinates[0], coordinates[1], width, height};
  }
  return Result<CaptureCommand, Error>::success(command);
}

Result<HeadlessCaptureTarget, Error> resolve_capture_target(
    const CaptureCommand& command, const DisplayId primary_display,
    const DisplaySnapshotSet& displays) {
  const auto fail = [](const char* reason) {
    return Result<HeadlessCaptureTarget, Error>::failure(command_error(reason));
  };
  if (command.mode != CaptureCommandMode::capture || displays.generation == 0)
    return fail("invalid_capture_request");
  if (displays.displays.empty())
    return Result<HeadlessCaptureTarget, Error>::failure(
        command_error("no_capturable_display_session_may_be_locked", ErrorCode::capture_failed));
  const auto id = command.display_id.value_or(primary_display);
  const auto found = std::find_if(displays.displays.begin(), displays.displays.end(),
      [id](const auto& display) { return display.id == id; });
  if (id.value == 0 || found == displays.displays.end()) return fail("display_not_found");
  const auto& d = *found;
  const auto b = d.desktop_frame_points;
  const double right = b.x + b.width, bottom = b.y + b.height;
  if (!std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(right) ||
      !std::isfinite(bottom) || !std::isfinite(d.point_pixel_scale) ||
      b.width <= 0 || b.height <= 0 || d.point_pixel_scale <= 0 ||
      d.capture_size_px.width <= 0 || d.capture_size_px.height <= 0)
    return fail("invalid_display_geometry");
  PixelRect pixels{0, 0, d.capture_size_px.width, d.capture_size_px.height};
  if (command.desktop_rect_points) {
    const auto r = *command.desktop_rect_points;
    // Reuse the same per-display intersection/physical-pixel rounding used by
    // automatic window selection, including rounded native capture dimensions.
    const auto mapped = map_window_bounds(
        {r.x, r.y, r.width, r.height}, {b.x, b.y, b.width, b.height}, d.capture_size_px);
    if (!mapped) return fail("invalid_rectangle_or_no_display_intersection");
    pixels = *mapped;
  }
  return Result<HeadlessCaptureTarget, Error>::success({d, pixels});
}

struct HeadlessCapture::State : std::enable_shared_from_this<State> {
  struct Run {
    SessionId session;
    OperationId operation;
    CaptureCommand command;
    DisplayId primary;
    SettingsSnapshot settings;
    Completion completion;
    bool capture_started{};
  };
  std::shared_ptr<DisplayCatalogPort> catalog;
  std::shared_ptr<CapturePort> capture;
  std::mutex mutex;
  std::shared_ptr<Run> active;
  std::uint64_t sequence{};

  void finish(const std::shared_ptr<Run>& run, Result<ExportSnapshot, Error> result) {
    Completion completion;
    {
      const std::scoped_lock lock(mutex);
      if (active != run) return;
      completion = std::move(run->completion);
      active.reset();
    }
    completion(std::move(result));
  }

  void cancel() {
    std::shared_ptr<Run> run;
    Completion completion;
    {
      const std::scoped_lock lock(mutex);
      run = std::exchange(active, {});
      if (run) completion = std::move(run->completion);
    }
    if (!run) return;
    if (capture) capture->cancel(run->session, run->operation);
    completion(Result<ExportSnapshot, Error>::failure(
        command_error("capture_cancelled", ErrorCode::operation_cancelled)));
  }

  void displays_ready(const std::shared_ptr<Run>& run, Result<DisplaySnapshotSet, Error> result) {
    {
      const std::scoped_lock lock(mutex);
      if (active != run || run->capture_started) return;
      run->capture_started = true;
    }
    if (!result) { finish(run, Result<ExportSnapshot, Error>::failure(result.error())); return; }
    auto displays = std::move(result.value());
    const auto target = resolve_capture_target(run->command, run->primary, displays);
    if (!target) { finish(run, Result<ExportSnapshot, Error>::failure(target.error())); return; }
    const auto selected = target.value();
    const CaptureBatchRequest request{run->session, run->operation, displays.generation,
                                     {selected.display.id}, PixelFormat::rgba16_float, true};
    capture->capture(request,
        [weak = weak_from_this(), run, selected, displays = std::move(displays)](
            Result<NativeFrameBatch, Error> captured) {
      const auto self = weak.lock();
      if (!self) return;
      {
        const std::scoped_lock lock(self->mutex);
        if (self->active != run) return;
      }
      if (!captured) {
        self->finish(run, Result<ExportSnapshot, Error>::failure(captured.error())); return;
      }
      auto batch = std::move(captured.value());
      if (batch.session_id != run->session || batch.operation_id != run->operation ||
          batch.display_generation != displays.generation || batch.frames.size() != 1 ||
          batch.frames[0].display_id != selected.display.id) {
        self->finish(run, Result<ExportSnapshot, Error>::failure(
            command_error("capture_association_mismatch", ErrorCode::state_inconsistent))); return;
      }
      auto segment = SourceColorInterpreter::interpret(std::move(batch.frames[0]), selected.display);
      if (!segment) {
        self->finish(run, Result<ExportSnapshot, Error>::failure(segment.error())); return;
      }
      std::vector<CanonicalFrameSegment> segments;
      segments.push_back(std::move(segment.value()));
      auto frozen = DisplayFrameAssembler::assemble(
          FrameId{run->session.value}, displays.generation, displays, std::move(segments));
      if (!frozen) {
        self->finish(run, Result<ExportSnapshot, Error>::failure(frozen.error())); return;
      }
      ExportSnapshot snapshot;
      snapshot.session_id = run->session;
      snapshot.operation_id = run->operation;
      snapshot.frozen_desktop = std::make_shared<const FrozenDesktop>(std::move(frozen.value()));
      snapshot.selection = {SelectionRevision{1}, selected.selection_px};
      snapshot.annotations = AnnotationDocument::empty().snapshot();
      snapshot.target_display_id = selected.display.id;
      snapshot.default_folder = run->settings.default_save_folder;
      snapshot.save_format = run->settings.save_format;
      snapshot.pq_diffuse_white = run->settings.pq_diffuse_white;
      snapshot.hdr_pq_precision = run->settings.hdr_pq_precision;
      snapshot.ultra_hdr_jpeg_quality = run->settings.ultra_hdr_jpeg_quality;
      self->finish(run, Result<ExportSnapshot, Error>::success(std::move(snapshot)));
    });
  }
};

HeadlessCapture::HeadlessCapture(std::shared_ptr<DisplayCatalogPort> catalog,
                               std::shared_ptr<CapturePort> capture)
    : state_(std::make_shared<State>()) {
  state_->catalog = std::move(catalog);
  state_->capture = std::move(capture);
}
HeadlessCapture::~HeadlessCapture() { state_->cancel(); }
void HeadlessCapture::cancel() { state_->cancel(); }

void HeadlessCapture::begin(CaptureCommand command, DisplayId primary,
                            SettingsSnapshot settings, Completion completion) {
  auto self = state_;
  if (!self->catalog || !self->capture) {
    completion(Result<ExportSnapshot, Error>::failure(command_error("missing_capture_ports"))); return;
  }
  std::shared_ptr<State::Run> run;
  {
    const std::scoped_lock lock(self->mutex);
    if (!self->active) {
      const auto sequence = ++self->sequence;
      run = std::make_shared<State::Run>(State::Run{
          SessionId{sequence}, OperationId{sequence}, std::move(command), primary,
          std::move(settings), std::move(completion)});
      self->active = run;
    }
  }
  if (!run) {
    completion(Result<ExportSnapshot, Error>::failure(
        command_error("capture_already_active", ErrorCode::precondition_failed))); return;
  }
  self->catalog->snapshot_displays({run->session, run->operation},
      [weak = std::weak_ptr<State>(self), run](Result<DisplaySnapshotSet, Error> result) {
    if (const auto state = weak.lock()) state->displays_ready(run, std::move(result));
  });
}
}  // namespace hdrshot
