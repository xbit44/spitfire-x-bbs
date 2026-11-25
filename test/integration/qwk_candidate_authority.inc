#include "../../src/apps/runtime/qwk_runtime_authority.hpp"
void runtime_authority_checks(storage::SqliteStorage& db,const app::MessageActor& reader,const app::QwkPacketPlan& plan,const std::filesystem::path& file){
    const auto policy=db.qwk_caller_policy(reader);check(bool(policy),"fresh runtime policy proof");const app::QwkTransferRequest request{reader,policy.value->board_id,policy.value->revision};
    const auto allowed=[&]{return sfx::apps::runtime::qwk_runtime_authorized(db,reader.session,reader.account,request,&plan);};
    check(allowed(),"current runtime callback proof accepts exact live actor/board/plan");
    check(!sfx::apps::runtime::qwk_runtime_authorized(db,id(2),reader.account,request)&&!sfx::apps::runtime::qwk_runtime_authorized(db,reader.session,2,request),"runtime ready session/account replacement refused");
    auto stale=request;++stale.actor.account_revision;check(!sfx::apps::runtime::qwk_runtime_authorized(db,reader.session,reader.account,stale),"stale supplied account revision refused");stale=request;++stale.actor.policy_revision;check(!sfx::apps::runtime::qwk_runtime_authorized(db,reader.session,reader.account,stale),"stale supplied security revision refused");
    const auto fault=[&](const char* sql,const char* undo,const char* why){
        {internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);c.exec(sql,"change only synthetic runtime authority fixture");}
        check(!allowed(),why);
        {internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);c.exec(undo,"restore only synthetic runtime authority fixture");}
        check(allowed(),"exact restored runtime authority revalidated freshly");
    };
    fault("UPDATE sfx_accounts SET account_state='locked' WHERE account_id=3","UPDATE sfx_accounts SET account_state='active' WHERE account_id=3","live locked account refused");
    fault("UPDATE sfx_accounts SET revision=revision+1 WHERE account_id=3","UPDATE sfx_accounts SET revision=revision-1 WHERE account_id=3","live account revision change refused");
    fault("UPDATE sfx_security_levels SET level_state='retired' WHERE security_level=10","UPDATE sfx_security_levels SET level_state='active' WHERE security_level=10","live retired security level refused");
    fault("UPDATE sfx_security_policy SET revision=revision+1,published_revision=NULL,published_policy_hash=NULL","UPDATE sfx_security_policy SET revision=revision-1,published_revision=revision-1,published_policy_hash=policy_hash","live security policy revision change refused");
    fault("UPDATE sfx_security_policy SET published_revision=NULL,published_policy_hash=NULL","UPDATE sfx_security_policy SET published_revision=revision,published_policy_hash=policy_hash","unpublished security policy refused");
    fault("UPDATE sfx_account_sessions SET completed_utc_seconds=1794484800,outcome_code='disconnected' WHERE account_id=3","UPDATE sfx_account_sessions SET completed_utc_seconds=NULL,outcome_code=NULL WHERE account_id=3","completed durable session refused");
    fault("UPDATE sfx_account_sessions SET account_id=2 WHERE account_id=3","UPDATE sfx_account_sessions SET account_id=3 WHERE session_id=(SELECT correlation_id FROM sfx_account_sessions WHERE node_id=3)","durable session rebound to different account refused");
    fault("UPDATE sfx_qwk_policy SET revision=revision+1","UPDATE sfx_qwk_policy SET revision=revision-1","changed board policy revision refused");
    const auto board=policy.value->board_id;const auto undo="UPDATE sfx_qwk_policy SET board_id='"+board+"'";
    fault("UPDATE sfx_qwk_policy SET board_id='ALT12345'",undo.c_str(),"changed board identity refused");
    fault("UPDATE sfx_local_echoes SET reading_minimum=999 WHERE echo_id=2","UPDATE sfx_local_echoes SET reading_minimum=1 WHERE echo_id=2","prepared download active plan access refusal retained");
}
