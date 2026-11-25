// Real SQLite transactions with synthetic owner/message/artifact identities.
// No installed Runtime, network connection or peer acceptance is asserted.
#define main preserved_file_fixture_main
#include "file_storage_tests.cpp"
#undef main
#include "sfx/application/ftn_setup.hpp"
namespace {
void migration25(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& paths){
    for(const char* point:{"before-ftn-wire-body-migration","before-ftn-wire-body-migration-commit","after-ftn-wire-body-migration-commit"}){
        auto config=settings(root/point);auto old=storage::sfx_storage_migrations();old.resize(24);
        {storage::SqliteStorage db(config,paths,old);check(db.start().succeeded()&&db.health().schema_version==24,"fixed v24 predecessor");check(db.shutdown().succeeded(),"v24 stopped");}
        seed(config.data_directory/"SFX.DB");const auto before=scalar(config.data_directory/"SFX.DB","SELECT count(*) FROM sfx_accounts");
        config.bootstrap_boundary_hook=[point](std::string_view at){return at==point?app::StorageStatus{app::StorageErrorCode::io,"injected schema25 interruption"}:app::StorageStatus::success();};
        {storage::SqliteStorage failed(config,paths);check(!failed.start(),"interrupted schema25 result is truthful");}
        check(scalar(config.data_directory/"SFX.DB","PRAGMA user_version")== (std::string_view(point)=="after-ftn-wire-body-migration-commit"?25:24),"schema25 commit/rollback atomic");
        config.bootstrap_boundary_hook={};storage::SqliteStorage retry(config,paths);check(retry.start().succeeded()&&retry.health().schema_version==27,"upgrade resumes once");check(retry.shutdown().succeeded(),"upgrade retry stopped");
        check(scalar(config.data_directory/"SFX.DB","SELECT count(*) FROM sfx_accounts")==before,"upgrade preserves original accounts");
    }
}
void migration27(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& paths){
    for(const char* point:{"before-message-location-migration","before-message-location-migration-commit"}){
        auto config=settings(root/point);auto old=storage::sfx_storage_migrations();old.resize(26);
        {storage::SqliteStorage db(config,paths,old);check(db.start().succeeded()&&db.health().schema_version==26,"fixed schema26 predecessor starts");check(db.shutdown().succeeded(),"schema26 predecessor stops");}
        const auto file=config.data_directory/"SFX.DB";seed(file);const auto before=scalar(file,"SELECT count(*) FROM sfx_accounts");
        config.bootstrap_boundary_hook=[point](std::string_view at){return at==point?app::StorageStatus{app::StorageErrorCode::io,"injected caller-location migration interruption"}:app::StorageStatus::success();};
        {storage::SqliteStorage failed(config,paths);check(!failed.start(),"interrupted caller-location migration reports failure");}
        check(scalar(file,"PRAGMA user_version")==26&&scalar(file,"SELECT count(*) FROM sfx_schema_migrations")==26,"schema27 publication and ledger roll back together");
        {internal::SqliteConnection db(file,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);check(internal::validate_sfx_shipping_database(db.get(),true),"failed schema27 retains exact fixed predecessor manifest");check(db.scalar_int64("SELECT count(*) FROM sqlite_schema WHERE name IN('sfx_caller_message_location','sfx_ftn_toss_pending')","location rollback")==0,"table and pending-work index roll back together");}
        config.bootstrap_boundary_hook={};storage::SqliteStorage retry(config,paths);check(retry.start().succeeded()&&retry.health().schema_version==27,"schema27 retry succeeds exactly once");check(retry.shutdown().succeeded(),"schema27 retry closes");
        check(scalar(file,"SELECT count(*) FROM sfx_accounts")==before&&scalar(file,"SELECT count(*) FROM sfx_schema_migrations")==27,"schema27 preserves accounts and adds one ledger entry");
        check(retry.start().succeeded()&&retry.shutdown().succeeded()&&scalar(file,"SELECT count(*) FROM sfx_schema_migrations")==27,"reopen does not repeat schema27 migration");
    }
}
}
int main(int argc,char** argv){try{
    check(argc==2,"new fixture root required");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"preserve previous fixture evidence");
    sfx::test::FakeStoragePathPolicy paths;auto config=settings(root/"primary");
    {storage::SqliteStorage fresh(config,paths);check(fresh.start().succeeded()&&fresh.health().schema_version==27,"current schema27 starts");check(fresh.shutdown().succeeded(),"empty current fixture stopped");}
    const auto file=config.data_directory/"SFX.DB";seed(file);storage::SqliteStorage db(config,paths);check(db.start().succeeded(),"owned synthetic writer ready");const app::MessageConfigurationActor owner{true,{}};
    auto c=*db.ftn_configuration(owner).value;app::FtnNetwork n;n.code="FIDO";n.name="FidoNet";n.address="42:100/49";n.enabled=true;n.origin_line="Synthetic SFX";
    check(app::add_ftn_setup_network(c,n,{}).succeeded(),"stage named network and group");
    for(const auto tag:{"SYSOP","FIDOTEST","MYSTIC"})check(app::add_ftn_setup_echo(c,n.code,tag).succeeded(),"stage selected echoes");
    app::FtnLink hub;hub.network=n.code;hub.code="HUB";hub.name="Fido Hub";hub.address="42:100/81";hub.host="127.0.0.1";hub.enabled=true;hub.poll_minutes=0;hub.contact_on_mail=true;hub.credentials.areafix={1}; // opaque synthetic credential port
    c.links.push_back(hub);c.networks[0].default_link=hub.code;for(const auto& e:c.message_echoes)c.permissions.push_back({n.code,hub.code,e.tag,false,true,true,false});
    check(scalar(file,"SELECT count(*) FROM sfx_message_groups WHERE name='FidoNet'")==0,"draft allocates no group");
    auto saved=db.change_ftn_configuration(owner,id(100).bytes,c);check(saved&&saved.value,"final settings save succeeds");
    check(scalar(file,"SELECT count(*) FROM sfx_message_groups WHERE name='FidoNet'")==1,"group created once at final Save");
    check(scalar(file,"SELECT count(*) FROM sfx_ftn_control_intents")==0,"Save alone never queues subscriptions");
    c=saved.value->configuration;check(!c.setup_subscriptions&&c.networks[0].message_group>0&&c.networks[0].new_group_name.empty(),"saved canonical group identity replaces draft name");
    for(const auto& e:c.message_echoes)check(e.local_echo>0&&e.maximum_messages==500U&&!e.maximum_age_days&&e.ansi_colors==false&&e.access_level==10&&e.read_level==10&&e.post_level==10,"new echo defaults projected from authoritative rows");
    c.message_echoes[0].access_level=c.message_echoes[0].read_level=c.message_echoes[0].post_level=999;c.message_echoes[0].update_local_permissions=true;c.message_echoes[0].maximum_age_days=45;c.message_echoes[0].update_local_retention=2;
    const std::vector<std::string> tags{"SYSOP","FIDOTEST","MYSTIC"};app::FtnEchoBulkOptions count;count.copy_count=true;count.maximum_messages=1000;check(app::apply_ftn_echo_options(c,n.code,tags,count).changed==3,"count-only explicit clone");
    c.setup_subscriptions=app::FtnSetupSubscriptions{n.code,hub.code,tags};const auto request=c;
    saved=db.change_ftn_configuration(owner,id(101).bytes,c);check(saved&&saved.value&&!saved.value->configuration.setup_subscriptions,"Save and Queue consumes command atomically");
    check(scalar(file,"SELECT count(*) FROM sfx_ftn_control_intents")==1&&scalar(file,"SELECT count(*) FROM sfx_ftn_provider_requests")==3,"one bounded request with three correlated tags");
    const auto replay=db.change_ftn_configuration(owner,id(101).bytes,request);check(replay&&replay.value->replayed&&scalar(file,"SELECT count(*) FROM sfx_ftn_control_intents")==1,"lost save response never duplicates request");
    auto changed=request;changed.setup_subscriptions->tags.pop_back();check(!db.change_ftn_configuration(owner,id(101).bytes,changed),"changed request cannot reuse receipt");
    const auto controls=db.pending_ftn_control(owner,{},16);check(controls&&controls.value->size()==1&&controls.value->front().password_subject&&controls.value->front().text.body=="+SYSOP\r+FIDOTEST\r+MYSTIC\r","subscription body contains tags only; AreaFix credential remains protected");
    c=*db.ftn_configuration(owner).value;check(c.message_echoes[0].access_level==999&&c.message_echoes[0].maximum_age_days==45U&&c.message_echoes[1].access_level==10&&!c.message_echoes[1].maximum_age_days,"clone preserves permission exceptions and independent age");
    check(!db.ftn_outbound_ready({},n.code,hub.code),"ordinary readiness authority refused");check(db.ftn_outbound_ready(owner,n.code,hub.code).value==false,"control intent is not a delivered or claimed packet");
    auto conflict=c;conflict.networks.push_back(n);check(!db.change_ftn_configuration(owner,id(102).bytes,conflict)&&scalar(file,"SELECT count(*) FROM sfx_message_groups WHERE name='FidoNet'")==1,"failed final save creates no orphan group");
    c.message_echoes[1].ansi_colors=true;c.message_echoes[1].require_real_name=false;check(db.change_ftn_configuration(owner,id(103).bytes,c).succeeded(),"enable ANSI on one echo only");c=*db.ftn_configuration(owner).value;
    const app::MessageActor actor{id(2),2,1,1};const app::MessageTarget target{app::MessageKind::public_echo,{c.message_echoes[1].local_echo},{},{}};
    const auto context=db.begin_message(actor,target);check(context&&context.value,"caller can post in selected ordinary echo");
    const app::CommitSourceMessageRequest post{actor,id(110),*context.value,"Colors",{app::MessageSourceFormat::ansi_canvas,1,"\x1b[31mRed\x1b[0m\r\nSecond"}};const auto posted=db.commit_source_message(post);check(posted&&posted.value,"immutable source post committed");
    const auto opened=db.open_message(actor,posted.value->id);check(opened&&opened.value->ansi_colors&&opened.value->original_source->bytes==post.source.bytes,"reader receives actual per-echo ANSI policy and untouched source");
    const auto pending=db.pending_ftn_scan(owner,n.code,0,16);check(pending&&pending.value->size()==1&&pending.value->front().body.find('\x1b')!=std::string::npos,"ANSI enabled outgoing rendition has bounded generated SGR");
    const auto intent=pending.value->front();app::FtnOutboundJob job;job.id=id(120);job.operation=intent.operation;job.network=n.code;job.link=hub.code;job.file={"colors.pkt",6,1700000000};job.digest.fill(7);
    check(db.commit_ftn_scan(owner,c.revision,intent,{job}).succeeded(),"exact synthetic outbound artifact publication");check(db.ftn_outbound_ready(owner,n.code,hub.code).value==true,"queued mail is visible without a claim");
    c=*db.ftn_configuration(owner).value;c.message_echoes[1].ansi_colors=false;check(db.change_ftn_configuration(owner,id(104).bytes,c).succeeded(),"disable ANSI without changing stored content");c=*db.ftn_configuration(owner).value;
    check(db.commit_ftn_scan(owner,c.revision,intent,{job}).succeeded(),"published wire body replays exactly after policy edit");
    const auto now_plain=db.open_message(actor,posted.value->id);check(now_plain&&now_plain.value->ansi_colors&&now_plain.value->original_source->bytes==post.source.bytes,"outgoing Off does not disable reading colors or change original source");
    {internal::SqliteConnection raw(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);check(sqlite3_exec(raw.get(),"UPDATE sfx_ftn_wire_bodies SET body=x'61'",nullptr,nullptr,nullptr)!=SQLITE_OK,"published wire body is immutable");}
    const app::MessageActor sysop{id(1),1,1,1};check(db.delete_message(sysop,id(133),posted.value->id).succeeded()&&scalar(file,"SELECT count(*) FROM sfx_ftn_wire_bodies")==0,"authorized explicit deletion removes frozen body with original message");
    check(scalar(file,"SELECT count(*) FROM sfx_ftn_outbound WHERE state=0")==1,"explicit local deletion preserves already published outbound artifact policy");
    check(db.shutdown().succeeded(),"current synthetic writer stopped");
    {internal::SqliteConnection raw(file,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);check(internal::validate_sfx_shipping_database(raw.get(),true),"exact schema25 ledger/manifest/integrity");}
    {storage::SqliteStorage reopen(config,paths);check(reopen.start().succeeded(),"settings reopen");const auto current=reopen.ftn_configuration(owner);check(current&&current.value->networks[0].default_link==hub.code&&!current.value->message_echoes[1].ansi_colors.value_or(true),"new fields survive restart");check(reopen.shutdown().succeeded(),"reopened writer stopped");}
    migration25(root/"upgrade",paths);
    migration27(root/"caller-location-upgrade",paths);
    std::cout<<"PASS "<<checks<<" actual setup/save/queue/clone/ANSI/schema25 checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
