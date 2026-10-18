#pragma once
#include "platform/macos/metal_edr_presenter.hpp"
#include "ports/magnifier_card_port.hpp"
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace hdrshot {
// A separate bounded worker keeps drawable/GPU waits off the GUI and
// independent of desktop redraws. Both presenters reuse the process Metal
// device/queue and the immutable native source; mutable textures are private to
// this worker.
class MacMagnifierCardPresenter final : public MagnifierCardPresenterPort {
public:
  using Backend = std::function<Result<MacMetalPresentReceipt, Error>(
      void *, MacMetalCardRequest, MacMetalEdrPresenter::RefreshCard)>;
  MacMagnifierCardPresenter(std::shared_ptr<MacMetalEdrPresenter>, void *,
                            MacMetalSurfaceRange);
  MacMagnifierCardPresenter(Backend, void *, MacMetalSurfaceRange);
  ~MacMagnifierCardPresenter() override;
  void present_card(MagnifierCardFrame, Completion) override;
  void invalidate_card(std::uint64_t generation) override;

private:
  struct Work {
    MagnifierCardFrame frame;
    Completion completion;
  };
  Result<MacMetalCardRequest, Error> request(const MagnifierCardFrame &) const;
  bool refresh(Work &, MacMetalCardRequest &);
  void run();
  Backend backend_;
  void *layer_{};
  MacMetalSurfaceRange range_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::optional<Work> pending_;
  std::uint64_t generation_{};
  bool stopping_{};
  std::thread worker_;
};
} // namespace hdrshot
