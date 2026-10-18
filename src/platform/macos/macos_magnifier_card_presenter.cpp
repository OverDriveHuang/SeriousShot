#include "platform/macos/macos_magnifier_card_presenter.hpp"
#include <algorithm>

namespace hdrshot {
namespace {
Error cancelled() {
  return {ErrorCode::operation_cancelled,
          "MacMagnifierCard",
          Retryability::never,
          {}};
}
Error invalid(const char *reason) {
  return {ErrorCode::invalid_input,
          "MacMagnifierCard",
          Retryability::never,
          {{"reason", reason}}};
}
void cancel(std::optional<MacMagnifierCardPresenter::Completion> completion) {
  if (completion)
    (*completion)(Result<std::uint64_t, Error>::failure(cancelled()));
}
} // namespace
MacMagnifierCardPresenter::MacMagnifierCardPresenter(
    std::shared_ptr<MacMetalEdrPresenter> presenter, void *layer,
    MacMetalSurfaceRange range)
    : MacMagnifierCardPresenter(
          [presenter = std::move(presenter)](
              void *target, MacMetalCardRequest card,
              MacMetalEdrPresenter::RefreshCard refresh) {
            return presenter->present_card_to_layer(target, std::move(card),
                                                    std::move(refresh));
          },
          layer, range) {}
MacMagnifierCardPresenter::MacMagnifierCardPresenter(Backend backend,
                                                     void *layer,
                                                     MacMetalSurfaceRange range)
    : backend_(std::move(backend)), layer_(layer), range_(range),
      worker_([this] { run(); }) {}
MacMagnifierCardPresenter::~MacMagnifierCardPresenter() {
  std::optional<Completion> abandoned;
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    if (pending_) {
      abandoned = std::move(pending_->completion);
      pending_.reset();
    }
  }
  cancel(std::move(abandoned));
  changed_.notify_one();
  if (worker_.joinable())
    worker_.join();
}
void MacMagnifierCardPresenter::present_card(MagnifierCardFrame frame,
                                             Completion completion) {
  std::optional<Completion> displaced;
  bool rejected;
  {
    std::lock_guard lock(mutex_);
    rejected = stopping_ || frame.generation < generation_;
    if (!rejected) {
      generation_ = frame.generation;
      if (pending_)
        displaced = std::move(pending_->completion);
      pending_ = Work{std::move(frame), std::move(completion)};
    }
  }
  if (rejected)
    completion(Result<std::uint64_t, Error>::failure(cancelled()));
  cancel(std::move(displaced));
  changed_.notify_one();
}
void MacMagnifierCardPresenter::invalidate_card(std::uint64_t generation) {
  std::optional<Completion> abandoned;
  {
    std::lock_guard lock(mutex_);
    generation_ = std::max(generation_, generation);
    if (pending_ && pending_->frame.generation < generation_) {
      abandoned = std::move(pending_->completion);
      pending_.reset();
    }
  }
  cancel(std::move(abandoned));
}
Result<MacMetalCardRequest, Error>
MacMagnifierCardPresenter::request(const MagnifierCardFrame &frame) const {
  using Output = Result<MacMetalCardRequest, Error>;
  if (!frame.source ||
      frame.display_generation != frame.source->display_generation)
    return Output::failure(invalid("missing_or_stale_source"));
  const auto &segments = frame.source->canonical_segments;
  const auto segment =
      std::find_if(segments.begin(), segments.end(), [&](const auto &s) {
        return s.display_id == frame.display_id;
      });
  if (segment == segments.end())
    return Output::failure(invalid("missing_display"));
  if (segment->encoding.primaries != ColorPrimaries::display_p3 ||
      (segment->encoding.transfer != TransferFunction::linear &&
       segment->encoding.transfer != TransferFunction::extended_srgb) ||
      (segment->pixel_format != PixelFormat::rgba16_float &&
       segment->pixel_format != PixelFormat::rgba32_float))
    return Output::failure(invalid("invalid_source_encoding"));
  MacMetalOverlayRequest source;
  source.width_px = std::size_t(segment->size_px.width);
  source.height_px = std::size_t(segment->size_px.height);
  if (!segment->rgba_half.empty())
    source.rgba_half_extended_p3 = {frame.source, &segment->rgba_half};
  if (!segment->rgba_float.empty())
    source.rgba_float_linear_p3 = {frame.source, &segment->rgba_float};
  source.linear_source = segment->linear_source;
  source.source_frame_key = frame.source->frame_id.value;
  source.source_is_linear =
      segment->encoding.transfer == TransferFunction::linear;
  source.target_surface_range = range_;
  source.ui_white_edr = range_ == MacMetalSurfaceRange::edr ? 2.03F : 1.F;
  source.magnifier = frame.image;
  return Output::success({std::move(source), frame.size_px, frame.ui_rgba});
}
bool MacMagnifierCardPresenter::refresh(Work &work, MacMetalCardRequest &card) {
  std::optional<Completion> displaced;
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || work.frame.generation != generation_)
      return false;
    if (pending_) {
      const auto &next = pending_->frame;
      // The acquired drawable cannot be reused across a new session/source or
      // size. Leave incompatible work queued and cancel this stale frame.
      if (next.session_id != work.frame.session_id ||
          next.source != work.frame.source ||
          next.display_id != work.frame.display_id ||
          next.size_px != work.frame.size_px)
        return false;
      displaced = std::move(work.completion);
      work = std::move(*pending_);
      pending_.reset();
    }
  }
  cancel(std::move(displaced));
  auto latest = request(work.frame);
  if (!latest)
    return false;
  card = std::move(latest.value());
  return true;
}
void MacMagnifierCardPresenter::run() {
  for (;;) {
    std::optional<Work> work;
    {
      std::unique_lock lock(mutex_);
      changed_.wait(lock, [&] { return stopping_ || pending_.has_value(); });
      if (stopping_)
        return;
      work = std::move(pending_);
      pending_.reset();
    }
    auto card = request(work->frame);
    if (!card) {
      work->completion(Result<std::uint64_t, Error>::failure(card.error()));
      continue;
    }
    auto result = backend_(layer_, std::move(card.value()),
                           [&](auto &value) { return refresh(*work, value); });
    bool stale;
    {
      std::lock_guard lock(mutex_);
      stale = stopping_ || work->frame.generation != generation_;
    }
    if (stale)
      work->completion(Result<std::uint64_t, Error>::failure(cancelled()));
    else if (!result)
      work->completion(Result<std::uint64_t, Error>::failure(result.error()));
    else
      work->completion(
          Result<std::uint64_t, Error>::success(work->frame.revision));
  }
}
} // namespace hdrshot
