#include "../support/include/sfx/test/linux_layout_fixture.hpp"
#include "sfx/application/chat.hpp"
// Reuse fixture builders, not the earlier File Manager runner.
#define main retained_file_manager_configuration_main
#include "linux_file_manager_tests.cpp"
#undef main
#include "sfx/platform/linux/file_configuration.hpp"
#include "sfx/application/local_control_protocol.hpp"
#include "linux_file_group_checks.inc"
int main(int argc,char** argv){try{
    check(argc==2,"fresh configuration fixture root required");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"preserve previous fixtures");
    const auto install=root/"install";
    for(const auto& p:{root,install,install/"config",install/"data",install/"data/fileintake",install/"data/fileremoved",install/"files",install/"files"/"general",install/"files"/"uploads",install/"files"/"uploadtosysop",root/"external"})check(std::filesystem::create_directory(p)&&chmod(p.c_str(),0700)==0,"fresh private configuration fixture");
    const auto config_directory=native::open_owned_directory(install/"config");const std::string first="SFX_LINUX_FIRST_RUN=1\n";
    native::publish_private_file(config_directory.get(),"first-run.v1",{first.begin(),first.end()},false);
    check(bool((sfx::test::mark_provisioned_linux_fixture(install),native::initialize_fresh_portable_board_key(install))),"configuration fixture key");
    const auto bindings=native::LinuxFilePayloadBindings::initialize_fresh(install);
    native::LinuxLocalStoragePathPolicy paths;storage::SqliteStorageSettings settings;settings.layout_profile=sfx::adapters::storage::SqliteStorageLayoutProfile::linux_lowercase_v1;settings.data_directory=install/"data";settings.utc_now=[]{return std::int64_t{1792248000};};storage::SqliteStorage db(settings,paths);
    check(db.start().succeeded()&&db.shutdown().succeeded(),"fresh production migrations");const auto database=install/"data"/"sfx.db";seed(database);check(db.start().succeeded(),"owned synthetic actors ready");
    native::LinuxFileConfigurationPaths safe_paths(install,bindings);app::LocalFileAdministration admin(db,app::LocalAccountAuthority::installation_owner,&safe_paths);
    app::LocalFileAdministration untrusted(db,app::LocalAccountAuthority::none,&safe_paths),recovery(db,app::LocalAccountAuthority::elevated_administrator,&safe_paths),no_paths(db,app::LocalAccountAuthority::installation_owner);
    check(untrusted.snapshot({}).code==app::LocalAccountResultCode::unauthorized&&recovery.snapshot({}).code==app::LocalAccountResultCode::unauthorized,"neither untrusted nor password-recovery authority reads configuration");
    auto snapshot=admin.snapshot({});check(snapshot.code==app::LocalAccountResultCode::ok&&!snapshot.policy.counts.ratio&&!snapshot.policy.counts.daily_downloads,"initial count policies Off without invented ratio");
    app::FileConfigurationChange change;const auto prepare=[&]{change={};change.operation=id(operations++);const auto current=admin.snapshot({});check(current.code==app::LocalAccountResultCode::ok,"current authoritative policy");change.expected_catalog_revision=current.policy.catalog_revision;change.policy=current.policy;};
    prepare();change.policy.maximum_upload_bytes=16777216;check(untrusted.execute(change).code==app::LocalAccountResultCode::unauthorized&&recovery.execute(change).code==app::LocalAccountResultCode::unauthorized,"no mutation under non-owner authority");
    auto changed=admin.execute(change);check(changed.code==app::LocalAccountResultCode::ok,"owner policy change");check(admin.execute(change).result.replayed,"exact mutation replay does not repeat it");auto stale=change;stale.operation=id(operations++);check(admin.execute(stale).code==app::LocalAccountResultCode::conflict,"stale distinct request conflicts");
    for(unsigned n=0;n<27;++n){prepare();change.action=app::FileConfigurationAction::group;change.group.name="Group "+std::to_string(n);change.group.order=100-n;check(admin.execute(change).code==app::LocalAccountResultCode::ok,"new group via typed owner service");}
    app::LocalFileConfigurationQuery query;query.part=app::FileConfigurationPart::groups;std::vector<std::int64_t> seen;
    do{snapshot=admin.snapshot(query);check(snapshot.code==app::LocalAccountResultCode::ok&&snapshot.groups.size()<=12,"bounded consistent group page");check(app::decode_file_configuration_snapshot(app::encode_file_configuration_snapshot(snapshot)).has_value(),"native configuration page survives IPC");for(const auto& g:snapshot.groups){check(seen.empty()||g.id.value>seen.back(),"keyset pages have no duplicate/omitted identity");seen.push_back(g.id.value);}query.catalog_revision=snapshot.policy.catalog_revision;query.after_id=snapshot.next_after;}while(query.after_id);
    check(seen.size()==28,"all configured groups returned across pages");
    prepare();change.policy.lightbar_default=false;check(admin.execute(change).code==app::LocalAccountResultCode::ok&&admin.snapshot(query).code==app::LocalAccountResultCode::conflict,"intervening catalog change invalidates continuation");
    query={};query.part=app::FileConfigurationPart::caller_exemption;query.account=2;snapshot=admin.snapshot(query);check(snapshot.code==app::LocalAccountResultCode::ok&&!snapshot.exemption->daily_exempt&&!snapshot.exemption->ratio_exempt,"absent account exemption reads explicit Off");
    check(!db.file_caller_exemption({false,app::FileActor{id(2),2,1,1}},2)&&!db.file_caller_exemption({true,{}},99999),"ordinary callers and missing accounts cannot read exemption metadata");
    prepare();change.action=app::FileConfigurationAction::caller_exemption;change.exemption=*snapshot.exemption;change.exemption.daily_exempt=true;check(admin.execute(change).code==app::LocalAccountResultCode::ok,"save exact caller exemption");snapshot=admin.snapshot(query);check(snapshot.exemption->daily_exempt&&snapshot.exemption->revision==2&&!snapshot.exemption->ratio_exempt,"exemption roundtrip carries current CAS revision");
    prepare();change.action=app::FileConfigurationAction::area;change.area.area.group={1};change.area.area.name="External Library";change.area.storage_path=(root/"external").u8string();
    check(no_paths.execute(change).code==app::LocalAccountResultCode::unavailable,"area mutations require native path capability");
    {const auto config=db.file_configuration({true,{}});auto hold=safe_paths.acquire(*config.value,change);check(static_cast<bool>(hold),"native existing external directory capability acquired");hold->verify();std::filesystem::rename(root/"external",root/"held-external");bool changed_path=false;try{hold->verify();}catch(...){changed_path=true;}std::filesystem::rename(root/"held-external",root/"external");check(changed_path,"Linux capability detects namespace substitution; no mandatory deny-sharing claim");}
    changed=admin.execute(change);check(changed.code==app::LocalAccountResultCode::ok,"safe external same-volume path saved without moving contents");const auto external_id=changed.result.identity;check(admin.execute(change).result.replayed,"exact area creation replay retained");
    prepare();change.action=app::FileConfigurationAction::area;change.area.area.group={1};change.area.area.name="Unsafe library";const auto before=change.expected_catalog_revision;
    for(const auto& path:{std::string("data"),std::string("files/general"),(root/"external").u8string(),std::string("files/general/Nested"),std::string("files/Missing")}){change.operation=id(operations++);change.area.storage_path=path;check(admin.execute(change).code==app::LocalAccountResultCode::policy_rejected,"protected/overlapping/missing directories refused");}
    check(admin.snapshot({}).policy.catalog_revision==before&&!std::filesystem::exists(install/"files"/"Missing"),"refusals neither publish nor create/adopt a directory");
    query={};query.part=app::FileConfigurationPart::areas;query.group={1};snapshot=admin.snapshot(query);check(snapshot.areas.size()==4,"area page includes private native configuration only under owner");
    auto external=std::find_if(snapshot.areas.begin(),snapshot.areas.end(),[&](const auto& a){return a.area.id.value==external_id;});check(external!=snapshot.areas.end(),"new permanent area identity returned");
    prepare();change.action=app::FileConfigurationAction::area;change.area=*external;change.area.area.enabled=false;change.area.area.name="External disabled";check(admin.execute(change).code==app::LocalAccountResultCode::ok,"metadata change leaves physical directory in place");
    check(std::filesystem::is_directory(root/"external")&&scalar(database,"SELECT count(*) FROM sfx_file_catalog")==0&&scalar(database,"SELECT count(*) FROM sfx_file_statistics")==0,"configuration operations fabricate no payload or accounting");
    check(db.shutdown().succeeded()&&db.start().succeeded(),"configuration survives DB restart");check(admin.snapshot({}).policy.maximum_upload_bytes==16777216,"saved maximum remains active");
    check(std::filesystem::create_directory(root/"lease-change")&&chmod((root/"lease-change").c_str(),0700)==0,"fresh lease race fixture");
    struct ReplacedLease final:app::IFileConfigurationPaths {
        native::LinuxFileConfigurationPaths& actual;std::filesystem::path source;
        ReplacedLease(native::LinuxFileConfigurationPaths& a,std::filesystem::path p):actual(a),source(std::move(p)){}
        std::unique_ptr<app::IFileConfigurationLease> acquire(const app::FileConfiguration& c,const app::FileConfigurationChange& change) override {
            auto lease=actual.acquire(c,change);std::filesystem::rename(source,std::filesystem::path(source.u8string()+"-retained"));return lease;
        }
    } replaced(safe_paths,root/"lease-change");
    app::LocalFileAdministration race(db,app::LocalAccountAuthority::installation_owner,&replaced);
    prepare();change.action=app::FileConfigurationAction::area;change.area.area.group={1};change.area.area.name="Changed folder";change.area.storage_path=(root/"lease-change").u8string();
    check(race.execute(change).code==app::LocalAccountResultCode::policy_rejected&&admin.snapshot({}).policy.catalog_revision==change.expected_catalog_revision,"namespace changed after acquisition refuses save without catalog publication");
    std::filesystem::rename(root/"lease-change-retained",root/"lease-change");
    group_checks(db,install,bindings);
    check(db.shutdown().succeeded(),"native config fixture stopped");{sql::SqliteConnection c(database,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);check(sql::validate_sfx_shipping_database(c.get(),true),"exact schema/integrity/foreign keys retained");}
    std::cout<<"PASS native file configuration "<<checks<<" checks; actual Linux filesystem/SQLite, synthetic owner/account actors, no GUI/installed relay claim\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL native configuration after "<<checks<<": "<<e.what()<<'\n';return 1;}}
