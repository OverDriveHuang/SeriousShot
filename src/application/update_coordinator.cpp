#include "application/update_coordinator.hpp"
#include <algorithm>
namespace hdrshot::update {
Coordinator::Coordinator(Identity i, Effects &e) : identity_(std::move(i)), effects_(e) {}
void Coordinator::publish() {
  view_.generation = generation_;
  view_.busy = effects_.busy();
  if (changed)
    changed(view_);
}
void Coordinator::trigger(std::int64_t now, bool cold, bool manual) {
  now_ = now;
  // Recovered manual transactions already own a verified package. A cold
  // start must offer it without changing its generation through a new check.
  if ((cold || !manual) && view_.candidate && receipt_ && receipt_->key == view_.candidate->key() &&
      (view_.phase == Phase::ready || view_.phase == Phase::deferred ||
       view_.phase == Phase::manual_pending || view_.phase == Phase::check_failed) &&
      reminder_due()) {
    prompt();
    return;
  }
  if (active_ || installing_ || pending_question_ || view_.phase == Phase::manual_pending)
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
    if (view_.error && view_.error->safe_context.contains("recheck_required"))
      view_.error.reset();
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
    view_.prompted = !reminder_due();
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
        view_.error=effects_.previous_install_error(*view_.candidate);
        receipt_ = r.value();
        view_.phase = view_.prompted ? Phase::deferred : Phase::ready;
        view_.detail = "安装包已校验";
        if(view_.error) {
          const auto reason=view_.error->safe_context.find("reason");
          view_.detail=reason==view_.error->safe_context.end()?to_string(view_.error->code):reason->second;
        }
        publish();
        prompt();
      });
}
bool Coordinator::reminder_due() const {
  if (!view_.candidate)
    return false;
  if (reminder_key_ != view_.candidate->key() || !reminder_origin_)
    return true;
  if (now_ < *reminder_origin_)
    return false;
  const auto elapsed =
      static_cast<std::uint64_t>(now_) - static_cast<std::uint64_t>(*reminder_origin_);
  return elapsed / 604800 > consumed_period_;
}
void Coordinator::consume_reminder(bool displayed) {
  const auto key = view_.candidate->key();
  if (displayed || reminder_key_ != key || !reminder_origin_) {
    reminder_key_ = key;
    reminder_origin_ = now_;
    consumed_period_ = 0;
  } else if (now_ >= *reminder_origin_) {
    // Closing a question left open over a deadline consumes that period too;
    // it must not immediately reopen on the next one-second tick.
    const auto elapsed =
        static_cast<std::uint64_t>(now_) - static_cast<std::uint64_t>(*reminder_origin_);
    consumed_period_ = std::max(consumed_period_, elapsed / 604800);
  }
  view_.prompted = true;
}
void Coordinator::prompt() {
  if (active_ || installing_ || pending_question_ || !view_.candidate || !receipt_ ||
      receipt_->key != view_.candidate->key() ||
      (view_.phase != Phase::ready && view_.phase != Phase::deferred &&
       view_.phase != Phase::manual_pending && view_.phase != Phase::check_failed) ||
      effects_.busy() || !reminder_due())
    return;
  // Reminder scope is this process and this seven-day period. Legacy durable
  // prompted keys are history only; they cannot suppress a new process.
  if (view_.phase == Phase::check_failed)
    view_.phase = Phase::ready; // The already verified package survives metadata failure.
  consume_reminder(true);
  pending_question_ = true;
  const auto token = ++generation_;
  publish();
  effects_.ask_install(*view_.candidate, [this, token, alive = alive_](bool accepted) {
    if (!*alive || token != generation_)
      return;
    if (accepted)
      install();
    else
      later();
  });
}
void Coordinator::tick(std::int64_t now) {
  now_ = now;
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
  view_.prompted = !reminder_due();
  view_.phase = pending ? Phase::manual_pending : view_.prompted ? Phase::deferred : Phase::ready;
  view_.detail = "安装包已校验";
  view_.error=effects_.previous_install_error(*view_.candidate);
  if(view_.error) {
    const auto reason=view_.error->safe_context.find("reason");
    view_.detail=reason==view_.error->safe_context.end()?to_string(view_.error->code):reason->second;
  }
  publish();
}
void Coordinator::later() {
  if (view_.candidate) {
    pending_question_ = false;
    ++generation_;
    consume_reminder(false);
    effects_.persist_prompt(view_.candidate->key(), [](auto) {});
    // A mounted manual transaction still owns the single archive slot. Hiding
    // its question must not unlock checks or imply that the source can be
    // removed.
    if (view_.phase != Phase::manual_pending)
      view_.phase = Phase::deferred;
    publish();
  }
}
void Coordinator::install() {
  if (active_ || installing_ || !view_.candidate || !receipt_ ||
      receipt_->key != view_.candidate->key() || effects_.busy())
    return;
  pending_question_ = false;
  consume_reminder(false);
  ++generation_;
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
      const bool recheck = r.error().safe_context.contains("recheck_required");
      view_.phase = recheck ? Phase::check_failed : Phase::deferred;
      if (recheck) receipt_.reset();
      const auto reason = r.error().safe_context.find("reason");
      view_.detail =
          reason == r.error().safe_context.end() ? to_string(r.error().code) : reason->second;
    } else
      view_.phase = effects_.manual_install() ? Phase::manual_pending : Phase::installing;
    publish();
  });
}
void Coordinator::manual_open_result(Result<bool,Error> result) {
  if(result) {
    view_.phase=Phase::manual_pending;
    view_.detail="安装包已打开。\n"+view_.detail;
  }
  else {
    const auto directory=view_.error?view_.error->safe_context.find("install_directory"):std::map<std::string,std::string>::const_iterator{};
    const auto saved_directory=view_.error && directory!=view_.error->safe_context.end()?directory->second:std::string{};
    view_.error=result.error();
    if(view_.error->code!=ErrorCode::object_not_found)view_.error->safe_context["manual_install_required"]="true";
    if(!saved_directory.empty())view_.error->safe_context["install_directory"]=saved_directory;
    const auto reason=view_.error->safe_context.find("reason");
    view_.detail=reason==view_.error->safe_context.end()?to_string(view_.error->code):reason->second;
    view_.phase=view_.error->code==ErrorCode::object_not_found?Phase::download_failed:Phase::deferred;
  }
  publish();
}
void Coordinator::cancel() {
  if (installing_)
    return;
  pending_question_ = false;
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
