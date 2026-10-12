#include "application/update_coordinator.hpp"
namespace hdrshot::update {
Coordinator::Coordinator(Identity i, Effects &e) : identity_(std::move(i)), effects_(e) {}
void Coordinator::publish() {
  view_.generation = generation_;
  view_.busy = effects_.busy();
  if (changed)
    changed(view_);
}
void Coordinator::trigger(std::int64_t now, bool cold, bool manual) {
  if (active_ || installing_ || view_.phase == Phase::manual_pending)
    return;
  if (!manual && !schedule_.due(now, cold))
    return;
  ++generation_;
  view_.last_check = now;
  schedule_.begin(now);
  active_ = true;
  attempt();
}
void Coordinator::attempt() {
  view_.phase = Phase::checking;
  view_.attempts = schedule_.attempts();
  view_.detail.clear();
  publish();
  const auto token = generation_;
  effects_.check(identity_, [this, token, alive = alive_](auto checked) {
    if (!*alive || token != generation_ || !active_)
      return;
    if (!checked) {
      if (schedule_.retry(checked.error().retryability == Retryability::same_input)) {
        view_.phase = Phase::retry_wait;
        publish();
        effects_.delay(view_.attempts == 1 ? 10 : 30, [this, token, alive = alive_] {
          if (*alive && token == generation_ && active_)
            attempt();
        });
        return;
      }
      active_ = false;
      view_.phase = Phase::check_failed;
      const auto reason = checked.error().safe_context.find("reason");
      view_.detail = reason == checked.error().safe_context.end() ? to_string(checked.error().code)
                                                                  : reason->second;
      publish();
      return;
    }
    view_.last_success = view_.last_check;
    const auto c = checked.value().candidate;
    if (!c || c->version <= identity_.version) {
      active_ = false;
      view_.candidate.reset();
      view_.phase = Phase::idle;
      view_.detail = "已是最新版本";
      if (checked.value().incompatible &&
          checked.value().incompatible->version > identity_.version) {
        view_.candidate = checked.value().incompatible;
        view_.detail = "新版本 " + view_.candidate->version.text() + " 需要更高系统版本";
      }
      publish();
      return;
    }
    pending_question_ = false;
    view_.candidate = c;
    view_.prompted = effects_.was_prompted(c->key());
    download();
  });
}
void Coordinator::download() {
  ++generation_;
  const auto generation = generation_;
  view_.phase = Phase::cleaning;
  publish();
  effects_.acquire_and_download(
      *view_.candidate,
      [this, generation, alive = alive_](auto n, auto total) {
        if (!*alive)
          return;
        if (generation != generation_)
          return;
        view_.phase = Phase::downloading;
        view_.received = n;
        view_.total = total;
        publish();
      },
      [this, generation, alive = alive_](auto r) {
        if (!*alive)
          return;
        if (generation != generation_)
          return;
        active_ = false;
        if (!r) {
          view_.phase = r.error().code == ErrorCode::path_not_writable ? Phase::cleanup_blocked
                                                                       : Phase::download_failed;
          const auto reason = r.error().safe_context.find("reason");
          view_.detail =
              reason == r.error().safe_context.end() ? to_string(r.error().code) : reason->second;
          publish();
          return;
        }
        view_.error.reset();
        receipt_ = r.value();
        view_.phase = view_.prompted ? Phase::deferred : Phase::ready;
        view_.detail = "安装包已校验";
        publish();
        prompt();
      });
}
void Coordinator::prompt() {
  if (view_.phase != Phase::ready || effects_.busy())
    return;
  const auto token = generation_;
  if (view_.prompted) {
    if (!pending_question_)
      return;
    pending_question_ = false;
    effects_.ask_install(*view_.candidate, [this, token, alive = alive_](bool accepted) {
      if (!*alive || token != generation_)
        return;
      if (accepted)
        install();
      else
        later();
    });
    return;
  }
  // Durable receipt precedes the UI effect. A write error cannot repeat a shown question.
  view_.prompted = true;
  effects_.persist_prompt(view_.candidate->key(), [this, token, alive = alive_](auto saved) {
    if (!*alive || token != generation_)
      return;
    if (!saved) {
      view_.phase = Phase::deferred;
      view_.detail = "无法保存更新询问状态，可在设置中主动安装";
      publish();
      return;
    }
    pending_question_ = true;
    publish();
    prompt();
  });
}
void Coordinator::tick(std::int64_t now) {
  if (view_.busy != effects_.busy())
    publish();
  prompt();
  trigger(now);
}
void Coordinator::restore(Candidate c, Receipt r, bool pending) {
  if (active_ || installing_)
    return;
  view_.candidate = std::move(c);
  receipt_ = std::move(r);
  view_.prompted = effects_.was_prompted(view_.candidate->key());
  view_.phase = pending ? Phase::manual_pending : view_.prompted ? Phase::deferred : Phase::ready;
  view_.detail = "安装包已校验";
  publish();
}
void Coordinator::later() {
  if (view_.candidate) {
    view_.prompted = true;
    effects_.persist_prompt(view_.candidate->key(), [](auto) {});
    view_.phase = Phase::deferred;
    publish();
  }
}
void Coordinator::install() {
  if (active_ || installing_ || !view_.candidate || !receipt_ || effects_.busy())
    return;
  installing_ = true;
  view_.error.reset();
  view_.phase = Phase::preparing;
  publish();
  const auto token = generation_;
  effects_.install(*view_.candidate, *receipt_, [this, token, alive = alive_](auto r) {
    if (!*alive || token != generation_)
      return;
    installing_ = false;
    if (!r && r.error().code == ErrorCode::object_not_found) {
      view_.prompted = true;
      effects_.persist_prompt(view_.candidate->key(), [](auto) {});
      receipt_.reset();
      active_ = true;
      download();
      return;
    }
    if (!r) {
      view_.error = r.error();
      view_.phase = Phase::deferred;
      const auto reason = r.error().safe_context.find("reason");
      view_.detail =
          reason == r.error().safe_context.end() ? to_string(r.error().code) : reason->second;
    } else
      view_.phase = effects_.manual_install() ? Phase::manual_pending : Phase::installing;
    publish();
  });
}
void Coordinator::cancel() {
  if (installing_)
    return;
  ++generation_;
  const auto token = generation_;
  effects_.cancel_download([this, token, alive = alive_](auto r) {
    if (!*alive || token != generation_)
      return;
    active_ = false;
    receipt_.reset();
    view_.phase = r ? Phase::idle : Phase::cleanup_blocked;
    view_.detail = r ? "下载已取消" : "无法清理安装包";
    publish();
  });
}
} // namespace hdrshot::update
