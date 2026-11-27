#include "sfx/adapters/storage/sqlite_storage.hpp"
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
void check(bool value,const char* message) { ++checks; if(!value) throw std::runtime_error(message); }
app::Identifier128 id(unsigned n) { app::Identifier128 value; value.bytes[0]=1; value.bytes[15]=static_cast<std::uint8_t>(n); return value; }
app::AuditContext audit(unsigned n) { return {id(n),id(n+100),{},1788990000,app::AuditReasonCode::initial_setup}; }
db::SqliteStorageSettings settings(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    db::SqliteStorageSettings result; result.data_directory=directory;
    result.utc_now=[] {return std::int64_t{1788990000};}; return result;
}
void legacy(const std::filesystem::path& directory,int version) {
    std::filesystem::create_directories(directory);
    internal::SqliteConnection seed(directory/"SFX.DB",SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,
        std::chrono::milliseconds(100),true);
    const auto migrations=db::sfx_storage_migrations();
    for (int n=0;n<version;++n) {
        const auto& migration=migrations[static_cast<std::size_t>(n)];
        seed.exec(migration.sql,"test legacy schema");
        seed.exec("INSERT INTO sfx_schema_migrations VALUES("+std::to_string(n+1)+",'"+migration.name+
            "','"+migration.content_sha256+"',1788990000)","test legacy ledger");
    }
    seed.exec("PRAGMA application_id=1397119042; PRAGMA user_version="+std::to_string(version),"legacy identity");
    seed.exec("INSERT INTO sfx_accounts(account_id,display_name,name_key,security_level,account_state,revision,purge_allowed,created_utc_seconds,modified_utc_seconds) "
        "VALUES(1,'Existing','existing',10,'active',1,1,1788990000,1788990000)","legacy caller");
    check(internal::validate_sfx_shipping_database(seed.get(),true),"exact legacy fixture before upgrade");
}
int main() {
    try {
        const auto root=std::filesystem::current_path()/"menu-preference-fixtures"/
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        sfx::test::FakeStoragePathPolicy paths;
        auto options=settings(root/"fresh");
        db::SqliteStorage storage(options,paths);
check(storage.start().succeeded() && storage.health().schema_version==sfx::test::shipping_schema_version,"fresh current shipping schema");
        app::CredentialEnvelope credential; credential.envelope_format_version=1; credential.scheme_identifier="menu-fixture";
        credential.parameter_encoding_version=1; credential.canonical_parameters={1}; credential.salt={1,2};
        credential.verifier={3,4}; credential.created_utc_seconds=1788990000;
        app::StageInitialSysopRequest stage; stage.display_name="Menu Sysop"; stage.credential=credential; stage.audit=audit(1);
        const auto staged=storage.stage_initial_sysop(stage); check(staged && staged.account_id,"create actual fixture account");
        const auto account=*staged.account_id;
        const auto policy=storage.security_policy(); check(policy && policy.value,"read policy");
        check(storage.complete_first_run({account,policy.value->revision,policy.value->policy_hash,
            policy.value->policy_hash,audit(2)}).succeeded(),"activate initial Sysop");
        auto before=storage.menu_preferences(account);
        check(before && before.value && before.value->revision==1 && before.value->preferences.color==0 &&
            before.value->preferences.hotkey && before.value->preferences.lightbar,"new account hotkeys default On");
        check(!storage.menu_preferences(0) && !storage.menu_preferences(9999),"invalid and unknown caller cannot read preferences");
        check(storage.update_menu_preferences({account,before.value->revision,{4,false,true},1788990001}).succeeded(),
            "existing Sysop explicitly saves hotkeys Off");
        app::CreateAccountRequest caller;caller.display_name="New Menu Caller";caller.credential=credential;
        caller.audit=audit(10);caller.audit.reason=app::AuditReasonCode::registration;
        const auto created=storage.create_account(caller);
        check(created && created.account_id && storage.menu_preferences(*created.account_id).value->preferences.hotkey,
            "ordinary new account also defaults hotkeys On");
        const auto retained=storage.menu_preferences(account);
        check(retained && retained.value->preferences.color==4 && !retained.value->preferences.hotkey,
            "new account creation does not reset an existing customized Off choice");
        for (unsigned color=0;color<16;++color) {
            auto current=storage.menu_preferences(account);
            check(storage.update_menu_preferences({account,current.value->revision,
                {static_cast<std::uint8_t>(color),color%2!=0,color%2==0},1788990001}).succeeded(),"save every approved palette index");
            check(storage.menu_preferences(account).value->preferences.color==color,"roundtrip exact choice");
        }
        auto current=storage.menu_preferences(account);
        check(!storage.update_menu_preferences({account,current.value->revision,{16,false,true},1788990001}),"reject invalid palette");
        check(!storage.update_menu_preferences({account,1,{1,false,true},1788990001}),"stale revision cannot overwrite");
        std::array<bool,2> success{};
        std::array<std::thread,2> writers;
        for (unsigned n=0;n<2;++n) writers[n]=std::thread([&,n] {
            success[n]=storage.update_menu_preferences({account,current.value->revision,
                {static_cast<std::uint8_t>(n+1),true,false},1788990002}).succeeded();
        });
        for(auto& writer:writers) writer.join();
        check(success[0]!=success[1],"only one concurrent save commits");
        const auto saved=storage.menu_preferences(account);
        const auto backup=root/"fresh-backup.db";
        check(storage.create_backup(backup).status.succeeded(),"back up exact current schema");
        check(storage.shutdown().succeeded(),"clean preference storage stop");
        check(storage.validate_backup(backup).succeeded(),"current preference backup validation");
        db::SqliteStorage reopened(options,paths);
        check(reopened.start().succeeded(),"reopen menu preference database");
        const auto recovered=reopened.menu_preferences(account);
        check(recovered && recovered.value->revision==saved.value->revision &&
            recovered.value->preferences.color==saved.value->preferences.color &&
            recovered.value->preferences.hotkey && !recovered.value->preferences.lightbar,"saved choices persist across restart");
        check(reopened.shutdown().succeeded(),"stop reopened preferences");
        for (int version:{1,2}) {
            const auto directory=root/("upgrade-v"+std::to_string(version));
            legacy(directory,version); auto migration_options=settings(directory);
            migration_options.bootstrap_boundary_hook=[](std::string_view point) {
                return point=="before-menu-migration-commit" ? app::StorageStatus{app::StorageErrorCode::io,"test interruption"} : app::StorageStatus::success();
            };
            { db::SqliteStorage failed(migration_options,paths); check(!failed.start(),"injected menu upgrade failure"); }
            {
                internal::SqliteConnection unchanged(directory/"SFX.DB",SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);
                check(unchanged.scalar_int64("PRAGMA user_version","version")==2 &&
                    internal::validate_sfx_shipping_database(unchanged.get(),true),"menu transaction rollback preserves exact predecessor");
                check(unchanged.scalar_int64("SELECT count(*) FROM sfx_accounts WHERE display_name='Existing'","caller")==1,
                    "failed migration preserves existing account");
            }
            migration_options.bootstrap_boundary_hook={}; db::SqliteStorage upgraded(migration_options,paths);
check(upgraded.start().succeeded() && upgraded.health().schema_version==sfx::test::shipping_schema_version,"retry upgrade succeeds");
            const auto preference=upgraded.menu_preferences(1);
            check(preference && preference.value->revision==1 && preference.value->preferences.color==0 &&
                !preference.value->preferences.hotkey,"legacy account backfill retains Off rather than new-account default");
            check(upgraded.shutdown().succeeded(),"upgraded fixture stopped");
        }
        std::cout<<"PASS menu preference storage: "<<checks<<" checks; 0 failed; 0 skipped\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n'; return 1; }
}
