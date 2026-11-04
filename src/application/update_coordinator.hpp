#pragma once
#include "ports/update/update_ports.hpp"
#include <memory>
namespace hdrshot::update {
class Coordinator {
public:
  Coordinator(Identity, Effects &);
  ~Coordinator() { *alive_ = false; }
  void trigger(std::int64_t now, bool cold = false, bool manual = false);
  void tick(std::int64_t now);
  void install();
  void later();
  void cancel();
  void manual_open_result(Result<bool, Error>);
  const View &view() const { return view_; }
  std::function<void(const View &)> changed;

private:
  void attempt();
  void download();
  void prompt();
  void publish();
  void set_detail(std::string text, DetailKind kind = DetailKind::literal);
  void set_detail(const Error &error);
  Identity identity_;
  Effects &effects_;
  Schedule schedule_;
  View view_;
  std::optional<Receipt> receipt_;
  std::uint64_t generation_{};
  bool active_{}, installing_{}, pending_question_{};
  bool round_checked_{}, round_prompt_consumed_{};
  std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
};
} // namespace hdrshot::update
