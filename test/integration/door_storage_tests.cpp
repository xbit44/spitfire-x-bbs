// Reuse only the isolated SQLite fixture builders. Its earlier runner is never
// called, and these synthetic actors are not installed authentication evidence.
#include "sfx/application/local_control_protocol.hpp"
#include "sfx/application/door_session.hpp"
#include "sfx/application/chat.hpp"
#define main retained_file_storage_door_fixture
#include "file_storage_tests.cpp"
#undef main
#include "sfx/application/local_control_protocol.hpp"
#include "sfx/application/door_session.hpp"

namespace {
// Independent immutable v1/v2 fixture writer. Never derive old records by
// truncating the current writer; additional fields would invalidate that test.
std::vector<std::uint8_t> legacy_storage_door(unsigned version,const app::DoorDefinition& d){
    std::vector<std::uint8_t> out;
    const auto number=[&](std::uint64_t n){for(unsigned i=0;i<8;++i)out.push_back(static_cast<std::uint8_t>(n>>(i*8)));};
    const auto bytes=[&](const auto& value){number(value.size());out.insert(out.end(),value.begin(),value.end());};
    number(version);number(d.id);bytes(d.name);number(d.enabled);number(static_cast<unsigned>(d.security_mode));number(d.security_level);number(d.sysop_only);
    const auto& r=d.rlogin;bytes(r.host);number(r.port);number(static_cast<unsigned>(r.authentication));number(static_cast<unsigned>(r.server_identity));
    bytes(r.server_name);bytes(r.client_name);bytes(r.terminal_type);bytes(r.salt);bytes(r.protected_credential);bytes(r.protected_pepper);
    number(r.prompt_credential);number(r.connection_timeout_seconds);number(r.quiet);number(r.banner);number(r.pause);
    if(version==2){number(static_cast<unsigned>(d.execution.mode));number(d.execution.level);number(d.execution.sysop_only);}return out;
}
app::DoorDefinition sample_door(unsigned n){app::DoorDefinition d;d.id=static_cast<app::DoorId>(n);d.name="Native outbound Door";d.enabled=true;d.rlogin.host="example.invalid";return d;}
void migration16(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& paths){
    for(const char* point:{"before-door-migration","before-door-migration-commit","after-door-migration-commit"}){
        auto s=settings(root/point);auto previous=storage::sfx_storage_migrations();previous.resize(15);
        {storage::SqliteStorage old(s,paths,previous);check(old.start().succeeded()&&old.health().schema_version==15,"exact v15 predecessor");check(old.shutdown().succeeded(),"v15 fixture stopped");}
        seed(s.data_directory/"SFX.DB");
        s.bootstrap_boundary_hook=[point](std::string_view at){return at==point?app::StorageStatus{app::StorageErrorCode::io,"injected Door migration fault"}:app::StorageStatus::success();};
        {storage::SqliteStorage failed(s,paths);check(!failed.start(),"Door migration interruption reported");}
        {internal::SqliteConnection c(s.data_directory/"SFX.DB",SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);
            check(internal::validate_sfx_shipping_database(c.get(),true),"interrupted upgrade leaves exact v15 or v16");
            check(c.scalar_int64("PRAGMA user_version","atomic Door upgrade")== (std::string_view(point)=="after-door-migration-commit"?16:15),"Door commit boundary atomic");}
        s.bootstrap_boundary_hook={};storage::SqliteStorage retry(s,paths);check(retry.start().succeeded()&&retry.health().schema_version==27,"Door migration retries exactly once through current schema");check(retry.shutdown().succeeded(),"upgraded fixture stopped");
        check(scalar(s.data_directory/"SFX.DB","SELECT count(*) FROM sfx_accounts")==3&&scalar(s.data_directory/"SFX.DB","SELECT count(*) FROM sfx_schema_migrations")==27,"accounts and exact migration ledger preserved");
    }
}
}
#include "door_owner_storage.inc"
#include "door_first_run_defaults.inc"
#include "door_program_storage.inc"
#include "door_category_delete.inc"
int main(int argc,char** argv){try{
    check(argc==2,"fresh Door fixture root required");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"preserve earlier fixtures");
    sfx::test::FakeStoragePathPolicy paths;door_first_run_defaults(root/"first-run-defaults",paths);
    category_delete_storage_checks(root/"category-delete",paths);
    migration16(root/"migration16",paths);
    migration17(root/"migration17",paths);
    migration18(root/"migration18",paths);
    auto s=settings(root/"primary");storage::SqliteStorage db(s,paths);check(db.start().succeeded()&&db.health().schema_version==27,"fresh exact schema27");check(db.shutdown().succeeded(),"stop before synthetic actors");const auto file=s.data_directory/"SFX.DB";seed(file);check(db.start().succeeded(),"start seeded Door fixture");
    const app::MessageConfigurationActor owner{true,{}};const app::MessageActor alice{id(2),2,1,1},sysop{id(1),1,1,1};
    auto c=db.door_configuration(owner);check(c&&c.value&&c.value->doors.empty()&&c.value->entries.empty()&&c.value->menus.size()==1&&c.value->menus.front().id==0,"fresh root only; no configured target/account/credential");
    check(!db.door_configuration({false,alice}),"ordinary caller cannot read protected Door configuration");
    app::DoorConfigurationChange change;unsigned operation=500;const auto prepare=[&]{c=db.door_configuration(owner);check(c&&c.value,"fresh configuration read");change={};change.operation=id(operation++);change.expected_revision=c.value->revision;};
    prepare();change.create_only=true;change.door=sample_door(1);const auto first=db.change_door_configuration(owner,change);check(first&&first.value,"Door definition saved through storage authority");
    const auto replay=db.change_door_configuration(owner,change);check(replay&&replay.value->replayed&&replay.value->revision==first.value->revision,"exact UUID receipt replay");
    change.door.name="Altered";check(!db.change_door_configuration(owner,change),"changed UUID reuse denied");change.operation=id(operation++);check(!db.change_door_configuration(owner,change),"stale catalog CAS denied");
    prepare();change.create_only=true;change.door=sample_door(1);change.door.name="Must not overwrite";check(!db.change_door_configuration(owner,change)&&app::find_door(*db.door_configuration(owner).value,1)->name=="Native outbound Door","create-only duplicate ID cannot overwrite saved definition");
    prepare();change.door=sample_door(999);check(!db.change_door_configuration(owner,change),"edit of missing permanent ID cannot become an implicit create");
    prepare();change.create_only=true;change.action=app::DoorConfigurationAction::save_menu;change.menu={1,"Games"};check(db.change_door_configuration(owner,change).succeeded(),"new permanent submenu");
    prepare();change.create_only=true;change.action=app::DoorConfigurationAction::save_entry;change.entry={0,7,app::DoorEntryKind::submenu,1,"Games",""};check(db.change_door_configuration(owner,change).succeeded(),"root references submenu");
    prepare();change.create_only=true;change.action=app::DoorConfigurationAction::save_entry;change.entry={1,42,app::DoorEntryKind::door,1,"Play","Context only"};check(db.change_door_configuration(owner,change).succeeded(),"gapped stable Door placement");
    const auto menu=db.door_menu(alice,1);check(menu&&menu.value->entries.size()==1&&!menu.value->has_hidden_entries,"authorized menu carries placement only");
    auto launch=db.door_launch(alice,{1,42,menu.value->revision});check(launch&&launch.value->door.id==1&&launch.value->menu.id==1&&launch.value->entry.number==42,"launch captures exact menu and shared definition");
    owner_storage_checks(db,owner,alice,sysop,prepare,change);
    prepare();change.door=*app::find_door(*c.value,1);change.door.automatic_event=app::DoorAutomaticEvent::logon;change.door.event_only=true;
    check(db.change_door_configuration(owner,change).succeeded(),"automatic event setting saved");
    c=db.door_configuration(owner);check(db.door_menu(alice,1).value->entries.empty()&&!db.door_launch(alice,{1,42,c.value->revision}),"SQLite excludes event-only from manual selection and direct manual launch");
    const auto events=db.door_events(alice,app::DoorAutomaticEvent::logon);check(events&&events.value->doors==std::vector<app::DoorId>{1},"authenticated event lookup finds shared Door once");
    check(db.door_event_launch(alice,{1,app::DoorAutomaticEvent::logon,events.value->revision}).succeeded(),"event-only launches through authorized event path");
    check(!db.door_event_launch(alice,{1,app::DoorAutomaticEvent::logoff,events.value->revision})&&!db.door_event_launch(alice,{1,app::DoorAutomaticEvent::logon,events.value->revision-1}),"wrong event and stale event snapshot denied");
    auto invalid_actor=alice;invalid_actor.account=999;check(!db.door_events(invalid_actor,app::DoorAutomaticEvent::logon),"unbound event actor denied");
    prepare();change.door=*app::find_door(*c.value,1);change.door.event_only=false;change.door.automatic_event=app::DoorAutomaticEvent::none;check(db.change_door_configuration(owner,change).succeeded(),"event fixture returns to manual mode");
    prepare();change.door=*app::find_door(*c.value,1);change.door.enabled=false;check(db.change_door_configuration(owner,change).succeeded(),"disable blocks new launches");
    check(!db.door_launch(alice,{1,42,menu.value->revision}),"stale pre-disable launch rejected");const auto hidden=db.door_menu(alice,1);check(hidden&&hidden.value->entries.empty()&&hidden.value->has_hidden_entries,"ineligible entries hidden and custom art suppressed");
    check(launch.value->door.enabled,"active captured launch remains coherent after disable");
    prepare();change.door=*app::find_door(*c.value,1);change.door.enabled=true;change.door.sysop_only=true;check(db.change_door_configuration(owner,change).succeeded(),"shared Sysop-only setting");
    check(db.door_menu(alice,1).value->entries.empty()&&db.door_menu(sysop,1).value->entries.size()==1,"same live access controls visibility");
    prepare();change.action=app::DoorConfigurationAction::remove_door;change.door.id=1;check(!db.change_door_configuration(owner,change),"referenced Door cannot be removed");
    prepare();change.action=app::DoorConfigurationAction::save_entry;change.entry={1,50,app::DoorEntryKind::door,1,"Moved",""};change.previous_number=42;check(db.change_door_configuration(owner,change).succeeded(),"explicit local number move atomic");
    check(db.door_menu(sysop,1).value->entries.front().number==50,"move leaves no old placement and no automatic renumber");
    prepare();change.action=app::DoorConfigurationAction::duplicate_menu;change.source_menu=1;change.menu={2,"Shared copy"};check(db.change_door_configuration(owner,change).succeeded(),"duplicate menu references atomically; starts unattached");
    c=db.door_configuration(owner);check(c.value->doors.size()==1&&std::count_if(c.value->entries.begin(),c.value->entries.end(),[](const auto& e){return e.menu==2&&e.target==1;})==1,"duplicate has shared target only");
    prepare();change.create_only=true;change.action=app::DoorConfigurationAction::save_entry;change.entry={1,1,app::DoorEntryKind::submenu,1,"Cycle",""};check(!db.change_door_configuration(owner,change),"cycle rejected transactionally");
    check(db.door_configuration(owner).value->revision==change.expected_revision,"rejected validation changes no revision");
    prepare();change.door=sample_door(2);check(!db.change_door_configuration({false,alice},change),"caller cannot mutate catalog");
    app::LocalDoorSnapshotCache cache;check(cache.query(db,app::LocalAccountAuthority::none,{}).code==app::LocalAccountResultCode::unauthorized&&
        cache.query(db,app::LocalAccountAuthority::elevated_administrator,{}).code==app::LocalAccountResultCode::unauthorized,"only installation owner has Config RPC authority");
    const auto page=cache.query(db,app::LocalAccountAuthority::installation_owner,{});check(page.code==app::LocalAccountResultCode::ok&&app::decode_door_catalog(page.bytes).has_value(),"bounded owner snapshot round trip");
    check(app::decode_door_configuration_snapshot(app::encode_door_configuration_snapshot(page)).has_value(),"owner snapshot survives strict IPC");
    for(auto action:{app::DoorConfigurationAction::save_door,app::DoorConfigurationAction::save_menu,app::DoorConfigurationAction::save_entry,app::DoorConfigurationAction::remove_door,app::DoorConfigurationAction::remove_menu,app::DoorConfigurationAction::remove_entry,app::DoorConfigurationAction::duplicate_menu}){
        app::DoorConfigurationChange wire;wire.operation=id(operation++);wire.expected_revision=3;wire.action=action;wire.door=sample_door(8);wire.menu={5,"Example"};wire.entry={0,9,app::DoorEntryKind::door,1,"Example",""};wire.source_menu=action==app::DoorConfigurationAction::duplicate_menu?1:0;
        const auto encoded=app::encode_door_configuration_change(wire);check(!encoded.empty()&&app::decode_door_configuration_change(encoded).has_value(),"all configuration actions survive IPC");
        for(std::size_t n=0;n<encoded.size();++n){auto short_record=encoded;short_record.resize(n);check(!app::decode_door_configuration_change(short_record),"all action record truncations rejected");}
    }
    prepare();change.create_only=true;change.door=sample_door(900);change.door.name="Unfinished slot";change.door.rlogin.host.clear();
    check(app::valid_door_definition(change.door)&&!app::valid_rlogin_target(change.door.rlogin),"saved slot is valid configuration but not a runnable connection");
    check(db.change_door_configuration(owner,change).succeeded(),"real SQLite accepts an empty saved Door slot");
    prepare();change.action=app::DoorConfigurationAction::save_entry;change.create_only=true;change.entry={0,97,app::DoorEntryKind::door,900,"Unfinished slot",""};
    check(db.change_door_configuration(owner,change).succeeded(),"real SQLite places the unfinished slot without a fake command");
    c=db.door_configuration(owner);const auto* slot=app::find_door(*c.value,900);
    check(slot&&slot->rlogin.host.empty()&&slot->enabled&&!app::door_accessible(*slot,{999,999}),"unfinished slot persists its enabled preference while remaining absent from caller choices");
    check(!db.door_launch(sysop,{0,97,c.value->revision}),"direct qualified launch refuses an unfinished slot before transport");
    const auto slot_bytes=app::encode_door_definition(*slot);check(app::decode_door_definition(slot_bytes).has_value(),"version3 record round trips an empty slot");
    check(db.shutdown().succeeded()&&db.start().succeeded(),"restart with unfinished slot");
    c=db.door_configuration(owner);check(app::find_door(*c.value,900)&&app::find_door(*c.value,900)->rlogin.host.empty(),"unfinished slot survives restart exactly");
    prepare();change.door=*app::find_door(*c.value,900);change.door.rlogin.host="completed.example.invalid";
    check(db.change_door_configuration(owner,change).succeeded(),"configure the same saved slot later");
    c=db.door_configuration(owner);check(app::door_accessible(*app::find_door(*c.value,900),{999,999}),"completed slot becomes eligible without changing identity or placement");
    const auto before=app::encode_door_catalog(*db.door_configuration(owner).value);check(db.shutdown().succeeded()&&db.start().succeeded(),"normal restart keeps accepted Sysop Door configuration");
    check(app::encode_door_catalog(*db.door_configuration(owner).value)==before,"every catalog/protected field byte survives upgrade reentry");check(db.shutdown().succeeded(),"normal final shutdown");
    check(scalar(file,"SELECT count(*) FROM pragma_foreign_key_check")==0,"final FK check clean");std::cout<<"PASS "<<checks<<" Door SQLite/IPC checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
