#include "sfx/application/chat.hpp"
#include "sfx/adapters/storage/control_panel_statistics.hpp"
#define main retained_chat_storage_fixture
#include "file_storage_tests.cpp"
#undef main
#include "chat_statistics_checks.inc"
int main(int argc,char** argv){try{
    check(argc==2,"new isolated Chat storage root");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"no fixture overwrite");sfx::test::FakeStoragePathPolicy paths;
    for(const char* point:{"before-chat-migration","before-chat-migration-commit","after-chat-migration-commit"}){
        auto s=settings(root/point);auto prior=storage::sfx_storage_migrations();prior.resize(18);
        {storage::SqliteStorage old(s,paths,prior);check(old.start().succeeded()&&old.health().schema_version==18,"exact predecessor18");check(old.shutdown().succeeded(),"predecessor closed");}seed(s.data_directory/"SFX.DB");
        s.bootstrap_boundary_hook=[point](std::string_view at){return at==point?app::StorageStatus{app::StorageErrorCode::io,"isolated Chat migration failure"}:app::StorageStatus::success();};
        {storage::SqliteStorage failed(s,paths);check(!failed.start(),"injected migration fails closed");}
        check(scalar(s.data_directory/"SFX.DB","PRAGMA user_version")== (std::string_view(point)=="after-chat-migration-commit"?19:18),"atomic migration version");
        s.bootstrap_boundary_hook={};storage::SqliteStorage retry(s,paths);check(retry.start().succeeded()&&retry.health().schema_version==sfx::test::shipping_schema_version,"migration retries");check(retry.shutdown().succeeded(),"retry stopped");
        check(scalar(s.data_directory/"SFX.DB","SELECT count(*) FROM sfx_accounts")==3&&scalar(s.data_directory/"SFX.DB","SELECT count(*) FROM sfx_account_sessions")==3,"legacy accounts and sessions retained");
    }
    for(const char* point:{"before-private-chat-migration","before-private-chat-migration-commit","after-private-chat-migration-commit"}){
        auto fixture=settings(root/point);auto prior=storage::sfx_storage_migrations();prior.resize(19);
        {storage::SqliteStorage old(fixture,paths,prior);check(old.start().succeeded()&&old.health().schema_version==19,"exact reviewed Chat predecessor19");check(old.shutdown().succeeded(),"v19 closed");}seed(fixture.data_directory/"SFX.DB");
        fixture.bootstrap_boundary_hook=[point](std::string_view at){return at==point?app::StorageStatus{app::StorageErrorCode::io,"isolated private Chat migration failure"}:app::StorageStatus::success();};
        {storage::SqliteStorage failed(fixture,paths);check(!failed.start(),"private Chat migration fails closed");}
        check(scalar(fixture.data_directory/"SFX.DB","PRAGMA user_version")==(std::string_view(point)=="after-private-chat-migration-commit"?20:19),"private Chat migration atomic");
        fixture.bootstrap_boundary_hook={};storage::SqliteStorage retry(fixture,paths);check(retry.start().succeeded()&&retry.health().schema_version==sfx::test::shipping_schema_version,"private Chat migration retry");check(retry.shutdown().succeeded(),"private Chat retry closed");
        check(scalar(fixture.data_directory/"SFX.DB","SELECT count(*) FROM sfx_accounts")==3,"private Chat upgrade keeps all accounts");
    }
    auto s=settings(root/"primary");storage::SqliteStorage db(s,paths);check(db.start().succeeded()&&db.health().schema_version==sfx::test::shipping_schema_version,"fresh current shipping schema");check(db.shutdown().succeeded(),"stop before synthetic actors");const auto file=s.data_directory/"SFX.DB";seed(file);check(db.start().succeeded(),"start fixture");
    const app::MessageConfigurationActor owner{true,{}};const app::MessageActor alice{id(2),2,1,1},sysop{id(1),1,1,1};
    auto c=db.chat_configuration(owner);check(c&&c.value&&c.value->rooms.size()==1&&c.value->rooms[0].access.level==10&&c.value->exclude_sysop_statistics,"default Lobby10 and statistics On");
    auto ordinary=db.chat_preferences(alice);check(ordinary&&ordinary.value&&!ordinary.value->dnd&&!ordinary.value->ghost&&ordinary.value->notices,"new preference defaults");
    auto draft=*ordinary.value;draft.ghost=true;check(!db.save_chat_preferences(alice,draft),"ordinary cannot enable Ghost");draft.ghost=false;draft.dnd=true;draft.notices=false;
    auto saved=db.save_chat_preferences(alice,draft);check(saved&&saved.value&&saved.value->dnd&&!saved.value->notices,"own preferences saved");check(!db.save_chat_preferences(alice,draft),"stale preference revision refused");
    auto ghost=*db.chat_preferences(sysop).value;ghost.ghost=true;check(db.save_chat_preferences(sysop,ghost).succeeded(),"authorized Sysop Ghost saved");
    auto spoof=alice;spoof.account=1;check(!db.chat_preferences(spoof)&&!db.save_chat_preferences(spoof,draft),"session account spoof refused");
    auto next=*c.value;next.rooms.push_back({2,"Games",true,{app::DoorSecurityMode::minimum,10,false}});check(!db.change_chat_configuration({false,alice},next),"ordinary settings write refused");
    auto changed=db.change_chat_configuration(owner,next);check(changed&&changed.value&&changed.value->revision==2,"owner configuration CAS");check(!db.change_chat_configuration(owner,next),"stale config refused");
    next=*changed.value;next.entry=2;check(!db.change_chat_configuration(owner,next),"protected entry role");next=*changed.value;next.commands.push_back({app::MenuId::main,'K',77,"Missing Door",true,{}});check(!db.change_chat_configuration(owner,next),"unregistered Door refused");
    next=*changed.value;next.exclude_sysop_statistics=false;check(db.change_chat_configuration(owner,next).succeeded(),"statistics Off saved");
    app::DoorConfigurationChange door_change;door_change.operation=id(990);door_change.expected_revision=db.door_configuration(owner).value->revision;door_change.create_only=true;
    door_change.door.id=77;door_change.door.name="Command-only Door";door_change.door.enabled=true;door_change.door.rlogin.host="example.invalid";
    check(db.change_door_configuration(owner,door_change).succeeded(),"shared unlisted Door created");
    next=*db.chat_configuration(owner).value;
    for(const auto menu:{app::MenuId::main,app::MenuId::messages,app::MenuId::files,app::MenuId::chat,app::MenuId::doors})next.commands.push_back({menu,'K',77,"Play named Door",true,{}});
    check(db.change_chat_configuration(owner,next).succeeded(),"same key on five menus points to one shared Door");
    check(db.door_menu(alice,0).value->entries.empty(),"command-only Door has no numbered placement");
    app::MenuDoorSelection selected;
    for(const auto menu:{app::MenuId::main,app::MenuId::messages,app::MenuId::files,app::MenuId::chat,app::MenuId::doors}){
        const auto commands=db.menu_door_commands(alice,menu);check(commands&&commands.value->commands.size()==1&&commands.value->commands[0].name=="Play named Door","authorized named menu command");
        selected={menu,'K',commands.value->revision,commands.value->door_revision};const auto launch=db.menu_door_launch(alice,selected);
        check(launch&&launch.value->door.id==77&&launch.value->menu.name==app::menu_definition(menu).title,"unlisted shared Door authorized without fake numbered placement");
    }
    check(!db.menu_door_commands(spoof,app::MenuId::main)&&!db.menu_door_launch(spoof,selected),"command read and launch reject actor spoof");
    auto stale_selection=selected;--stale_selection.revision;check(!db.menu_door_launch(alice,stale_selection),"stale Chat placement revision rejected");
    stale_selection=selected;--stale_selection.door_revision;check(!db.menu_door_launch(alice,stale_selection),"stale shared Door revision rejected");
    next=*db.chat_configuration(owner).value;next.commands.back().access.level=11;check(db.change_chat_configuration(owner,next).succeeded(),"extra placement restriction saved");
    auto restricted=db.menu_door_commands(alice,app::MenuId::doors);check(restricted&&restricted.value->commands.empty(),"extra restriction hides only that placement");
    check(db.menu_door_commands(alice,app::MenuId::chat).value->commands.size()==1,"Chat placement unaffected by Door menu restriction");
    selected={app::MenuId::doors,'K',restricted.value->revision,restricted.value->door_revision};check(!db.menu_door_launch(alice,selected),"manual guess cannot bypass placement requirement");
    door_change.create_only=false;door_change.operation=id(991);door_change.expected_revision=db.door_configuration(owner).value->revision;door_change.door.execution.level=11;
    check(db.change_door_configuration(owner,door_change).succeeded(),"shared execution requirement changed");
    const auto execute=db.menu_door_commands(alice,app::MenuId::chat);selected={app::MenuId::chat,'K',execute.value->revision,execute.value->door_revision};
    check(!db.menu_door_launch(alice,selected)&&db.menu_door_launch(sysop,selected).succeeded(),"execution requirement enforced for command-only Door");
    door_change.operation=id(992);door_change.expected_revision=db.door_configuration(owner).value->revision;door_change.door.enabled=false;
    check(db.change_door_configuration(owner,door_change).succeeded(),"disable shared target");
    check(db.menu_door_commands(sysop,app::MenuId::chat).value->commands.empty()&&!db.menu_door_launch(sysop,selected),"disabled target hidden and old launch refused");
    check(db.shutdown().succeeded()&&db.start().succeeded(),"reopen");check(db.chat_preferences(alice).value->dnd&&!db.chat_preferences(alice).value->notices&&db.chat_preferences(sysop).value->ghost,"preferences survive reopen");
    check(!db.chat_configuration(owner).value->exclude_sysop_statistics&&db.chat_configuration(owner).value->rooms[1].name=="Games","explicit saved config retained");
    check(scalar(file,"SELECT count(*) FROM sfx_account_sessions")==3&&scalar(file,"SELECT count(*) FROM sfx_chat_usage_exclusions")==0,"no rewrite of historical sessions");
    auto allowance=db.private_chat_allowance(alice,"2026-10-27");check(allowance&&allowance.value&&!allowance.value->minutes_per_chat&&!allowance.value->seconds_remaining_today,"private limits default unlimited");
    check(!db.private_chat_allowance(spoof,"2026-10-27")&&!db.private_chat_allowance(alice,"2026-02-30"),"private allowance rejects spoof and invalid date");
    app::PublishSecurityPolicyRequest policy;policy.expected_revision=db.security_policy().value->revision;policy.levels=*db.security_levels().value;
    for(auto& level:policy.levels)if(level.level==10){level.private_chat_minutes=1;level.private_chat_minutes_per_day=2;}
    policy.new_user_level=10;policy.sysop_threshold=999;policy.policy_hash=policy.published_policy_hash=std::string(64,'a');policy.audit={id(1100),id(1101),{},1792248000,app::AuditReasonCode::security_policy};
    check(db.publish_security_policy(policy).succeeded(),"private limits publish with normal security policy");
    auto current=alice;++current.policy_revision;
    allowance=db.private_chat_allowance(current,"2026-10-27");check(allowance&&allowance.value->minutes_per_chat==1&&allowance.value->seconds_remaining_today==120,"limits enforced from current security level");
    check(db.security_level(10).value->private_chat_minutes_per_day==2,"single level reads saved private limits");
    check(!db.private_chat_allowance(alice,"2026-10-27"),"stale policy cannot read allowance");
    check(db.persist_private_chat_usage(id(2),60,{{"2026-10-27",60}}).succeeded(),"private elapsed seconds persisted");
    check(db.persist_private_chat_usage(id(2),60,{{"2026-10-27",60}}).succeeded(),"lost-reply retry never charges twice");
    check(!db.persist_private_chat_usage(id(2),59,{{"2026-10-27",59}})&&!db.persist_private_chat_usage(id(2),60,{}),"counter regression and missing days rejected");
    check(db.private_chat_allowance(current,"2026-10-27").value->seconds_remaining_today==60,"daily balance subtracts private seconds");
    check(db.persist_private_chat_usage(id(2),120,{{"2026-10-27",120}}).succeeded()&&db.private_chat_allowance(current,"2026-10-27").value->seconds_remaining_today==0,"daily exhausted without changing caller session");
    check(db.private_chat_allowance(current,"2026-10-28").value->seconds_remaining_today==120,"new local day restores daily allowance");
    check(db.persist_private_chat_usage(id(2),150,{{"2026-10-27",120},{"2026-10-28",30}}).succeeded(),"midnight usage keeps both days");
    check(!db.persist_private_chat_usage(id(2),150,{{"2026-10-28",150}})&&!db.persist_private_chat_usage(id(9999),1,{{"2026-10-28",1}}),"old day cannot be discarded and unknown session rejected");
    check(scalar(file,"SELECT sum(persisted_seconds) FROM sfx_account_sessions")==0,"private counters do not replace normal caller accounting");
    check(db.shutdown().succeeded()&&db.start().succeeded(),"private limits and usage reopen");
    check(db.private_chat_allowance(current,"2026-10-28").value->seconds_remaining_today==90,"private usage survives reopen");
    check(db.shutdown().succeeded(),"close");chat_statistics_checks(root/"statistics",paths);std::cout<<"PASS "<<checks<<" isolated Chat storage checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
