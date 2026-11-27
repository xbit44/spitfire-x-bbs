// Real managed SQLite migration; exclusively new synthetic disposable fixtures.
#define main retained_file_storage_fixture_main
#include "file_storage_tests.cpp"
#undef main
#include <map>
namespace {
std::map<std::string,std::string> contents(const std::filesystem::path& path){
    internal::SqliteConnection db(path,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);std::map<std::string,std::string> result;
    internal::SqliteStatement tables(db.get(),"SELECT name FROM sqlite_schema WHERE type='table' AND (name NOT GLOB 'sqlite_*' OR name='sqlite_sequence') ORDER BY name","capture synthetic durable tables");
    while(tables.row()){
        const auto name=tables.text(0);if(name.rfind("sfx_ftn_",0)==0&&name!="sfx_ftn_configuration"&&name!="sfx_ftn_configuration_receipts")continue;
        std::string query="SELECT * FROM \""+name+"\""+(name=="sfx_schema_migrations"?" WHERE version<=21":"");sqlite3_stmt* columns{};
        check(sqlite3_prepare_v2(db.get(),query.c_str(),-1,&columns,nullptr)==SQLITE_OK,"original table projection prepares");const auto count=sqlite3_column_count(columns);sqlite3_finalize(columns);query+=" ORDER BY ";for(int column=1;column<=count;++column){if(column>1)query+=',';query+=std::to_string(column);}sqlite3_stmt* raw{};
        if(sqlite3_prepare_v2(db.get(),query.c_str(),-1,&raw,nullptr)!=SQLITE_OK)throw std::runtime_error("original table capture "+name+": "+sqlite3_errmsg(db.get()));
        ++checks;std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> statement(raw,sqlite3_finalize);auto& data=result[name];int rc{};
        while((rc=sqlite3_step(raw))==SQLITE_ROW)for(int i=0;i<sqlite3_column_count(raw);++i){const auto type=sqlite3_column_type(raw,i);data+=std::to_string(type)+":";const auto* bytes=static_cast<const char*>(sqlite3_column_blob(raw,i));const auto size=sqlite3_column_bytes(raw,i);data+=std::to_string(size)+":";if(bytes)data.append(bytes,static_cast<std::size_t>(size));data+=';';}
        check(rc==SQLITE_DONE,"complete exact original table capture");
    }return result;
}
app::CommitSourceMessageRequest seeded_message(storage::SqliteStorage& db,unsigned op,app::MessageKind kind){
    app::MessageActor actor{id(2),2,1,1};app::MessageTarget target{kind,kind==app::MessageKind::public_echo?app::LocalEchoId{2}:app::LocalEchoId{},3,{}};
    auto context=db.begin_message(actor,target);check(context&&context.value,"native caller posting authority before upgrade");
    app::CommitSourceMessageRequest request{actor,id(op),*context.value,"Preserved subject",{app::MessageSourceFormat::ansi_canvas,1,"Original \x1b[31mart\x1b[0m\r\nSecond line"}};
    check(db.commit_source_message(request).succeeded(),"legitimate native source/receipt/recipient/arrival committed before upgrade");return request;
}
void upgrade(const std::filesystem::path& root,const char* fault){
    sfx::test::FakeStoragePathPolicy paths;auto s=settings(root);auto plan=storage::sfx_storage_migrations();check(plan.size()==sfx::test::shipping_migration_count,"current shipping lineage retains the exact21-to22 migration slice");auto target=plan;target.resize(22);plan.resize(21);
    {storage::SqliteStorage old(s,paths,plan);const auto started=old.start();if(!started)std::cerr<<started.operation<<'\n';check(started.succeeded()&&old.health().schema_version==21,"exact21 startup");check(old.shutdown().succeeded(),"old fixture stopped");}
    const auto file=s.data_directory/"SFX.DB";seed(file);
    std::vector<app::CommitSourceMessageRequest> messages;
    {storage::SqliteStorage old(s,paths,plan);check(old.start().succeeded(),"populated21 ready");messages.push_back(seeded_message(old,101,app::MessageKind::public_echo));messages.push_back(seeded_message(old,102,app::MessageKind::private_local));check(old.shutdown().succeeded(),"native old content stopped");}
    {storage::SqliteStorage old(s,paths,plan);check(old.start().succeeded(),"predecessor file fixtures open");const app::FileConfigurationActor owner{true,{}};
        app::FileRegistration r;r.operation=id(501);r.area={1};r.expected_area_revision=1;r.filename="migration.zip";r.descriptions={"Preserved file","Original metadata"};r.public_uploader="Sysop";r.payload={123,{},"synthetic-volume","synthetic-file"};r.payload.sha256[0]=1;r.original_upload_utc=1700000000;const auto added=old.register_file(owner,r);check(bool(added),"actual predecessor file publication and import receipt");
        app::FileOperationRequest operation;operation.operation=id(502);operation.file=added.value->id;operation.expected_revision=added.value->revision;operation.action=app::FileOperationAction::metadata;operation.descriptions={"Updated metadata","Retained committed operation"};operation.public_uploader="Sysop";check(bool(old.prepare_file_operation(owner,operation)),"predecessor metadata intent");app::FileOperationCompletion completed;completed.source=completed.destination=r.payload;completed.destination_present=true;check(bool(old.commit_file_operation(owner,operation.operation,completed)),"predecessor completed native-operation receipt with synthetic object identity");
        const auto current=old.managed_file(owner,added.value->id);check(bool(current),"read predecessor updated file");operation={};operation.operation=id(503);operation.file=added.value->id;operation.expected_revision=current.value->file.revision;operation.action=app::FileOperationAction::copy;operation.destination={1};operation.expected_destination_revision=1;operation.filename="pending-copy.zip";operation.allow_duplicate_content=true;check(bool(old.prepare_file_operation(owner,operation)),"predecessor pending copy intent retained without native placement");check(old.shutdown().succeeded(),"predecessor file fixtures closed");}
    {internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);c.exec("UPDATE sqlite_sequence SET seq=4000 WHERE name='sfx_local_messages';UPDATE sqlite_sequence SET seq=5000 WHERE name='sfx_file_catalog'","preserve historical purged message and file ID high-water");}
    const auto before=contents(file);
    if(fault)s.bootstrap_boundary_hook=[fault](std::string_view point){return point==fault?app::StorageStatus{app::StorageErrorCode::io,"injected FTN22 migration boundary"}:app::StorageStatus::success();};
    {storage::SqliteStorage current(s,paths,target);const auto started=current.start();if(!started&& !fault)std::cerr<<"startup: "<<started.operation<<" code="<<static_cast<unsigned>(started.code)<<'\n';check(fault?!started:started.succeeded(),"upgrade/fault result truthful");if(started)check(current.shutdown().succeeded(),"new schema stopped");}
    const bool committed=!fault||std::string_view(fault)=="after-ftn-mail-migration-commit";
    check(scalar(file,"PRAGMA user_version")== (committed?22:21),"fixed final migration commit/rollback boundary");
    check(contents(file)==before,"every original table value, native source, receipt, counter, state, ledger and sequence preserved");
    {internal::SqliteConnection c(file,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);check(internal::validate_sfx_shipping_database(c.get(),true),"full exact schema/ledger/foreign-key verification");check(c.scalar_int64("PRAGMA foreign_keys","reopened FK authority")==1,"reopened connection enforces foreign keys");}
    s.bootstrap_boundary_hook={};storage::SqliteStorage retry(s,paths,target);const auto start=retry.start();if(!start)std::cerr<<"retry: "<<start.operation<<'\n';check(start.succeeded()&&retry.health().schema_version==22,"retry/reopen reaches exact22 once");
    for(const auto& request:messages)check(retry.commit_source_message(request).succeeded(),"old local source-commit fingerprint replays without rewriting history");
    check(retry.shutdown().succeeded(),"new fixture closed");check(contents(file)==before,"replay cannot duplicate message/receipt/arrival state");
    {internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
        check(sqlite3_exec(c.get(),"INSERT INTO sfx_local_messages(kind,echo_id,area_number,author_account_id,subject,body,encoding,body_lines,arrival_ordinal,created_utc_seconds) VALUES(0,1,9,NULL,x'61',x'61','cp437',1,9,1700000000)",nullptr,nullptr,nullptr)!=SQLITE_OK,"remote NULL author cannot bypass typed import scope");
        check(sqlite3_exec(c.get(),"INSERT INTO sfx_local_messages(kind,author_account_id,subject,body,encoding,body_lines,arrival_ordinal,created_utc_seconds) VALUES(2,2,x'61',x'61','cp437',1,9,1700000000)",nullptr,nullptr,nullptr)!=SQLITE_OK,"kind2 cannot bypass immutable typed target");
        check(c.scalar_int64("SELECT count(*) FROM pragma_foreign_key_check","all original references survive")==0,"post-negative foreign-key check");}
}
}
int main(int argc,char** argv){try{check(argc==2,"new fixture root required");const std::filesystem::path root=argv[1];check(!std::filesystem::exists(root),"preserve previous qualification fixtures");
    upgrade(root/"normal",nullptr);for(const auto fault:{"before-ftn-mail-migration","after-ftn-message-rebuild","before-ftn-mail-migration-commit","after-ftn-mail-migration-commit"})upgrade(root/fault,fault);
    std::cout<<"PASS "<<checks<<" actual populated21-to22 FTN migration checks\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
