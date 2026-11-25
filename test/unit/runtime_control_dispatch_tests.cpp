#include "runtime_control_dispatch.hpp"
#include <iostream>
#include <stdexcept>

namespace app=sfx::application;
namespace application=sfx::application;
namespace rt=sfx::apps::runtime;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
#include "runtime_control_dispatch_fixture.inc"
#include "runtime_control_door_bulletin_checks.inc"
void run(){
    DispatchFixture host;app::AlphaStatusSnapshotCache cache;
    app::LocalControlFrame request;request.request_id=77;
    for(const auto type:malformed_types){
        request.type=type;request.payload={255};
        const auto response=rt::dispatch_runtime_control(host,request,cache);
        check(host.calls==0,"malformed input must not invoke a host operation");
        check(request.payload.empty(),"request bytes cleared on every malformed return");
        check(!response||response->request_id==77,"response preserves correlation");
    }
    request.type=app::LocalControlMessageType::account_query_request;
    app::LocalAccountQuery query;query.account_id=42;
    request.payload=app::encode_account_query(query);
    const auto account=rt::dispatch_runtime_control(host,request,cache);
    check(host.calls==1&&account&&account->type==app::LocalControlMessageType::account_query_response,"valid account query reaches exactly one host operation");
    check(app::decode_account_snapshot(account->payload)->code==app::LocalAccountResultCode::unavailable,"host failure remains unavailable");
    request.type=app::LocalControlMessageType::lifecycle_command_request;
    request.payload=app::encode_lifecycle_command_request({app::AlphaLifecycleCommandKind::cancel_drain,0});
    const auto cancelled=rt::dispatch_runtime_control(host,request,cache);
    check(host.calls==2&&cancelled&&app::decode_control_response(cancelled->payload)->code==app::LocalControlResponseCode::ok,"cancel drain reaches typed operation without shutdown");
    app::AlphaBootstrapRequest setup;
    setup.configuration.bbs_time_zone="Pacific Standard Time";
    setup.configuration.security_policy_revision=1;setup.configuration.security_policy_hash=std::string(64,'a');
    setup.sysop_name="Fixture Sysop";setup.sysop_password="Fixture account password";setup.system_password="Fixture System Password";
    request.type=app::LocalControlMessageType::bootstrap_request;request.payload=app::encode_bootstrap_request(setup);
    check(!request.payload.empty(),"valid bootstrap fixture encoded");
    const auto bootstrap=rt::dispatch_runtime_control(host,request,cache);
    check(host.calls==3&&bootstrap&&request.payload.empty(),"bootstrap dispatch clears encoded secret bytes");
    check(app::decode_control_response(bootstrap->payload)->code==app::LocalControlResponseCode::setup_required,"setup result not promoted to ready");
    host.fail_bootstrap=true;request.payload=app::encode_bootstrap_request(setup);
    bool threw=false;try{(void)rt::dispatch_runtime_control(host,request,cache);}catch(const std::runtime_error&){threw=true;}
    check(threw&&host.calls==4&&request.payload.empty(),"exception also erases request payload");
    app::erase_bootstrap_secrets(setup);
    for(const auto type:{app::LocalControlMessageType::hello,app::LocalControlMessageType::shutdown_request}){
        request.type=type;request.payload.clear();
        const auto response=rt::dispatch_runtime_control(host,request,cache);
        check(host.calls==4&&response&&app::decode_control_response(response->payload)->code==app::LocalControlResponseCode::invalid_request,"platform-only authentication and shutdown cannot dispatch business operations");
    }
}
}
int main(){try{run();door_bulletin_dispatch_checks();std::cout<<"PASS "<<checks<<" shared Runtime dispatch checks\n";return 0;}
catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
