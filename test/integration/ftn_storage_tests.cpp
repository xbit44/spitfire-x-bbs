// Real SQLite and the existing isolated fixture builders; no live BBS data.
#define main retained_ftn_file_fixture_runner
#include "file_storage_tests.cpp"
#undef main
#include "sfx/application/ftn_storage.hpp"
#include <set>

namespace {
app::FtnTransferToken token(unsigned n){app::FtnTransferToken t{};t[0]=0x46;t[12]=static_cast<std::uint8_t>(n>>24);t[13]=static_cast<std::uint8_t>(n>>16);t[14]=static_cast<std::uint8_t>(n>>8);t[15]=static_cast<std::uint8_t>(n);return t;}
app::FtnNetwork network(){app::FtnNetwork n;n.code="my-net";n.name="My Own Network";n.address="42:100/1@my-net";n.enabled=true;return n;}
void migration(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& paths){
    for(const char* point:{"before-ftn-migration","before-ftn-migration-commit","after-ftn-migration-commit"}){
        auto s=settings(root/point);auto prior=storage::sfx_storage_migrations();prior.resize(20);
        {storage::SqliteStorage old(s,paths,prior);check(old.start().succeeded()&&old.health().schema_version==20,"exact accepted v20 predecessor");check(old.shutdown().succeeded(),"old fixture stopped");}
        s.bootstrap_boundary_hook=[point](std::string_view at){return at==point?app::StorageStatus{app::StorageErrorCode::io,"injected FTN upgrade boundary"}:app::StorageStatus::success();};
        {storage::SqliteStorage failed(s,paths);check(!failed.start(),"migration boundary interruption reported");}
        const auto version=scalar(s.data_directory/"SFX.DB","PRAGMA user_version");
        check(version==(std::string_view(point)=="after-ftn-migration-commit"?21:20),"upgrade all committed or all rolled back");
        s.bootstrap_boundary_hook={};storage::SqliteStorage retry(s,paths);check(retry.start().succeeded()&&retry.health().schema_version==sfx::test::shipping_schema_version,"interrupted upgrade safely retries through current shipping schema");check(retry.shutdown().succeeded(),"retry stopped");
    }
}
}
int main(int argc,char** argv){try{
    check(argc==2,"fresh fixture directory argument required");const std::filesystem::path root=argv[1];check(!std::filesystem::exists(root),"preserve prior fixture evidence");
    sfx::test::FakeStoragePathPolicy paths;auto s=settings(root/"database");
    {storage::SqliteStorage fresh(s,paths);check(fresh.start().succeeded()&&fresh.health().schema_version==sfx::test::shipping_schema_version,"fresh current shipping schema");check(fresh.shutdown().succeeded(),"fresh stopped for synthetic owner fixture");}
    seed(s.data_directory/"SFX.DB");storage::SqliteStorage db(s,paths);check(db.start().succeeded(),"real SQLite writer ready");const app::MessageConfigurationActor owner{true,{}};
    auto query=db.ftn_configuration(owner);check(query&&query.value&&query.value->revision==1&&query.value->networks.empty(),"default empty typed FTN catalog");
    check(!db.ftn_configuration({false,app::MessageActor{id(2),2,1,1}}),"ordinary caller cannot inspect FTN credentials");
    auto c=*query.value;c.networks.push_back(network());
    auto saved=db.change_ftn_configuration(owner,token(1),c);check(saved&&saved.value->applied_revision==2,"valid no-hub configuration saved");
    const auto replay=db.change_ftn_configuration(owner,token(1),c);check(replay&&replay.value->replayed&&replay.value->applied_revision==2,"lost reply reconciles exact operation");
    auto altered=c;altered.networks[0].name="Other";check(!db.change_ftn_configuration(owner,token(1),altered),"changed-content token replay refused");
    c=*db.ftn_configuration(owner).value;
    const auto before_count=scalar(s.data_directory/"SFX.DB","SELECT count(*) FROM sfx_local_echoes");
    for(unsigned i=0;i<2000;++i){app::FtnMessageEcho echo;echo.network="my-net";echo.tag="ECHO."+std::to_string(i);echo.description=std::string(96,'d');echo.store_locally=i<2;c.message_echoes.push_back(echo);}
    check(app::encode_ftn_configuration(c).size()>65536,"real mapped catalog exceeds one IPC frame");
    saved=db.change_ftn_configuration(owner,token(2),c);if(!saved)std::cerr<<"save error "<<saved.status.operation<<" code "<<static_cast<unsigned>(saved.status.code)<<'\n';
    check(saved&&saved.value->configuration.message_echoes[0].local_echo>0&&saved.value->configuration.message_echoes[1].local_echo>0,"selected mapping and definition finalize together");
    check(scalar(s.data_directory/"SFX.DB","SELECT count(*) FROM sfx_local_echoes")==before_count+2,"only selected local conferences created");
    check(scalar(s.data_directory/"SFX.DB","SELECT count(*) FROM sfx_local_echoes WHERE access_minimum=10 AND reading_minimum=10 AND posting_minimum=10")>=2,"approved10/10/10 defaults stored");
    check(db.change_ftn_configuration(owner,token(2),c).value->replayed&&scalar(s.data_directory/"SFX.DB","SELECT count(*) FROM sfx_local_echoes")==before_count+2,"retry cannot duplicate conference mappings");
    check(!db.change_ftn_configuration(owner,token(3),c),"stale FTN revision refused");
    auto current=*db.ftn_configuration(owner).value;const auto revision=current.revision;
    auto failed=current;failed.message_echoes.push_back({"my-net","NEW.ONE","First"});failed.message_echoes.push_back({"my-net","ECHO.0","Name collision"});
    check(!db.change_ftn_configuration(owner,token(4),failed),"invalid duplicate selection refused");
    check(db.ftn_configuration(owner).value->revision==revision&&scalar(s.data_directory/"SFX.DB","SELECT count(*) FROM sfx_local_echoes")==before_count+2,"failed finalization leaves no orphan or revision");
    failed=current;failed.message_echoes.push_back({"my-net","NEW.ONE","First"});failed.message_echoes.push_back({"my-net","NEW.TWO","Second"});failed.message_echoes.back().local_name="ECHO.0";
    check(!db.change_ftn_configuration(owner,token(5),failed),"SQL name collision after first insert rolls back");
    check(db.ftn_configuration(owner).value->revision==revision&&scalar(s.data_directory/"SFX.DB","SELECT count(*) FROM sfx_local_echoes")==before_count+2,"real writer rollback preserves both catalogs");
    failed=current;--failed.message_catalog_revision;check(!db.change_ftn_configuration(owner,token(6),failed),"affected message catalog revision rechecked");
    failed=current;--failed.file_catalog_revision;check(!db.change_ftn_configuration(owner,token(7),failed),"affected file catalog revision rechecked");
    failed=current;failed.file_echoes.push_back({"my-net","FILES","Invalid target",999999});check(!db.change_ftn_configuration(owner,token(8),failed),"file target existence rechecked");
    check(db.shutdown().succeeded(),"writer stopped for actual reopen");
    {storage::SqliteStorage reopened(s,paths);check(reopened.start().succeeded(),"real reopen");const auto after=reopened.ftn_configuration(owner);check(after&&after.value->revision==revision&&after.value->message_echoes.size()==2000,"latest mapping/catalog survives reopen");check(reopened.shutdown().succeeded(),"reopened writer stopped");}
    auto limited=s;limited.queue.maximum_bytes=1024; // Real admission pressure, no global production budget change.
    {storage::SqliteStorage pressure(limited,paths);check(pressure.start().succeeded(),"small-budget writer fixture starts");const auto before=pressure.ftn_configuration(owner);auto value=*before.value;value.networks[0].name="Not admitted";const auto result=pressure.change_ftn_configuration(owner,token(9),value);check(!result&&result.status.code==app::StorageErrorCode::backpressure,"FTN accounting reaches real writer admission");check(pressure.ftn_configuration(owner).value->revision==revision,"backpressure preserves saved revision");check(pressure.shutdown().succeeded(),"pressure fixture stopped");}
    migration(root/"migration",paths);
    {internal::SqliteConnection read(s.data_directory/"SFX.DB",SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);check(internal::validate_sfx_shipping_database(read.get(),true),"current exact manifest/integrity/foreign keys");}
    std::cout<<"PASS FTN SQLite "<<checks<<" checks; real transactions, synthetic owner fixture\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL FTN SQLite after "<<checks<<": "<<e.what()<<'\n';return 1;}}
