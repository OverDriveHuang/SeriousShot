#include "platform/macos/macos_preview_presenter_port.hpp"
#include "test_support.hpp"
#include "platform/macos/macos_magnifier_card_presenter.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace {

using namespace hdrshot;

std::uint16_t float_to_half_bits(const float value) {
  const _Float16 half = static_cast<_Float16>(value);
  std::uint16_t bits{};
  std::memcpy(&bits, &half, sizeof(bits));
  return bits;
}

FrozenDesktopRef frozen_frame() {
  return std::make_shared<const FrozenDesktop>(FrozenDesktop{
      FrameId{7},
      3,
      LogicalRect{0, 0, 2, 2},
      {CanonicalFrameSegment{
          DisplayId{9},
          LogicalRect{0, 0, 2, 2},
          1.0,
          PixelSize{2, 2},
          PixelFormat::rgba16_float,
          ColorEncoding{
              ColorPrimaries::display_p3,
              TransferFunction::extended_srgb,
              AlphaMode::straight,
              0.0,
          },
          DisplayDynamicRange::sdr,
          std::vector<std::uint16_t>(16, float_to_half_bits(0.5F)),
      }},
  });
}

PresentPreviewRequest request(
    const FrozenDesktopRef& frame,
    const std::uint64_t revision) {
  return PresentPreviewRequest{
      OperationId{revision},
      PreviewModel{
          SessionId{1},
          frame,
          SelectionSnapshot{revision, PixelRect{0, 0, 2, 2}},
          AnnotationDocumentSnapshot{},
          OverlayStyle{},
          frame->display_generation,
      },
  };
}

void single_inflight_keeps_only_latest_pending_revision() {
  std::mutex mutex;
  std::condition_variable changed;
  bool first_entered = false;
  bool release_first = false;
  std::size_t backend_calls = 0;
  std::size_t callbacks = 0;
  std::size_t cancelled = 0;
  std::vector<std::uint64_t> successes;
  std::vector<MacMetalSurfaceRange> surface_ranges;

  MacPreviewPresenterPort port(
      [&](void*, const MacMetalOverlayRequest& render_request)
          -> Result<MacMetalPresentReceipt, Error> {
        std::unique_lock lock(mutex);
        ++backend_calls;
        surface_ranges.push_back(render_request.target_surface_range);
        if (backend_calls == 1) {
          first_entered = true;
          changed.notify_all();
          changed.wait(lock, [&] { return release_first; });
        }
        return Result<MacMetalPresentReceipt, Error>::success(MacMetalPresentReceipt{
            render_request.width_px,
            render_request.height_px,
            render_request.width_px,
            render_request.height_px,
            "fake",
            "fake",
            100.0,
        });
      },
      reinterpret_cast<void*>(1),
      1.0);

  const auto frame = frozen_frame();
  const auto completion = [&](Result<PresentReceipt, Error> result) {
    const std::scoped_lock lock(mutex);
    ++callbacks;
    if (result) {
      successes.push_back(result.value().selection_revision);
    } else if (result.error().code == ErrorCode::operation_cancelled) {
      ++cancelled;
    }
    changed.notify_all();
  };

  port.present(request(frame, 1), completion);
  {
    std::unique_lock lock(mutex);
    HDRSHOT_CHECK(changed.wait_for(
        lock, std::chrono::seconds(2), [&] { return first_entered; }));
  }
  for (std::uint64_t revision = 2; revision <= 100; ++revision) {
    port.present(request(frame, revision), completion);
  }
  {
    const std::scoped_lock lock(mutex);
    release_first = true;
  }
  changed.notify_all();
  {
    std::unique_lock lock(mutex);
    HDRSHOT_CHECK(changed.wait_for(
        lock, std::chrono::seconds(3), [&] { return callbacks == 100; }));
  }

  const std::scoped_lock lock(mutex);
  HDRSHOT_CHECK(backend_calls == 2);
  HDRSHOT_CHECK(cancelled == 98);
  HDRSHOT_CHECK(successes == (std::vector<std::uint64_t>{1, 100}));
  HDRSHOT_CHECK(surface_ranges.size() == 2);
  HDRSHOT_CHECK(surface_ranges[0] == MacMetalSurfaceRange::sdr);
  HDRSHOT_CHECK(surface_ranges[1] == MacMetalSurfaceRange::sdr);
}

void explicit_display_id_selects_the_matching_segment() {
  auto mutable_frame = *frozen_frame();
  auto second = mutable_frame.canonical_segments.front();
  second.display_id = DisplayId{10};
  second.size_px = PixelSize{3, 1};
  second.rgba_half = std::vector<std::uint16_t>(12, float_to_half_bits(0.25F));
  mutable_frame.canonical_segments.push_back(std::move(second));
  auto frame = std::make_shared<const FrozenDesktop>(std::move(mutable_frame));
  std::mutex mutex;
  std::condition_variable changed;
  std::optional<std::pair<std::size_t, std::size_t>> rendered_size;
  std::optional<Result<PresentReceipt, Error>> completion;
  MacPreviewPresenterPort port(
      [&](void*, const MacMetalOverlayRequest& render_request)
          -> Result<MacMetalPresentReceipt, Error> {
        {
          const std::scoped_lock lock(mutex);
          rendered_size = {render_request.width_px, render_request.height_px};
        }
        return Result<MacMetalPresentReceipt, Error>::success(MacMetalPresentReceipt{
            render_request.width_px,
            render_request.height_px,
            render_request.width_px,
            render_request.height_px,
            "fake",
            "fake",
            100.0,
        });
      },
      reinterpret_cast<void*>(1),
      1.0);
  auto targeted = request(frame, 8);
  targeted.target_display_id = DisplayId{10};
  port.present(targeted, [&](auto result) {
    const std::scoped_lock lock(mutex);
    completion = std::move(result);
    changed.notify_all();
  });
  {
    std::unique_lock lock(mutex);
    HDRSHOT_CHECK(changed.wait_for(
        lock, std::chrono::seconds(2), [&] { return completion.has_value(); }));
    HDRSHOT_CHECK(completion->has_value());
    HDRSHOT_CHECK(rendered_size == (std::pair<std::size_t, std::size_t>{3U, 1U}));
  }
}

void empty_selection_reaches_the_platform_presenter() {
  std::mutex mutex;
  std::condition_variable changed;
  bool received_empty_selection = false;
  std::optional<Result<PresentReceipt, Error>> completion;
  MacPreviewPresenterPort port(
      [&](void*, const MacMetalOverlayRequest& render_request)
          -> Result<MacMetalPresentReceipt, Error> {
        received_empty_selection = render_request.selection_px.empty();
        return Result<MacMetalPresentReceipt, Error>::success(MacMetalPresentReceipt{
            render_request.width_px,
            render_request.height_px,
            render_request.width_px,
            render_request.height_px,
            "fake",
            "fake",
            100.0,
        });
      },
      reinterpret_cast<void*>(1),
      1.0);
  auto initial = request(frozen_frame(), 1);
  initial.model.selection = SelectionSnapshot{1, {}};
  initial.target_display_id = DisplayId{9};
  port.present(initial, [&](auto result) {
    const std::scoped_lock lock(mutex);
    completion = std::move(result);
    changed.notify_all();
  });
  {
    std::unique_lock lock(mutex);
    HDRSHOT_CHECK(changed.wait_for(
        lock, std::chrono::seconds(2), [&] { return completion.has_value(); }));
    HDRSHOT_CHECK(completion->has_value());
    HDRSHOT_CHECK(received_empty_selection);
  }
}

void candidate_uses_frozen_pixels_without_committing_selection() {
  const auto frame = frozen_frame();
  std::mutex mutex;
  std::condition_variable changed;
  bool correct_rect_and_storage = false;
  std::optional<Result<PresentReceipt, Error>> completion;
  MacPreviewPresenterPort port(
      [&](void*, const MacMetalOverlayRequest& render_request) {
        correct_rect_and_storage = render_request.selection_px == (PixelRect{0, 0, 1, 2}) &&
            render_request.rgba_half_extended_p3.get() == &frame->canonical_segments[0].rgba_half;
        return Result<MacMetalPresentReceipt, Error>::success(MacMetalPresentReceipt{
            2, 2, 2, 2, "fake", "fake", 100.0});
      }, reinterpret_cast<void*>(1), 1.0);
  auto initial = request(frame, 1);
  initial.model.selection = SelectionSnapshot{1, {}};
  initial.model.initial_highlight = {PreviewHighlightKind::window_candidate, {0, 0, 1, 2}, 42};
  port.present(initial, [&](auto result) {
    const std::scoped_lock lock(mutex);
    completion = std::move(result);
    changed.notify_all();
  });
  std::unique_lock lock(mutex);
  HDRSHOT_CHECK(changed.wait_for(lock, std::chrono::seconds(2), [&] { return completion.has_value(); }));
  HDRSHOT_CHECK(completion->has_value());
  HDRSHOT_CHECK(correct_rect_and_storage);
  HDRSHOT_CHECK(completion->value().selection_revision == 1);
  HDRSHOT_CHECK(completion->value().preview_revision == 42);
  HDRSHOT_CHECK(initial.model.selection.desktop_rect.empty());
}

void card_late_refresh_coalesces_and_invalidates() {
  std::mutex mutex;std::condition_variable changed;
  bool entered=false,release=false;int calls=0,callbacks=0,cancellations=0;
  std::vector<std::uint64_t> successes,drawn;
  MacMagnifierCardPresenter port([&](void*,MacMetalCardRequest card,MacMetalEdrPresenter::RefreshCard refresh) {
    {std::unique_lock lock(mutex);++calls;entered=true;changed.notify_all();
      changed.wait_for(lock,std::chrono::seconds(2),[&]{return release;});}
    if(!refresh(card)) return Result<MacMetalPresentReceipt,Error>::failure(
        {ErrorCode::operation_cancelled,"fake",Retryability::never,{}});
    {std::lock_guard lock(mutex);drawn.push_back(card.source.magnifier.revision);}
    return Result<MacMetalPresentReceipt,Error>::success({});
  },reinterpret_cast<void*>(1),MacMetalSurfaceRange::edr);
  const auto frame=frozen_frame();
  auto make=[&](std::uint64_t revision,std::uint64_t generation=1) {
    return MagnifierCardFrame{SessionId{1},frame,DisplayId{9},frame->display_generation,
        generation,revision,{12,24},{true,{1,1},{0,4,12,12},4,revision},std::vector<std::uint8_t>(12*24*4)};
  };
  auto done=[&](Result<std::uint64_t,Error> result) {
    std::lock_guard lock(mutex);++callbacks;
    if(result)successes.push_back(result.value());else ++cancellations;changed.notify_all();
  };
  port.present_card(make(1),done);
  {std::unique_lock lock(mutex);HDRSHOT_CHECK(changed.wait_for(lock,std::chrono::seconds(2),[&]{return entered;}));}
  for(std::uint64_t i=2;i<=50;++i)port.present_card(make(i),done);
  {std::lock_guard lock(mutex);release=true;}changed.notify_all();
  {std::unique_lock lock(mutex);HDRSHOT_CHECK(changed.wait_for(lock,std::chrono::seconds(2),[&]{return callbacks==50;}));
    HDRSHOT_CHECK(calls==1 && cancellations==49 && successes==std::vector<std::uint64_t>{50});
    HDRSHOT_CHECK(drawn==std::vector<std::uint64_t>{50});entered=false;release=false;}
  port.present_card(make(51),done);
  {std::unique_lock lock(mutex);HDRSHOT_CHECK(changed.wait_for(lock,std::chrono::seconds(2),[&]{return entered;}));}
  port.invalidate_card(2);port.present_card(make(52),done);
  {std::lock_guard lock(mutex);release=true;}changed.notify_all();
  {std::unique_lock lock(mutex);HDRSHOT_CHECK(changed.wait_for(lock,std::chrono::seconds(2),[&]{return callbacks==52;}));
    HDRSHOT_CHECK(cancellations==51 && drawn.size()==1);}
  port.present_card(make(53,2),done);
  {std::unique_lock lock(mutex);HDRSHOT_CHECK(changed.wait_for(lock,std::chrono::seconds(2),[&]{return callbacks==53;}));
    HDRSHOT_CHECK(successes.back()==53);}
}
void card_close_cancels_inflight_and_pending() {
  std::mutex mutex;std::condition_variable changed;bool entered=false;int callbacks=0,successes=0;
  auto frame=frozen_frame();
  {
    MacMagnifierCardPresenter port([&](void*,MacMetalCardRequest card,MacMetalEdrPresenter::RefreshCard refresh) {
      {std::lock_guard lock(mutex);entered=true;changed.notify_all();}
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      if(!refresh(card))return Result<MacMetalPresentReceipt,Error>::failure({ErrorCode::operation_cancelled,"fake",Retryability::never,{}});
      return Result<MacMetalPresentReceipt,Error>::success({});
    },reinterpret_cast<void*>(1),MacMetalSurfaceRange::edr);
    auto done=[&](auto result){std::lock_guard lock(mutex);++callbacks;if(result)++successes;};
    MagnifierCardFrame card{SessionId{1},frame,DisplayId{9},frame->display_generation,1,1,{12,24},
        {true,{1,1},{0,4,12,12},4,1},std::vector<std::uint8_t>(12*24*4)};
    port.present_card(card,done);
    {std::unique_lock lock(mutex);HDRSHOT_CHECK(changed.wait_for(lock,std::chrono::seconds(2),[&]{return entered;}));}
    port.present_card(card,done);
  }
  HDRSHOT_CHECK(callbacks==2 && successes==0);
}

}  // namespace

int main() {
  return hdrshot::test::run({
      {"Card late input and invalidation",card_late_refresh_coalesces_and_invalidates},
      {"Card close cancels work",card_close_cancels_inflight_and_pending},
      {"P3 latest-only scheduling", single_inflight_keeps_only_latest_pending_revision},
      {"P3 selects explicit display segment", explicit_display_id_selects_the_matching_segment},
      {"P3 forwards an empty initial selection", empty_selection_reaches_the_platform_presenter},
      {"P3 forwards candidate without copying frozen pixels", candidate_uses_frozen_pixels_without_committing_selection},
  });
}
