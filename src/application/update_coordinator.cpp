#include "application/update_coordinator.hpp"
namespace hdrshot::update {
Coordinator::Coordinator(Identity i, Effects &e) : identity_(std::move(i)), effects_(e) {}
void Coordinator::set_detail(std::string text, DetailKind kind) {
  view_.detail = std::move(text);
  view_.detail_kind = kind;
  view_.detail_error.reset();
  view_.manual_open_count = 0;
}
void Coordinator::set_detail(const Error &error) {
  const auto reason = error.safe_context.find("reason");
  set_detail(reason == error.safe_context.end() ? to_string(error.code)
                                                : reason->second);
  view_.detail_error = error;
}
void Coordinator::publish() {
  view_.generation = generation_;
  view_.busy = effects_.busy();
  if (changed)
    changed(view_);
}
void Coordinator::trigger(std::int64_t now, bool cold, bool manual) {
  if (active_ || installing_ || effects_.update_operation_active())
    return;
  if (!manual && !schedule_.due(now, cold))
    return;
  ++generation_;
  pending_question_ = false;
  round_checked_ = false;
  round_prompt_consumed_ = false;
  view_.prompted = false;
  receipt_.reset();
  view_.received = view_.total = 0;
  view_.error.reset();
  view_.last_check = now;
  schedule_.begin(now);
  active_ = true;
  attempt();
}
void Coordinator::attempt() {
  view_.phase = Phase::checking;
  view_.attempts = schedule_.attempts();
  set_detail(std::string{});
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
      round_checked_ = false;
      view_.phase = Phase::check_failed;
      set_detail(checked.error());
      publish();
      return;
    }
    view_.last_success = view_.last_check;
    if (view_.error && view_.error->safe_context.contains("recheck_required"))
      view_.error.reset();
    const auto c = checked.value().candidate;
    if (!c || c->version <= identity_.version) {
      active_ = false;
      round_checked_ = false;
      receipt_.reset();
      view_.candidate.reset();
      view_.phase = Phase::idle;
      set_detail("已是最新版本", DetailKind::up_to_date);
      if (checked.value().incompatible &&
          checked.value().incompatible->version > identity_.version) {
        view_.candidate = checked.value().incompatible;
        set_detail("新版本 " + view_.candidate->version.text() +
                       " 需要更高系统版本",
                   DetailKind::incompatible);
      }
      publish();
      return;
    }
    pending_question_ = false;
    view_.candidate = c;
    round_checked_ = true;
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
          receipt_.reset();
          const auto stage = r.error().safe_context.find("update_stage");
          view_.phase = (r.error().code == ErrorCode::path_not_writable ||
                         (stage != r.error().safe_context.end() && stage->second == "cleanup"))
                            ? Phase::cleanup_blocked : Phase::download_failed;
          set_detail(r.error());
          publish();
          return;
        }
        view_.error=effects_.previous_install_error(*view_.candidate);
        receipt_ = r.value();
        view_.phase = Phase::ready;
        if (view_.error)
          set_detail(*view_.error);
        else
          set_detail("安装包已校验");
        publish();
        prompt();
      });
}
void Coordinator::prompt() {
  if (!round_checked_ || round_prompt_consumed_ || active_ || installing_ || pending_question_ ||
      effects_.update_operation_active() || !view_.candidate || !receipt_ ||
      view_.candidate->version <= identity_.version || receipt_->key != view_.candidate->key() ||
      (view_.phase != Phase::ready && view_.phase != Phase::deferred) || effects_.busy())
    return;
  round_prompt_consumed_ = true;
  view_.prompted = true;
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
  if (view_.busy != effects_.busy())
    publish();
  trigger(now);
  prompt();
}
void Coordinator::later() {
  pending_question_ = false;
  round_prompt_consumed_ = true;
  view_.prompted = true;
  if (effects_.update_operation_active()) {
    publish();
    return;
  }
  // Closing a window during checking/downloading does not cancel its work.
  if (!active_ && !installing_ && view_.candidate &&
      (view_.phase == Phase::ready || view_.phase == Phase::deferred ||
       view_.phase == Phase::manual_pending)) {
    ++generation_;
    if (view_.phase != Phase::manual_pending)
      view_.phase = Phase::deferred;
  }
  publish();
}
void Coordinator::install() {
  if (!round_checked_ || active_ || installing_ || effects_.update_operation_active() ||
      !view_.candidate || !receipt_ || view_.candidate->version <= identity_.version ||
      receipt_->key != view_.candidate->key() || effects_.busy() ||
      (view_.phase != Phase::ready && view_.phase != Phase::deferred &&
       view_.phase != Phase::manual_pending))
    return;
  pending_question_ = false;
  round_prompt_consumed_ = true;
  view_.prompted = true;
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
    if (!r) {
      view_.error = r.error();
      const bool recheck = r.error().code == ErrorCode::object_not_found ||
                           r.error().safe_context.contains("recheck_required");
      view_.phase = recheck ? Phase::check_failed : Phase::deferred;
      if (recheck) {
        receipt_.reset();
        round_checked_ = false;
        view_.error->safe_context["recheck_required"] = "true";
      }
      set_detail(r.error());
    } else
      view_.phase = effects_.manual_install() ? Phase::manual_pending : Phase::installing;
    publish();
  });
}
void Coordinator::manual_open_result(Result<bool,Error> result) {
  if(result) {
    view_.phase=Phase::manual_pending;
    view_.detail = "安装包已打开。\n" + view_.detail;
    ++view_.manual_open_count;
  }
  else {
    const auto directory=view_.error?view_.error->safe_context.find("install_directory"):std::map<std::string,std::string>::const_iterator{};
    const auto saved_directory=view_.error && directory!=view_.error->safe_context.end()?directory->second:std::string{};
    view_.error=result.error();
    if(view_.error->code!=ErrorCode::object_not_found)view_.error->safe_context["manual_install_required"]="true";
    if(!saved_directory.empty())view_.error->safe_context["install_directory"]=saved_directory;
    set_detail(*view_.error);
    if (view_.error->code == ErrorCode::object_not_found) {
      receipt_.reset();
      round_checked_ = false;
      view_.error->safe_context["recheck_required"] = "true";
      view_.phase = Phase::download_failed;
    } else
      view_.phase = Phase::deferred;
  }
  publish();
}
void Coordinator::cancel() {
  if (installing_ || effects_.update_operation_active())
    return;
  pending_question_ = false;
  ++generation_;
  const auto token = generation_;
  round_checked_ = false;
  round_prompt_consumed_ = true;
  view_.prompted = true;
  receipt_.reset();
  active_ = true;
  effects_.cancel_download([this, token, alive = alive_](auto r) {
    if (!*alive || token != generation_)
      return;
    active_ = false;
    receipt_.reset();
    view_.phase = r ? Phase::idle : Phase::cleanup_blocked;
    if (r)
      set_detail("下载已取消", DetailKind::download_canceled);
    else
      set_detail("无法清理安装包", DetailKind::cancel_cleanup_failed);
    publish();
  });
}
} // namespace hdrshot::update
