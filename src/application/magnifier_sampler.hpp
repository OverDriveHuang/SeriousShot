#pragma once
#include "core/frame.hpp"
#include "domain/capture/magnifier_readout.hpp"
#include "domain/color/extended_p3_mapper.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
namespace hdrshot {
struct MagnifierSampleRequest {
  FrozenDesktopRef source;
  DisplayId display{};
  PixelPoint point{};
  std::uint64_t revision{};
  double reference_white_nits{203};
};
struct MagnifierSampleResult {
  PixelPoint point{};
  std::uint64_t revision{};
  MagnifierReadout readout{};
};
// One in-flight tiny readback plus one replaceable pending request. No GUI join
// or wait. A detached worker owns its state and source until completion; close
// invalidates delivery and drops the pending source immediately.
class MagnifierSampler {
public:
  using Completion = std::function<void(MagnifierSampleResult)>;
  struct Counters {
    std::uint64_t requested{}, readbacks{}, delivered{};
    unsigned in_flight{}, pending{};
  };
  MagnifierSampler() : state_(std::make_shared<State>()) {}
  ~MagnifierSampler() { close(); }
  void submit(MagnifierSampleRequest request, Completion completion) {
    const auto state = state_;
    bool launch = false;
    {
      std::lock_guard lock(state->mutex);
      if (state->closed)
        return;
      ++state->requested;
      ++state->epoch;
      state->pending =
          Job{std::move(request), std::move(completion), state->epoch};
      if (!state->running) {
        state->running = true;
        launch = true;
      }
    }
    state->ready.notify_one();
    if (launch)
      std::thread([state] { run(state); }).detach();
  }
  void invalidate() {
    std::lock_guard lock(state_->mutex);
    ++state_->epoch;
    state_->pending.reset();
  }
  void close() {
    {
      std::lock_guard lock(state_->mutex);
      state_->closed = true;
      ++state_->epoch;
      state_->pending.reset();
    }
    state_->ready.notify_one();
  }
  Counters counters() const {
    std::lock_guard lock(state_->mutex);
    return {state_->requested, state_->readbacks, state_->delivered,
            state_->active ? 1U : 0U, state_->pending ? 1U : 0U};
  }

private:
  struct Job {
    MagnifierSampleRequest request;
    Completion completion;
    std::uint64_t epoch{};
  };
  struct State {
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<Job> pending;
    bool running{}, active{}, closed{};
    std::uint64_t epoch{}, requested{}, readbacks{}, delivered{};
  };
  static void run(std::shared_ptr<State> state) {
    for (;;) {
      std::optional<Job> job;
      {
        std::unique_lock lock(state->mutex);
        state->ready.wait(
            lock, [&] { return state->closed || state->pending.has_value(); });
        if (state->closed) {
          state->running = false;
          return;
        }
        job = std::move(state->pending);
        state->pending.reset();
        state->active = true;
        ++state->readbacks;
      }
      const auto &req = job->request;
      MagnifierSampleResult out{req.point, req.revision};
      if (req.source) {
        const auto it = std::find_if(
            req.source->canonical_segments.begin(),
            req.source->canonical_segments.end(),
            [&](const auto &s) { return s.display_id == req.display; });
        if (it != req.source->canonical_segments.end() && req.point.x >= 0 &&
            req.point.y >= 0 && req.point.x < it->size_px.width &&
            req.point.y < it->size_px.height) {
          analysis::Rgb rgb{};
          bool valid = true;
          if (it->linear_source) {
            auto pixels = it->linear_source->read_region(
                {req.point.x, req.point.y, 1, 1});
            valid = bool(pixels) && pixels.value().size() == 4;
            if (valid)
              for (int c = 0; c < 3; ++c)
                rgb[c] = pixels.value()[c];
          } else {
            const auto offset =
                (std::size_t(req.point.y) * it->size_px.width + req.point.x) *
                4;
            const auto needed = offset + 4;
            valid = (!it->rgba_float.empty() ? it->rgba_float.size()
                                             : it->rgba_half.size()) >= needed;
            for (int c = 0; valid && c < 3; ++c) {
              if (!it->rgba_float.empty())
                rgb[c] = it->rgba_float[offset + c];
              else {
                auto decoded = ExtendedP3Mapper::decode_binary16(
                    it->rgba_half[offset + c]);
                if (!decoded) {
                  valid = false;
                  break;
                }
                rgb[c] = decoded.value();
              }
            }
            if (it->encoding.transfer == TransferFunction::extended_srgb)
              for (auto &c : rgb)
                c = ExtendedP3Mapper::inverse_extended_srgb(float(c));
          }
          for (double c : rgb)
            valid = valid && std::isfinite(c);
          if (valid)
            out.readout = magnifier_readout(rgb, req.reference_white_nits,
                                            it->capture_sdr_tolerance,
                                            it->capture_frozen_sdr &&
                                                it->display_dynamic_range ==
                                                    DisplayDynamicRange::sdr);
        }
      }
      bool deliver = false;
      {
        std::lock_guard lock(state->mutex);
        state->active = false;
        deliver = !state->closed && state->epoch == job->epoch;
        if (deliver)
          ++state->delivered;
      }
      if (deliver && job->completion)
        job->completion(std::move(out));
    }
  }
  std::shared_ptr<State> state_;
};
} // namespace hdrshot
