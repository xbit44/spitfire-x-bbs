// New isolated databases only. Exercise real migration, rollback and editor commits.
#define main retained_message_storage_main
#include "message_storage_tests.cpp"
#undef main
#include <map>
std::map<std::string,std::string> subject_snapshot(const std::filesystem::path& file){
    internal::SqliteConnection c(file,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);std::map<std::string,std::string> result;
    internal::SqliteStatement names(c.get(),"SELECT name FROM sqlite_schema WHERE type='table' ORDER BY name","all old table contents");
    while(names.row()){
        const auto name=names.text(0);std::string sql="SELECT * FROM \""+name+"\""+(name=="sfx_schema_migrations"?" WHERE version<=22":"");sqlite3_stmt* raw{};
        check(sqlite3_prepare_v2(c.get(),sql.c_str(),-1,&raw,nullptr)==SQLITE_OK,"old table projection");const auto count=sqlite3_column_count(raw);sqlite3_finalize(raw);sql+=" ORDER BY ";for(int i=1;i<=count;++i){if(i>1)sql+=',';sql+=std::to_string(i);}
        check(sqlite3_prepare_v2(c.get(),sql.c_str(),-1,&raw,nullptr)==SQLITE_OK,"ordered table projection");std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> q(raw,sqlite3_finalize);auto& data=result[name];int rc{};
        while((rc=sqlite3_step(raw))==SQLITE_ROW)for(int i=0;i<count;++i){const auto size=sqlite3_column_bytes(raw,i);data+=std::to_string(sqlite3_column_type(raw,i))+":"+std::to_string(size)+":";const auto* bytes=static_cast<const char*>(sqlite3_column_blob(raw,i));if(bytes)data.append(bytes,size);data+=';';}
        check(rc==SQLITE_DONE,"table projection complete");
    }return result;
}
int main(int argc,char** argv){try{
    check(argc==2,"fresh isolated path required");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"preserve previous fixtures");sfx::test::FakeStoragePathPolicy policy;
    for(const auto* fault:{"none","before-optional-subject-migration","after-optional-subject-rebuild","before-optional-subject-migration-commit","after-optional-subject-migration-commit"}){
        auto s=options(root/fault);auto prior=db::sfx_storage_migrations();check(prior.size()==27,"current lineage27 retains fixed23 and additive24");auto subject_only=prior;subject_only.resize(23);prior.resize(22);const auto file=s.data_directory/"SFX.DB";
        {db::SqliteStorage old(s,policy,prior);check(old.start().succeeded()&&old.shutdown().succeeded(),"real22 predecessor");seed(file);check(old.start().succeeded(),"populated22 starts");for(unsigned n=30;n<34;++n){auto post=request(old,{id(2),2,1,1},{app::MessageKind::public_echo,{2},{},{}},n);check(old.commit_message(post).succeeded(),"old messages committed");}check(old.shutdown().succeeded(),"predecessor stopped");}
        edit(file,"UPDATE sqlite_sequence SET seq=5000 WHERE name='sfx_local_messages'");const auto before=subject_snapshot(file);
        const bool inject=std::string_view(fault)!="none";if(inject)s.bootstrap_boundary_hook=[fault](std::string_view at){return at==fault?app::StorageStatus{app::StorageErrorCode::io,"synthetic23 boundary failure"}:app::StorageStatus::success();};
        {db::SqliteStorage next(s,policy,subject_only);const auto started=next.start();if(!inject&&!started)std::cerr<<started.operation<<'\n';check(inject?!started:started.succeeded(),"truthful migration/fault outcome");if(started)check(next.shutdown().succeeded(),"new schema stopped");}
        const bool committed=!inject||std::string_view(fault)=="after-optional-subject-migration-commit";check(scalar(file,"PRAGMA user_version")== (committed?23:22),"atomic23 commit or rollback");check(subject_snapshot(file)==before,"all old table values, receipts, counters, sequence and ledger preserved");
        s.bootstrap_boundary_hook={};db::SqliteStorage store(s,policy,subject_only);check(store.start().succeeded()&&store.health().schema_version==23,"retry reaches23 once");check(subject_snapshot(file)==before,"retry preserves every original value");
        unsigned op=100;
        for(bool ansi:{false,true})for(bool escape:{false,true})for(bool private_mail:{false,true})for(bool populated:{false,true}){
            sfx::test::DeterministicInteractiveByteStreamFake stream;stream.script_non_secret_text(std::string(populated?"Subject":"")+"\r\nBody retained"+(ansi&&escape?"":"\r\n"));
            if(escape){if(ansi){stream.script_non_secret_text("\x1b");stream.script_read_outcome(app::InteractiveStreamOutcome::timed_out);}else stream.script_non_secret_text("/?\r\n");stream.script_non_secret_text("S\r\nY\r\n");}else stream.script_non_secret_text("/s\r\nY\r\n");
            app::StreamCallerTerminal terminal(stream);std::string output;
            app::MessageComposePorts ports{store,terminal,[]{return std::optional<app::MessageActor>{{id(2),2,1,1}};},[&]{return id(op++);},[ansi]{return app::MessageEditorDimensions{ansi,80,24};},[&]{return terminal.read_editor_byte();},[&](std::string_view text){output+=text;return true;}};
            ports.accepted_key=[&](bool cr){terminal.finish_editor_key(cr);};
            const app::MessageTarget target=private_mail?app::MessageTarget{app::MessageKind::private_local,{},3,{}}:app::MessageTarget{app::MessageKind::public_echo,{2},{},{}};
            const auto outcome=app::compose_local_message(ports,target,private_mail?"Bob":"All Callers","Alice");if(outcome!=app::MessageComposeOutcome::saved)std::cerr<<"ansi="<<ansi<<" escape="<<escape<<" subject="<<populated<<"\n"<<output<<'\n';check(outcome==app::MessageComposeOutcome::saved,"slash Save and FSE Escape/plain Help Save accept optional subjects for public/private messages");
            const auto saved=store.quote_source({id(2),2,1,1},{scalar(file,"SELECT max(message_id) FROM sfx_local_messages")});check(saved&&saved.value->header.subject==(populated?"Subject":"")&&saved.value->body.canonical_lf=="Body retained\n","subject remains truly empty and body exact");
        }
        auto bad=request(store,{id(2),2,1,1},{app::MessageKind::public_echo,{2},{},{}},500);bad.subject="";bad.body=" \n";check(!store.commit_message(bad),"blank body still refused");bad.body="Body\n";bad.subject="\x1b";check(!store.commit_message(bad),"subject control injection still refused");
        check(store.shutdown().succeeded(),"fixture closed");{internal::SqliteConnection c(file,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);check(internal::validate_sfx_shipping_database(c.get(),true),"exact schema integrity and foreign keys after commits");}
    }
    std::cout<<"PASS "<<checks<<" optional subject migration and real editor/store checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
