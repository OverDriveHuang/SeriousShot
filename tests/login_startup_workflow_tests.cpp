#include "application/login_startup_workflow.hpp"
#include "test_support.hpp"
using namespace hdrshot;
namespace {
struct Fake final : LoginStartupPort {
  LoginStartupSnapshot snapshot{LoginStartupState::disabled, "/stable/SeriousShot.app", {}, true, false, true, {}};
  int reads{}, writes{}; bool fail_write{}, fail_before{}, fail_after{};
  Result<LoginStartupSnapshot,Error> query() override {
    ++reads;
    if ((fail_before && reads == 1) || (fail_after && reads > 1)) return Result<LoginStartupSnapshot,Error>::failure(error());
    return Result<LoginStartupSnapshot,Error>::success(snapshot);
  }
  static Error error(){return {ErrorCode::permission_denied,"fixture",Retryability::after_user_action,{{"native_code","42"}}};}
  Result<bool,Error> set_enabled(bool enabled) override {
    ++writes;snapshot.state=enabled?LoginStartupState::enabled:LoginStartupState::disabled;
    snapshot.can_enable=!enabled;snapshot.can_disable=enabled;
    return fail_write?Result<bool,Error>::failure(error()):Result<bool,Error>::success(true);
  }
  Result<bool,Error> open_system_settings() override{return Result<bool,Error>::success(true);}
};
}
int main(){return test::run({
 {"failed write still returns final system state and original error",[]{Fake p;p.fail_write=true;const auto r=LoginStartupWorkflow(p).change_enabled(true);HDRSHOT_CHECK(r.snapshot.state==LoginStartupState::enabled);HDRSHOT_CHECK(r.operation_error==Fake::error());HDRSHOT_CHECK(p.reads==2&&p.writes==1);}},
 {"failed final query is unknown, never old checkbox",[]{Fake p;p.fail_after=true;const auto r=LoginStartupWorkflow(p).change_enabled(true);HDRSHOT_CHECK(r.snapshot.state==LoginStartupState::unknown);HDRSHOT_CHECK(r.query_error.has_value());HDRSHOT_CHECK(!r.snapshot.can_enable&&!r.snapshot.can_disable);}},
 {"pre-read failure never writes",[]{Fake p;p.fail_before=true;const auto r=LoginStartupWorkflow(p).change_enabled(true);HDRSHOT_CHECK(p.writes==0);HDRSHOT_CHECK(r.snapshot.state==LoginStartupState::unknown&&r.query_error.has_value());}},
 {"enabled and disabled requests are idempotent",[]{Fake p;LoginStartupWorkflow w(p);w.change_enabled(false);HDRSHOT_CHECK(p.writes==0);w.change_enabled(true);w.change_enabled(true);HDRSHOT_CHECK(p.writes==1);w.change_enabled(false);HDRSHOT_CHECK(p.writes==2);}},
 {"external denial never unregisters or registers around approval",[]{Fake p;p.snapshot.state=LoginStartupState::requires_approval;p.snapshot.can_enable=false;p.snapshot.can_disable=true;const auto r=LoginStartupWorkflow(p).change_enabled(true);HDRSHOT_CHECK(p.writes==0);HDRSHOT_CHECK(r.snapshot.state==LoginStartupState::requires_approval&&r.operation_error.has_value());}},
 {"notFound is separately actionable for a valid bundle",[]{Fake p;p.snapshot.state=LoginStartupState::service_not_found;const auto r=LoginStartupWorkflow(p).change_enabled(true);HDRSHOT_CHECK(p.writes==1);HDRSHOT_CHECK(r.snapshot.state==LoginStartupState::enabled);}},
 {"nonwritable states cannot become successful disabled writes",[]{for(auto state:{LoginStartupState::blocked,LoginStartupState::invalid_target,LoginStartupState::unknown}){Fake p;p.snapshot.state=state;p.snapshot.can_enable=p.snapshot.can_disable=false;const auto r=LoginStartupWorkflow(p).change_enabled(true);HDRSHOT_CHECK(p.writes==0&&r.snapshot.state==state&&r.operation_error.has_value());}}}
});}
