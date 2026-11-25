#include "sfx/application/menu_session.hpp"
#define main retained_qwk_storage_main
#include "qwk_storage_tests.cpp"
#undef main
#include "qwk_candidate_boundaries.inc"
#include "qwk_candidate_authority.inc"
int main(int argc,char** argv)try{
    check(argc==2,"new isolated candidate fixture required");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"preserve previous fixture");std::filesystem::create_directories(root);
    storage::SqliteStorageSettings settings;settings.data_directory=root;settings.utc_now=[]{return std::int64_t{1794484800};};sfx::test::FakeStoragePathPolicy paths;storage::SqliteStorage db(settings,paths);
    check(db.start().succeeded()&&db.shutdown().succeeded(),"create current schema");const auto file=root/"SFX.DB";seed(file);check(db.start().succeeded(),"trusted synthetic actors ready");
    const app::MessageActor author{id(2),2,1,1},reader{id(3),3,1,1};
    const auto post=[&](app::MessageTarget target,unsigned operation){const auto context=db.begin_message(author,target);check(bool(context),"ordinary compose authority");const auto committed=db.commit_message({author,id(operation),*context.value,"Candidate fixture","Original body\n"});check(bool(committed),"ordinary message commit");return committed.value->id;};
    const auto public_id=post({app::MessageKind::public_echo,{2},{},{}},100),private_id=post({app::MessageKind::private_local,{},3,{}},101);
    check(bool(db.qwk_preferences(reader)),"saved defaults available");const auto plan=db.begin_qwk_packet(reader,id(102));check(bool(plan),"finite authenticated plan");
    std::size_t public_queue{};for(std::size_t i=0;i<plan.value->queues.size();++i)if(plan.value->queues[i].echo.value==2)public_queue=i;check(public_queue!=0,"actual public queue found");
    runtime_authority_checks(db,reader,*plan.value,file);
    const auto candidate=db.next_qwk_candidate(reader,*plan.value,public_queue);const auto opened=db.next_qwk_message(reader,*plan.value,public_queue);
    check(candidate&&candidate.value->has_value()&&opened&&opened.value->has_value()&&candidate.value->value().message==public_id&&candidate.value->value().arrival==opened.value->value().header.identity.arrival_ordinal,"real SQLite metadata and fresh full read select same identity/arrival");
    check(!db.next_qwk_candidate(author,*plan.value,public_queue)&&!db.next_qwk_candidate(reader,*plan.value,plan.value->queues.size()),"cross-owner and invalid queue remain refused");
    // Deliberate corruption of ONLY this new fixture. Restore the exact trigger
    // definition in the same transaction; never alter production/schema rules.
    const auto body=[&](std::string bytes){internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);internal::SqliteStatement read(c.get(),"SELECT sql FROM sqlite_master WHERE type='trigger' AND name='sfx_local_messages_immutable_update'","owned corruption fixture trigger");check(read.row(),"exact immutable guard captured");const auto sql=read.text(0);c.exec("BEGIN IMMEDIATE;DROP TRIGGER sfx_local_messages_immutable_update","new fixture fault transaction");internal::SqliteStatement update(c.get(),"UPDATE sfx_local_messages SET body=?1 WHERE message_id=?2","new fixture body fault only");update.bind(1,std::vector<std::uint8_t>(bytes.begin(),bytes.end()));update.bind(2,public_id.value);update.step();c.exec(sql.c_str(),"restore exact immutable trigger");c.exec("COMMIT","finish new fixture fault");};
    body("Two\nrows\n");
    const auto metadata=db.next_qwk_candidate(reader,*plan.value,public_queue);check(metadata&&metadata.value->has_value()&&metadata.value->value().message==public_id,"real metadata path avoids decoding a deliberately malformed body");
    check(!db.next_qwk_message(reader,*plan.value,public_queue),"fresh full read still rejects malformed body");
    app::QwkPacketMetadata meta;meta.board_name="Candidate test";meta.sysop="Synthetic";meta.caller_name="Bob";meta.created="11-12-2026,12:00:00";
    const auto refused=sfx::archive::prepare_qwk_download(db,reader,id(103),meta,[](std::int64_t){return sfx::archive::QwkRecordDate{"11-12-26","12:00","1794484800"};});
    check(!refused.result.ok()&&!refused.progress&&refused.zip.empty()&&scalar(file,"SELECT count(*) FROM sfx_qwk_continuation")==0&&scalar(file,"SELECT count(*) FROM sfx_caller_echo_state")==0,"body cannot be skipped/exported and failure changes no delivery/read pointers");
    body("Original body\n");check(bool(db.open_message(reader,public_id)),"online public read advances normally");const auto advanced=db.next_qwk_candidate(reader,*plan.value,public_queue);check(advanced&&!advanced.value->has_value(),"candidate honors forward online public reading under existing finite plan");
    const auto mail=db.next_qwk_candidate(reader,*plan.value,0);check(mail&&mail.value->has_value()&&mail.value->value().message==private_id,"private candidate remains caller-owned");check(bool(db.open_message(reader,private_id)),"online private read");const auto read_mail=db.next_qwk_candidate(reader,*plan.value,0);check(read_mail&&!read_mail.value->has_value(),"private candidate honors unread state changes");
    bounded_queue_checks(db,reader,author,file);
    {internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);c.exec("UPDATE sfx_accounts SET revision=revision+1 WHERE account_id=3","revoke only synthetic actor revision");}
    check(!db.next_qwk_candidate(reader,*plan.value,public_queue)&&!db.next_qwk_message(reader,*plan.value,public_queue),"current authority revision checked by both paths");check(db.shutdown().succeeded(),"fixture shutdown");
    std::cout<<"PASS QWK real candidate/deferred-body/authority/read-progress "<<checks<<" checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL QWK candidate after "<<checks<<": "<<e.what()<<'\n';return 1;}
