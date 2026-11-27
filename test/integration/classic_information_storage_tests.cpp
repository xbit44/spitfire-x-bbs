#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/application/local_classic_information.hpp"
#include "sfx/test/shipping_schema.hpp"
#include "sfx/test/storage_fakes.hpp"
#include "sqlite_internal.hpp"
#include "sqlite_sfx_baseline.hpp"
#include <filesystem>
#include <iostream>
#include <thread>

namespace app=sfx::application;
namespace db=sfx::adapters::storage;
namespace internal=db::internal;
unsigned checks=0;
void check(bool yes,const char* message) {++checks;if(!yes)throw std::runtime_error(message);}
app::Identifier128 identifier(unsigned n) {app::Identifier128 v;v.bytes[0]=7;v.bytes[14]=static_cast<std::uint8_t>(n>>8);v.bytes[15]=static_cast<std::uint8_t>(n);return v;}
struct Clock:app::IClock {std::int64_t unix_seconds()const noexcept override{return 1791150000;}std::uint64_t monotonic_millis()const noexcept override{return 0;}};
struct Identifiers:app::IIdentifierGenerator {unsigned n=500;app::Identifier128 generate()override{return identifier(++n);}};
db::SqliteStorageSettings options(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);db::SqliteStorageSettings s;s.data_directory=root;s.utc_now=[]{return std::int64_t{1791150000};};return s;
}
std::vector<db::SqliteMigration> legacy_plan() {
    auto plan=db::sfx_storage_migrations();plan.resize(6);return plan;
}
void legacy(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    internal::SqliteConnection c(root/"SFX.DB",SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,std::chrono::milliseconds(100),true);
    const auto plan=db::sfx_storage_migrations();
    for(int n=0;n<4;++n) {
        const auto& m=plan[n];c.exec(m.sql,"legacy schema");
        c.exec("INSERT INTO sfx_schema_migrations VALUES("+std::to_string(n+1)+",'"+m.name+"','"+m.content_sha256+"',1700000000)","legacy ledger");
    }
    c.exec("PRAGMA application_id=1397119042;PRAGMA user_version=4","legacy identity");
    c.exec("INSERT INTO sfx_accounts(account_id,display_name,name_key,security_level,account_state,revision,purge_allowed,created_utc_seconds,modified_utc_seconds) VALUES(1,'Existing','existing',10,'active',1,1,1700000000,1700000000)","legacy caller");
    c.exec("UPDATE sfx_account_menu_preferences SET color=5,hotkey=0,lightbar=0 WHERE account_id=1","legacy choices");
    check(internal::validate_sfx_shipping_database(c.get(),true),"exact unchanged v4 predecessor");
}
void version5(const std::filesystem::path& root,const std::filesystem::path& backup={}) {
    legacy(root);sfx::test::FakeStoragePathPolicy policy;
    auto plan=db::sfx_storage_migrations();plan.resize(5);
    db::SqliteStorage prior(options(root),policy,plan);
    check(prior.start().succeeded() && prior.health().schema_version==5,"exact frozen v5 predecessor starts");
    app::ClassicInformationChange add;add.values={"Retained Caller Board","retained.example:2323",""};
    check(prior.change_classic_information(add,{false,1,1,1791150000,identifier(700)}).succeeded(),"v5 caller audit seeded through approved operation");
    app::ClassicInformationChange opening{app::ClassicInformationAction::opening_date,0,1,{},"1984-05-12"};
    check(prior.change_classic_information(opening,{true,0,0,1791150000,identifier(701)}).succeeded(),"v5 configured date seeded through owner boundary");
    if(!backup.empty())check(prior.create_backup(backup).status.succeeded(),"v5 recovery backup created");
    check(prior.shutdown().succeeded(),"v5 predecessor stopped");
}
struct TextFixture:app::IOtherBoardTextStorage {
    std::vector<app::OtherBoardValues> rows;
    std::vector<app::OtherBoardLogRecord> logs;
    std::int64_t revision=1;
    app::StorageResult<app::OtherBoardPage> read(std::int64_t after) override {
        app::OtherBoardPage page;
        for(auto n=static_cast<std::size_t>(after);n<rows.size();++n)page.entries.push_back({static_cast<std::int64_t>(n+1),revision,rows[n]});
        return {{},page};
    }
    app::StorageResult<app::OtherBoardLogRecord> change(const app::ClassicInformationChange& change,
        const app::ClassicInformationActor& actor,std::string_view name) override {
        if(change.action==app::ClassicInformationAction::add_board)rows.push_back(change.values);
        else {
            if(change.expected_revision!=revision || change.id<1 || static_cast<std::size_t>(change.id)>rows.size())
                return {{app::StorageErrorCode::conflict,"stale text selection"},{}};
            rows.erase(rows.begin()+static_cast<std::ptrdiff_t>(change.id-1));
        }
        ++revision;return {{},app::OtherBoardLogRecord{change.action,"Text Board",std::string(name),actor.caller,actor.now_utc,actor.event_id}};
    }
    bool publish_log(const app::OtherBoardLogRecord& record) noexcept override {logs.push_back(record);return true;}
};
void text_transition(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy) {
    for(const auto* point:{"none","before-other-bbs-text-migration","before-other-bbs-text-migration-commit","after-other-bbs-text-migration-commit"}) {
        auto settings=options(root/point);version5(settings.data_directory);
        const auto backup=root/(std::string(point)+"-v6.db");
        {db::SqliteStorage v6(settings,policy,legacy_plan());check(v6.start().succeeded(),"exact v6 predecessor starts");
            check(v6.create_backup(backup).status.succeeded() && v6.shutdown().succeeded(),"v6 backup retained before retirement");}
        if(std::string_view(point)!="none") {
            settings.bootstrap_boundary_hook=[point](std::string_view at) {return at==point?app::StorageStatus{app::StorageErrorCode::io,"injected v7 boundary"}:app::StorageStatus::success();};
            {db::SqliteStorage failed(settings,policy);check(!failed.start(),"v7 interruption refuses startup");}
            internal::SqliteConnection retained(settings.data_directory/"SFX.DB",SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);
            const bool committed=std::string_view(point)=="after-other-bbs-text-migration-commit";
            check(internal::validate_sfx_shipping_database(retained.get(),true),"interruption keeps exact v6 or v7 lineage");
            check(retained.scalar_int64("PRAGMA user_version","retirement version")== (committed?7:6),"retirement transaction atomic");
            check(retained.scalar_int64("SELECT count(*) FROM sfx_other_boards","legacy catalog")== (committed?0:1),"interruption never partially retires catalog");
            check(retained.scalar_int64("SELECT count(*) FROM sfx_classic_information_audit WHERE action=3","opening audit")==1,"opening audit survives interruption");
        }
        settings.bootstrap_boundary_hook={};auto text=std::make_shared<TextFixture>();settings.other_board_text=text;
        db::SqliteStorage current(settings,policy);
        check(current.start().succeeded() && current.health().schema_version==sfx::test::shipping_schema_version,"v7 retirement retries through current shipping schema");
        check(current.other_boards(0).value->entries.empty(),"no import of old test catalog");
        check(current.opening_date().value->configured_date=="1984-05-12" && current.opening_date().value->initialization_utc==1700000000,"configured and original dates preserved");
        check(current.menu_preferences(1).value->preferences.color==5 && !current.menu_preferences(1).value->preferences.hotkey,"account choices preserved");
        app::ClassicInformationChange add;add.values={"Text Board","text.example:23",""};
        app::ClassicInformationActor caller{false,1,1,1791150000,identifier(801)};
        check(current.change_classic_information(add,caller).succeeded() && text->rows.size()==1,"current add uses text backend");
        check(text->logs.size()==1 && text->logs[0].caller==1 && text->logs[0].actor_name=="Existing","actual caller identity reaches log publisher");
        app::ClassicInformationChange remove{app::ClassicInformationAction::remove_board,1,text->revision,{},{}};
        check(!current.change_classic_information(remove,caller) && text->rows.size()==1,"current level10 delete denied before file changes");
        caller.caller_revision=2;
        check(!current.change_classic_information(add,caller) && text->logs.size()==1,"stale account cannot add or log");
        check(current.shutdown().succeeded(),"authority boundary stops for account change");
        {internal::SqliteConnection edit(settings.data_directory/"SFX.DB",SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
            check(edit.scalar_int64("SELECT count(*) FROM sfx_other_boards","retired catalog")==0 &&
                edit.scalar_int64("SELECT count(*) FROM sfx_classic_information_audit WHERE action IN(0,1,2)","retired audit")==0,"no new database board catalog/audit");
            edit.exec("UPDATE sfx_accounts SET security_level=999,revision=2 WHERE account_id=1","current authority fixture");}
        check(current.start().succeeded(),"current restarts with existing text list");
        caller.event_id=identifier(802);remove.expected_revision=1;
        check(!current.change_classic_information(remove,caller),"current stale file selection refused");
        remove.expected_revision=text->revision;
        check(current.change_classic_information(remove,caller).succeeded() && text->rows.empty() && text->logs.size()==2,"current999 removes text and logs caller identity");
        check(current.shutdown().succeeded(),"current stops before recovery");
        {internal::SqliteConnection edit(settings.data_directory/"SFX.DB",SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
            edit.exec("UPDATE sfx_accounts SET account_state='locked',revision=3 WHERE account_id=1","current disabled account fixture");}
        check(current.start().succeeded(),"current disabled account fixture starts");
        caller.caller_revision=3;caller.event_id=identifier(803);
        check(!current.change_classic_information(add,caller) && text->rows.empty() && text->logs.size()==2,"current disabled account cannot publish text or a log even with matching revision");
        check(current.shutdown().succeeded(),"disabled account fixture stops before recovery");
        check(current.validate_backup(backup).succeeded() && current.restore_backup(backup).succeeded(),"exact v6 recovery accepted");
        check(current.start().succeeded() && current.health().schema_version==sfx::test::shipping_schema_version && current.other_boards(0).value->entries.empty(),"recovered v6 re-retires without importing");
        check(current.opening_date().value->configured_date=="1984-05-12" && current.shutdown().succeeded(),"recovery preserves unrelated information");
    }
}
int main(int argc,char** argv) {try {
    check(argc==2,"isolated fixture parent argument required");
    const auto root=std::filesystem::absolute(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    sfx::test::FakeStoragePathPolicy policy;Clock clock;Identifiers identifiers;
    auto fresh=options(root/"fresh");db::SqliteStorage storage(fresh,policy,legacy_plan());
    check(storage.start().succeeded() && storage.health().schema_version==6,"fresh migration6");
    const auto initial=storage.opening_date();check(initial && initial.value && initial.value->initialization_utc==1791150000 && !initial.value->configured_date,"fresh date uses actual initial Setup database time");
    app::LocalClassicInformationAdministration denied(storage,identifiers,clock,app::LocalAccountAuthority::elevated_administrator);
    check(denied.snapshot(0).code==app::LocalAccountResultCode::unauthorized,"different administrator has no classic catalog authority");
    app::LocalClassicInformationAdministration local(storage,identifiers,clock,app::LocalAccountAuthority::installation_owner);
    app::ClassicInformationChange add;add.values={"One Board","bbs.example:2323",""};
    check(local.execute(add).committed,"owner can add Telnet-only entry");
    add.values={"SSH Board","","ssh://bbs.example:2222"};check(local.execute(add).committed,"SSH-only nondefault port");
    add.values={"Both Board","bbs.example","[2001:db8::1]:2222"};check(local.execute(add).committed,"both addresses with IPv6 informational value");
    const auto first=storage.other_boards(0);check(first && first.value && first.value->entries.size()==3,"immediate published page");
    const auto saved=first.value->entries.front();
    app::ClassicInformationChange edit{app::ClassicInformationAction::edit_board,saved.id,saved.revision,{"Updated","new.example:24",""},{}};
    std::array<bool,2> wins{};std::array<std::thread,2> workers;
    for(unsigned n=0;n<2;++n)workers[n]=std::thread([&,n]{wins[n]=storage.change_classic_information(edit,{true,0,0,clock.unix_seconds(),identifier(20+n)}).succeeded();});
    for(auto& worker:workers)worker.join();
    check(wins[0]!=wins[1],"one concurrent revision save only");
    check(!local.execute(edit).committed,"stale catalog edit refused");
    app::ClassicInformationChange remove{app::ClassicInformationAction::remove_board,saved.id,2,{},{}};
    check(local.execute(remove).committed,"owner remove audited");
    check(!local.execute(remove).committed && !local.execute(edit).committed,"removed stable identity cannot be revived by stale edit");
    add.values={"Fourth","four.example",""};check(local.execute(add).committed,"later add succeeds");
    check(storage.other_boards(0).value->entries.back().id==4,"removed identity never reused");
    app::ClassicInformationActor unbound{false,1,1,clock.unix_seconds(),identifier(40)};
    check(!storage.change_classic_information(add,unbound),"unbound caller cannot publish");
    check(!storage.change_classic_information(edit,unbound),"caller cannot edit, including submitter");
    app::ClassicInformationActor duplicate{true,0,0,clock.unix_seconds(),identifier(41)};
    check(storage.change_classic_information(add,duplicate).succeeded(),"first audit identity commits");
    const auto count=storage.other_boards(0).value->entries.size();
    check(!storage.change_classic_information(add,duplicate) && storage.other_boards(0).value->entries.size()==count,"failed audit rolls back addition atomically");
    for(unsigned n=0;n<35;++n)check(local.execute(add).committed,"bounded paged entries");
    auto page=storage.other_boards(0);check(page.value->entries.size()==32 && page.value->has_more,"bounded first page");
    const auto next=storage.other_boards(page.value->entries.back().id);check(next && next.value && !next.value->has_more && next.value->entries.front().id>page.value->entries.back().id,"stable cursor without duplicates");
    app::ClassicInformationChange date{app::ClassicInformationAction::opening_date,0,initial.value->revision,{},"1984-05-12"};
    check(local.execute(date).committed && storage.opening_date().value->configured_date==date.date,"historical editable date stored");
    check(!local.execute(date).committed,"opening date CAS conflict");
    auto stats=storage.system_information_counters(1791129600,1791216000);check(stats && stats.value && stats.value->total_calls==0 && stats.value->today_calls==0 && stats.value->active_accounts==0,"real zero counters, not fabricated users or calls");
    const auto backup=root/"backup.db";check(storage.create_backup(backup).status.succeeded(),"exact v5 backup");
    check(storage.shutdown().succeeded() && storage.validate_backup(backup).succeeded(),"backup manifest/ledger verification");
    db::SqliteStorage reopened(fresh,policy,legacy_plan());check(reopened.start().succeeded() && reopened.opening_date().value->configured_date==date.date && reopened.other_boards(0).value->has_more,"catalog/date survive Runtime restart");
    check(reopened.shutdown().succeeded(),"fresh shutdown");
    auto upgraded=options(root/"upgrade");legacy(upgraded.data_directory);db::SqliteStorage old(upgraded,policy,legacy_plan());
    check(old.start().succeeded() && old.health().schema_version==6,"v4 upgrade additive exact schema");
    check(old.opening_date().value->initialization_utc==1700000000,"upgrade default retained INITIALIZATION date, not current migration");
    const auto preference=old.menu_preferences(1);check(preference.value->preferences.color==5 && !preference.value->preferences.hotkey && !preference.value->preferences.lightbar,"existing caller choices preserved");
    app::ClassicInformationActor caller{false,1,1,clock.unix_seconds(),identifier(60)};
    check(old.change_classic_information(add,caller).succeeded(),"active authoritative caller can add immediately");
    const auto own=old.other_boards(0).value->entries.front();
    edit.id=own.id;edit.expected_revision=own.revision;
    check(!old.change_classic_information(edit,caller),"original submitter still cannot edit");
    caller.caller_revision=2;caller.event_id=identifier(61);check(!old.change_classic_information(add,caller),"stale caller revision refuses publication");
    const auto active=old.system_information_counters(1791129600,1791216000);check(active.value->active_accounts==1,"real active caller counter");
    check(old.shutdown().succeeded(),"upgraded shutdown");
    const auto v5backup=root/"v5-recovery.db";version5(root/"v5-upgrade",v5backup);
    db::SqliteStorage upgrade5(options(root/"v5-upgrade"),policy,legacy_plan());
    check(upgrade5.start().succeeded() && upgrade5.health().schema_version==6,"exact v5 upgrades to6");
    check(upgrade5.other_boards(0).value->entries.front().values.name=="Retained Caller Board" &&
        upgrade5.opening_date().value->configured_date=="1984-05-12" && upgrade5.opening_date().value->initialization_utc==1700000000,
        "v5 catalog and historical/initialization dates retained");
    check(upgrade5.menu_preferences(1).value->preferences.color==5 && !upgrade5.menu_preferences(1).value->preferences.hotkey,
        "v5 saved account choices survive6");
    check(upgrade5.shutdown().succeeded() && upgrade5.validate_backup(v5backup).succeeded() && upgrade5.restore_backup(v5backup).succeeded(),
        "normal offline recovery accepts exact v5 backup after upgrade");
    check(upgrade5.start().succeeded() && upgrade5.health().schema_version==6 && upgrade5.other_boards(0).value->entries.front().id==1,
        "restored5 remigrates safely with stable identities");
    check(upgrade5.shutdown().succeeded(),"restored migrated6 stopped");
    {
        internal::SqliteConnection retained(root/"v5-upgrade/SFX.DB",SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);
        check(retained.scalar_int64("SELECT count(*) FROM sfx_classic_information_audit WHERE (action=0 AND caller_account_id=1 AND installation_owner=0) OR (action=3 AND caller_account_id IS NULL AND installation_owner=1)","retained prior audit")==2,
            "prior caller/owner audit records preserved, not reattributed");
    }
    for(const auto* point:{"before-classic-delete-migration","before-classic-delete-migration-commit","after-classic-delete-migration-commit"}) {
        auto interrupted=options(root/point);version5(interrupted.data_directory);
        interrupted.bootstrap_boundary_hook=[point](std::string_view boundary) {
            return boundary==point?app::StorageStatus{app::StorageErrorCode::io,"injected v6 boundary"}:app::StorageStatus::success();
        };
        {db::SqliteStorage failed(interrupted,policy,legacy_plan());check(!failed.start(),"injected v6 boundary refuses startup");}
        {
            internal::SqliteConnection retained(interrupted.data_directory/"SFX.DB",SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);
            check(internal::validate_sfx_shipping_database(retained.get(),true),"v6 boundary retains exact complete5 or6 schema and ledger");
            check(retained.scalar_int64("PRAGMA user_version","v6 retained version")== (std::string_view(point)=="after-classic-delete-migration-commit"?6:5),"atomic v6 boundary version");
            check(retained.scalar_int64("SELECT count(*) FROM sfx_classic_information_audit","v6 retained audit")==2,"interruption retains both prior audit records");
        }
        interrupted.bootstrap_boundary_hook={};db::SqliteStorage retry(interrupted,policy,legacy_plan());
        check(retry.start().succeeded() && retry.health().schema_version==6 && retry.other_boards(0).value->entries.size()==1,"retry6 neither duplicates nor loses catalog");
        check(retry.shutdown().succeeded(),"v6 retry stopped");
    }
    for(const auto level:{10,999}) {
        auto authorized=options(root/(level==999?"sysop-delete":"caller-delete"));legacy(authorized.data_directory);
        {
            internal::SqliteConnection setup(authorized.data_directory/"SFX.DB",SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
            setup.exec("UPDATE sfx_accounts SET security_level="+std::to_string(level)+" WHERE account_id=1","isolated authority fixture");
        }
        db::SqliteStorage deleting(authorized,policy,legacy_plan());check(deleting.start().succeeded(),"delete fixture starts");
        app::ClassicInformationActor actor{false,1,1,clock.unix_seconds(),identifier(100+level)};
        check(deleting.change_classic_information(add,actor).succeeded(),"each authenticated caller may still add");
        const auto shown=deleting.other_boards(0).value->entries.front();
        app::ClassicInformationChange deletion{app::ClassicInformationAction::remove_board,shown.id,shown.revision,{},{}};
        actor.event_id=identifier(200+level);actor.caller_revision=2;
        check(!deleting.change_classic_information(deletion,actor),"stale account revision cannot remove even at999");
        actor.caller_revision=1;
        if(level==10)check(!deleting.change_classic_information(deletion,actor) && deleting.other_boards(0).value->entries.size()==1,"non999 caller removal refused transactionally");
        else {
            deletion.expected_revision=2;
            check(!deleting.change_classic_information(deletion,actor),"stale entry revision cannot remove");
            deletion.expected_revision=1;
            actor.event_id=identifier(100+level);
            check(!deleting.change_classic_information(deletion,actor) && deleting.other_boards(0).value->entries.size()==1,"duplicate audit ID rolls back removal");
            std::array<bool,2> removed{};std::array<std::thread,2> racers;
            for(unsigned n=0;n<2;++n)racers[n]=std::thread([&,n]{auto a=actor;a.event_id=identifier(300+n);removed[n]=deleting.change_classic_information(deletion,a).succeeded();});
            for(auto& racer:racers)racer.join();
            check(removed[0]!=removed[1] && deleting.other_boards(0).value->entries.empty(),"concurrent999 deletes have exactly one winner");
        }
        check(deleting.shutdown().succeeded(),"delete authority fixture stopped");
        {
            internal::SqliteConnection retained(authorized.data_directory/"SFX.DB",SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
            check(retained.scalar_int64("SELECT count(*) FROM sfx_classic_information_audit WHERE action=2 AND caller_account_id=1 AND installation_owner=0","caller remove audit")== (level==999?1:0),"caller deletion durable audit is bound to identity, not installation-owner authority");
            retained.exec("UPDATE sfx_accounts SET account_state='locked',revision=revision+1 WHERE account_id=1","isolated authority revocation");
        }
        db::SqliteStorage revoked(authorized,policy,legacy_plan());check(revoked.start().succeeded(),"revoked fixture reopens");
        actor.caller_revision=2;actor.event_id=identifier(400+level);
        check(!revoked.change_classic_information(add,actor) && !revoked.change_classic_information(deletion,actor),"disabled caller cannot add/remove despite exact current revision");
        check(revoked.shutdown().succeeded(),"revoked fixture stopped");
    }
    for(const auto* point:{"before-classic-information-migration","before-classic-information-migration-commit","after-classic-information-migration-commit"}) {
        auto interrupted=options(root/point);legacy(interrupted.data_directory);
        interrupted.bootstrap_boundary_hook=[point](std::string_view boundary) {
            return boundary==point?app::StorageStatus{app::StorageErrorCode::io,"injected v5 boundary"}:app::StorageStatus::success();
        };
        {db::SqliteStorage failed(interrupted,policy,legacy_plan());check(!failed.start(),"injected v5 migration boundary refused startup");}
        const bool committed=std::string_view(point)=="after-classic-information-migration-commit";
        {
            internal::SqliteConnection retained(interrupted.data_directory/"SFX.DB",SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);
            check(internal::validate_sfx_shipping_database(retained.get(),true),"boundary preserves exact approved schema and ledger");
            check(retained.scalar_int64("PRAGMA user_version","retained migration")== (committed?5:4),"atomic boundary only old or complete new schema");
        }
        interrupted.bootstrap_boundary_hook={};db::SqliteStorage retry(interrupted,policy,legacy_plan());
        check(retry.start().succeeded() && retry.opening_date().value->initialization_utc==1700000000,"retry retains original initialization date");
        check(retry.shutdown().succeeded(),"migration retry stopped");
    }
    const auto tampered=root/"tampered.db";std::filesystem::copy_file(backup,tampered);
    {
        internal::SqliteConnection changed(tampered,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
        changed.exec("UPDATE sfx_bbs_opening_date SET initialization_utc=1791159999","isolated tampered initialization");
    }
    check(!reopened.validate_backup(tampered),"altered retained initialization cannot validate as an exact backup");
    text_transition(root/"text-transition",policy);
    std::cout<<"PASS "<<checks<<" classic storage checks (frozen v5/v6 plus current text authority/v7)\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
