// NG34: new synthetic production storage/archive/codec/sealed-source fixtures.
// No historical owner packet or live hub is used or claimed reproduced.
#define main retained_qwk_storage_main
#include "qwk_storage_tests.cpp"
#undef main
#include "alpha_qwk_reply_spool.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/application/ftn_work.hpp"
#include <fstream>
#include "qwk_packet_notices.hpp"
#include "qwk_download_preparation.hpp"
#include <future>
#include <psapi.h>
#include <sstream>
namespace ins=sfx::platform::win32::installation;
namespace arc=sfx::archive;
namespace {
struct Measured:app::IQwkStorage {
    storage::SqliteStorage& db;std::uint64_t next_calls{},reference_calls{},next_us{},reference_us{},batch_calls{},batch_us{};
    std::function<void(const std::vector<app::MessageId>&)> after_batch;
    explicit Measured(storage::SqliteStorage& value):db(value){}
    void reset(){next_calls=reference_calls=next_us=reference_us=batch_calls=batch_us=0;}
    app::StorageResult<app::QwkPolicy> qwk_policy(const app::MessageConfigurationActor& a)override{return db.qwk_policy(a);}
    app::StorageResult<app::QwkPolicy> save_qwk_policy(const app::MessageConfigurationActor& a,app::Identifier128 op,std::int64_t rev,std::string_view board,std::uint32_t count)override{return db.save_qwk_policy(a,op,rev,board,count);}
    app::StorageResult<app::QwkSavedPreferences> qwk_preferences(const app::MessageActor& a)override{return db.qwk_preferences(a);}
    app::StorageResult<app::QwkSavedPreferences> save_qwk_preferences(const app::MessageActor& a,std::int64_t rev,const app::QwkPreferences& p)override{return db.save_qwk_preferences(a,rev,p);}
    app::StorageResult<app::QwkPacketPlan> begin_qwk_packet(const app::MessageActor& a,app::Identifier128 op)override{return db.begin_qwk_packet(a,op);}
    app::StorageResult<bool> validate_qwk_packet_authority(const app::MessageActor& a,const app::QwkPacketPlan& p)override{return db.validate_qwk_packet_authority(a,p);}
    app::StorageResult<std::optional<app::OpenedMessage>> next_qwk_message(const app::MessageActor& a,const app::QwkPacketPlan& p,std::size_t n)override{const auto start=std::chrono::steady_clock::now();auto r=db.next_qwk_message(a,p,n);++next_calls;next_us+=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();return r;}
    app::StorageResult<std::optional<app::QwkMessageCandidate>> next_qwk_candidate(const app::MessageActor& a,const app::QwkPacketPlan& p,std::size_t n)override{const auto start=std::chrono::steady_clock::now();auto r=db.next_qwk_candidate(a,p,n);++next_calls;next_us+=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();return r;}
    app::StorageResult<app::QwkQueueSelection<app::QwkMessageCandidate>> select_qwk_candidate(const app::MessageActor& a,const app::QwkPacketPlan& p,std::size_t n,std::size_t maximum)override{const auto start=std::chrono::steady_clock::now();auto r=db.select_qwk_candidate(a,p,n,maximum);++next_calls;next_us+=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();return r;}
    app::StorageResult<app::QwkQueueSelection<app::OpenedMessage>> select_qwk_message(const app::MessageActor& a,const app::QwkPacketPlan& p,std::size_t n,std::size_t maximum)override{const auto start=std::chrono::steady_clock::now();auto r=db.select_qwk_message(a,p,n,maximum);++next_calls;next_us+=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();return r;}
    app::StorageResult<std::uint32_t> qwk_reference(const app::MessageActor& a,app::MessageId id)override{const auto start=std::chrono::steady_clock::now();auto r=db.qwk_reference(a,id);++reference_calls;reference_us+=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();return r;}
    app::StorageResult<std::vector<std::uint32_t>> qwk_references(const app::MessageActor& a,const std::vector<app::MessageId>& ids)override{const auto start=std::chrono::steady_clock::now();auto r=db.qwk_references(a,ids);++batch_calls;batch_us+=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();if(r&&after_batch)after_batch(ids);return r;}
    app::StorageResult<app::QwkDeliveryOutcome> commit_qwk_delivery(const app::MessageActor& a,const app::QwkDeliveryCommit& c)override{return db.commit_qwk_delivery(a,c);}
    app::StorageResult<std::optional<app::QwkDeliveryOutcome>> qwk_delivery_outcome(const app::MessageActor& a,const app::QwkDeliveryCommit& c)override{return db.qwk_delivery_outcome(a,c);}
    app::StorageResult<app::QwkPointerChange> preview_qwk_public_pointers(const app::MessageActor& a,const app::QwkPointerChange& c)override{return db.preview_qwk_public_pointers(a,c);}
    app::StorageStatus change_qwk_public_pointers(const app::MessageActor& a,const app::QwkPointerChange& c)override{return db.change_qwk_public_pointers(a,c);}
};
void fallback_selection_checks(storage::SqliteStorage& db,const app::MessageActor& actor){
    struct Fallback final:Measured {
        std::vector<std::size_t> seen;std::optional<std::size_t> eligible;
        using Measured::Measured;
        app::StorageResult<std::optional<app::OpenedMessage>> next_qwk_message(const app::MessageActor&,const app::QwkPacketPlan&,std::size_t n)override{seen.push_back(n);if(!eligible||n!=*eligible)return {app::StorageStatus::success(),std::optional<app::OpenedMessage>{}};app::OpenedMessage m;m.header.identity.id={static_cast<std::int64_t>(n+1)};m.header.identity.arrival_ordinal=n+1;return {app::StorageStatus::success(),std::optional<app::OpenedMessage>{std::move(m)}};}
        app::StorageResult<std::optional<app::QwkMessageCandidate>> next_qwk_candidate(const app::MessageActor& a,const app::QwkPacketPlan& p,std::size_t n)override{return app::IQwkStorage::next_qwk_candidate(a,p,n);}
        app::StorageResult<app::QwkQueueSelection<app::QwkMessageCandidate>> select_qwk_candidate(const app::MessageActor& a,const app::QwkPacketPlan& p,std::size_t n,std::size_t maximum)override{return app::IQwkStorage::select_qwk_candidate(a,p,n,maximum);}
        app::StorageResult<app::QwkQueueSelection<app::OpenedMessage>> select_qwk_message(const app::MessageActor& a,const app::QwkPacketPlan& p,std::size_t n,std::size_t maximum)override{return app::IQwkStorage::select_qwk_message(a,p,n,maximum);}
    } fallback(db);
    for(const std::size_t count:{1U,8U,9U})for(bool populated:{false,true})for(std::size_t start=0;start<count;++start)for(bool full:{false,true}){
        app::QwkPacketPlan p;p.queues.resize(count);fallback.seen.clear();fallback.eligible=populated?std::optional<std::size_t>{count-1}:std::optional<std::size_t>{};std::size_t at=start,done{};std::optional<app::MessageId> selected;
        while(done<count){const auto limit=std::min(app::kQwkSelectionQueuesMaximum,count-done);if(full){const auto result=fallback.select_qwk_message(actor,p,at,limit);check(bool(result),"default full selection succeeds");at=result.value->queue;done+=result.value->scanned;if(result.value->selected)selected=result.value->selected->header.identity.id;}else{const auto result=fallback.select_qwk_candidate(actor,p,at,limit);check(bool(result),"default metadata selection succeeds");at=result.value->queue;done+=result.value->scanned;if(result.value->selected)selected=result.value->selected->message;}if(selected||done==count)break;at=(at+1)%count;}
        const auto expected=populated?(count-1+count-start)%count+1:count;check(done==expected&&fallback.seen.size()==expected&&bool(selected)==populated,"default port stops at first fair match or exactly one traversal");for(std::size_t n=0;n<expected;++n)check(fallback.seen[n]==(start+n)%count,"default port preserves exact queue visitation order");
    }
}
void byte_boundary(storage::SqliteStorage& db,const app::MessageActor& actor){
    // Production builder/codec/ZIP with a deterministic bounded storage port;
    // access/receipt behavior is qualified separately against actual SQLite.
    struct Large final:Measured{
        app::QwkPacketPlan plan;std::string body;std::uint32_t highest_allocated{};
        Large(storage::SqliteStorage& db,const app::MessageActor& actor):Measured(db){plan=*db.begin_qwk_packet(actor,id(40000)).value;plan.queues={{{2},1,0,0,true,true}};plan.catalog={{{2},2,"Synthetic","Byte boundary"}};plan.next_queue=0;plan.policy.maximum_messages=10000;plan.netmail_conference.reset();for(unsigned n=0;n<400;++n)body+=std::string(79,'x')+'\n';}
        app::StorageResult<app::QwkPacketPlan> begin_qwk_packet(const app::MessageActor&,app::Identifier128 operation)override{auto result=plan;result.operation=operation;return {app::StorageStatus::success(),result};}
        app::StorageResult<std::optional<app::QwkMessageCandidate>> next_qwk_candidate(const app::MessageActor& a,const app::QwkPacketPlan& p,std::size_t n)override{return app::IQwkStorage::next_qwk_candidate(a,p,n);}
        app::StorageResult<app::QwkQueueSelection<app::QwkMessageCandidate>> select_qwk_candidate(const app::MessageActor& a,const app::QwkPacketPlan& p,std::size_t n,std::size_t maximum)override{return app::IQwkStorage::select_qwk_candidate(a,p,n,maximum);}
        app::StorageResult<app::QwkQueueSelection<app::OpenedMessage>> select_qwk_message(const app::MessageActor& a,const app::QwkPacketPlan& p,std::size_t n,std::size_t maximum)override{return app::IQwkStorage::select_qwk_message(a,p,n,maximum);}
        app::StorageResult<std::optional<app::OpenedMessage>> next_qwk_message(const app::MessageActor&,const app::QwkPacketPlan& p,std::size_t queue)override{
            check(queue==0,"one deterministic boundary queue");const auto n=p.queues[0].after_arrival+1;if(n>2200)return {app::StorageStatus::success(),std::optional<app::OpenedMessage>{}};
            app::OpenedMessage m;m.header.identity.id={n};m.header.identity.arrival_ordinal=n;m.header.identity.area_number=n;m.header.identity.utc_seconds=1793970000;m.header.target.kind=app::MessageKind::public_echo;m.header.target.echo={2};m.header.from="Synthetic Author";m.header.to="All Callers";m.header.subject="Complete byte-boundary record";m.body.canonical_lf=body;return {app::StorageStatus::success(),std::optional<app::OpenedMessage>{std::move(m)}};
        }
        app::StorageResult<std::vector<std::uint32_t>> qwk_references(const app::MessageActor&,const std::vector<app::MessageId>& ids)override{check(!ids.empty()&&ids.size()<=32,"byte-boundary reference batch remains capped32");std::vector<std::uint32_t> out;for(const auto i:ids){out.push_back(static_cast<std::uint32_t>(i.value));highest_allocated=std::max(highest_allocated,out.back());}return {app::StorageStatus::success(),out};}
    } source(db,actor);
    app::QwkPacketMetadata meta;meta.board_name="NG34 Byte Boundary";meta.sysop="Synthetic";meta.caller_name="Bob";meta.created="11-06-2026,09:00:00";
    const auto packet=arc::prepare_qwk_download(source,actor,id(40001),meta,[](std::int64_t){return arc::QwkRecordDate{"11-06-26","09:00","1793970000"};});
    check(packet.result.ok()&&packet.progress&&packet.progress->more&&packet.progress->expanded_bytes<=app::kQwkExpandedBytes,"complete fair prefix fits actual64MiB expanded boundary and reports remainder");
    const auto& p=*packet.progress;check(p.delivered.size()>2000&&p.delivered.size()<2200&&p.final_queues[0].after_arrival==static_cast<std::int64_t>(p.delivered.size())&&source.highest_allocated>p.delivered.size(),"byte limit excludes unencoded prefetched cursor/ref from delivery progress");
    for(std::size_t n=0;n<p.delivered.size();++n)check(p.delivered[n].message.value==static_cast<std::int64_t>(n+1),"byte-boundary packet is contiguous with no skipped message");
    const auto members=download_members(packet.zip);check(members.at("MESSAGES.DAT").size()==128+p.delivered.size()*32128,"actual ZIP contains every complete record and no truncated final body");
    std::cout<<"BYTE_BOUNDARY delivered="<<p.delivered.size()<<" expanded="<<p.expanded_bytes<<" highest_prefetched="<<source.highest_allocated<<'\n';
}
std::string wire(std::uint16_t conference,bool privacy,std::string_view to,std::string_view subject,std::string body,std::uint32_t parent=0){
    std::replace(body.begin(),body.end(),'\n',static_cast<char>(0xe3));
    const auto blocks=1+(body.size()+127)/128;std::string out(blocks*128,' ');
    const auto field=[&](unsigned at,unsigned width,std::string_view value){out.replace(at,std::min<std::size_t>(width,value.size()),value.substr(0,width));};
    out[0]=privacy?'*':' ';field(1,7,std::to_string(conference));field(8,8,"11-06-26");field(16,5,"09:00");field(21,25,to);field(46,25,"Bob");field(71,25,subject);if(parent)field(108,8,std::to_string(parent));field(116,6,std::to_string(blocks));out[122]=static_cast<char>(0xe1);out[123]=static_cast<char>(conference);out[124]=static_cast<char>(conference>>8);out.replace(128,body.size(),body);return out;
}
std::string reply_zip(const std::vector<arc::QwkArchiveFile>& files){
    // Independent synthetic reader-side ZIP producer. The production writer
    // intentionally emits downloads only; do not broaden its output allowlist.
    std::size_t size=4096;for(const auto& file:files)size+=file.bytes.size()*2+4096;std::vector<char> bytes(size);std::size_t used{};
    auto* writer=archive_write_new();check(writer&&archive_write_set_format_zip(writer)==ARCHIVE_OK&&archive_write_set_options(writer,"zip:compression=deflate")==ARCHIVE_OK,"synthetic REP ZIP producer");
    check(archive_write_open_memory(writer,bytes.data(),bytes.size(),&used)==ARCHIVE_OK,"bounded reader-side ZIP memory");
    for(const auto& file:files){auto* entry=archive_entry_new();archive_entry_set_pathname(entry,file.name.c_str());archive_entry_set_filetype(entry,AE_IFREG);archive_entry_set_perm(entry,0600);archive_entry_set_size(entry,file.bytes.size());
        check(archive_write_header(writer,entry)==ARCHIVE_OK&&archive_write_data(writer,file.bytes.data(),file.bytes.size())==static_cast<la_ssize_t>(file.bytes.size()),"complete synthetic REP member");archive_entry_free(entry);}
    check(archive_write_close(writer)==ARCHIVE_OK&&archive_write_free(writer)==ARCHIVE_OK,"complete reader-side ZIP directory");return {bytes.data(),used};
}
}
int main(int argc,char** argv)try{
    check(argc==2||(argc==3&&(std::string(argv[2])=="--network-only"||std::string(argv[2])=="--byte-boundary")),"new NG34 fixture root and optional focused run");const bool network_only=argc==3&&std::string(argv[2])=="--network-only";const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"preserve prior NG34 evidence");
    const auto sid=ins::operator_sid();auto sd=ins::mutable_descriptor(sid);std::filesystem::create_directories(root.parent_path());ins::create_directory(root,sd.get());ins::create_directory(root/L"Data",sd.get());
    storage::SqliteStorageSettings settings;settings.data_directory=root/L"Data";settings.utc_now=[]{return std::int64_t{1793970000};};sfx::test::FakeStoragePathPolicy paths;storage::SqliteStorage db(settings,paths);
    check(db.start().succeeded(),"production schema starts");check(db.health().schema_version==29,"current beta2 schema29");check(db.shutdown().succeeded(),"stop before trusted synthetic actors");const auto file=root/L"Data"/L"SFX.DB";seed(file);check(db.start().succeeded(),"seeded production storage starts");
    app::MessageActor alice{id(2),2,1,1},bob{id(3),3,1,1};
    fallback_selection_checks(db,bob);
    auto prefs=app::StorageResult<app::QwkSavedPreferences>{};
    if(argc==3&&std::string(argv[2])=="--byte-boundary")prefs=db.qwk_preferences(bob);
    if(argc==3&&std::string(argv[2])=="--byte-boundary"){byte_boundary(db,bob);check(db.shutdown().succeeded(),"boundary fixture stopped");std::cout<<"PASS NG34 byte-boundary "<<checks<<'\n';return 0;}
    app::QwkPacketMetadata meta;meta.board_name="NG34 Synthetic";meta.sysop="Sysop";meta.caller_name="Bob";meta.created="11-06-2026,09:00:00";
    const auto date=[](std::int64_t){return arc::QwkRecordDate{"11-06-26","09:00","1793970000"};};unsigned operation=1000;
    check(scalar(file,"SELECT count(*) FROM sfx_qwk_caller_preferences")==0,"fresh callers have never entered Settings");
    const auto first_empty=sfx::apps::runtime::prepare_caller_qwk_download(db,bob,id(++operation),meta,date);
    check(first_empty.result.ok()&&first_empty.notice==app::kQwkEmptyNotice&&!first_empty.progress,"first-ever empty Download succeeds without visiting Settings");
    prefs=db.qwk_preferences(bob);check(prefs&&prefs.value->preferences.word_wrap==db.qwk_policy({true,{}}).value->word_wrap_default,"first Download derives missing wrap default from owner policy");
    auto explicit_choice=prefs.value->preferences;explicit_choice.word_wrap=true;const auto chosen_preferences=db.save_qwk_preferences(bob,prefs.value->revision,explicit_choice);check(bool(chosen_preferences),"explicit caller packet choice");
    check(sfx::apps::runtime::prepare_caller_qwk_download(db,bob,id(++operation),meta,date).result.ok()&&db.qwk_preferences(bob).value->revision==chosen_preferences.value->revision&&db.qwk_preferences(bob).value->preferences.word_wrap==true,"Download preserves existing explicit preferences/revision");
    Measured measured(db);std::ofstream timings(root/L"timings.tsv");timings<<"case\tms\tarchive_error\tstorage_code\tdetail\tdelivered\tnext_calls\tnext_us\treference_calls\treference_us\tbatch_calls\tbatch_us\n";
    const auto prepare=[&](const char* name){measured.reset();const auto start=std::chrono::steady_clock::now();auto packet=arc::prepare_qwk_download(measured,bob,id(++operation),meta,date);const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();timings<<name<<'\t'<<elapsed<<'\t'<<static_cast<unsigned>(packet.result.error)<<'\t'<<static_cast<unsigned>(packet.storage.code)<<'\t'<<packet.result.detail<<'\t'<<(packet.progress?packet.progress->delivered.size():0)<<'\t'<<measured.next_calls<<'\t'<<measured.next_us<<'\t'<<measured.reference_calls<<'\t'<<measured.reference_us<<'\t'<<measured.batch_calls<<'\t'<<measured.batch_us<<'\n';timings.flush();std::cout<<name<<" ms="<<elapsed<<" result="<<static_cast<unsigned>(packet.result.error)<<" storage="<<static_cast<unsigned>(packet.storage.code)<<" detail="<<packet.result.detail<<std::endl;return packet;};
    auto empty=prepare("genuine-empty");check(empty.result.ok()&&empty.storage&&empty.notice==app::kQwkEmptyNotice&&!empty.progress,"genuine empty is successful explicit notice, no ZIP/progress");
    const auto post=[&](const app::MessageTarget& target,std::string subject,std::string body){auto context=db.begin_message(alice,target);check(bool(context),"ordinary message context");auto result=db.commit_message({alice,id(++operation),*context.value,std::move(subject),std::move(body)});check(bool(result),"ordinary production message commit");return result.value->id;};
    app::MessageTarget chat{app::MessageKind::public_echo,{2},{},{}};const auto first_public=post(chat,"Public without personal mail","Public body\n");
    const app::MessageActor first_public_actor{id(1),1,1,1};const auto first_public_download=sfx::apps::runtime::prepare_caller_qwk_download(db,first_public_actor,id(++operation),meta,date);
    check(first_public_download.result.ok()&&first_public_download.progress&&first_public_download.progress->delivered.size()==1,"first-ever public Download succeeds without visiting Settings");
    check(scalar(file,"SELECT count(*) FROM sfx_local_messages WHERE recipient_account_id=3")==0,"zero personal new-mail fixture");
    auto public_only=prepare("public-zero-personal");check(public_only.result.ok()&&public_only.storage&&public_only.progress&&public_only.progress->delivered.size()==1,"public selection succeeds despite zero personal mail");
    check(scalar(file,"SELECT count(*) FROM sfx_caller_echo_state")==0,"preparation does not advance public read pointers");
    app::MessageTarget mail{app::MessageKind::private_local,{},3,{}};post(mail,"Unread private","Private body\n");
    auto with_private=prepare("private-unread");check(with_private.result.ok()&&with_private.progress&&with_private.progress->delivered.size()==2,"unread private and public export together");
    auto config=*db.ftn_configuration({true,{}}).value;config.settings.listener_enabled=false;
    app::FtnNetwork network;network.code="FIDO";network.name="Synthetic FidoNet";network.address="1:105/49@fidonet";network.enabled=true;config.networks.push_back(network);
    app::FtnLink link;link.network="FIDO";link.code="PEER";link.name="Synthetic target";link.address="1:105/44@fidonet";link.host="127.0.0.1";link.enabled=true;link.poll_minutes=0;link.credentials.session={1};config.links.push_back(link);
    const auto saved=db.change_ftn_configuration({true,{}},id(++operation).bytes,config);check(bool(saved),"synthetic NetMail route; no engine/listener/hub");
    const auto policy=*db.qwk_caller_policy(bob).value;unsigned packet_number=0;
    app::QwkReplySourceInfo last_info;
    const auto import=[&](const char* name,std::vector<std::string> records,std::string headers={}){
        std::string data(128,' ');data.replace(0,policy.board_id.size(),policy.board_id);for(const auto& r:records)data+=r;
        std::vector<arc::QwkArchiveFile> members{{policy.board_id+".MSG",data}};if(!headers.empty())members.push_back({"HEADERS.DAT",std::move(headers)});const auto zip=reply_zip(members);
        auto archive=arc::read_qwk_reply_zip(zip,policy.board_id);check(archive.result.ok(),"production REP archive validation");std::vector<app::QwkFile> files;std::uint64_t expanded{};
        for(auto& f:archive.files){expanded+=f.bytes.size();files.push_back({std::move(f.name),std::move(f.bytes)});}auto decoded=app::decode_qwk_reply(policy.board_id,files);check(bool(decoded),"production reply codec");
        const auto job=root/(L"REP-"+std::to_wstring(++packet_number));ins::create_directory(job,sd.get());auto sealed=sfx::apps::runtime::seal_qwk_reply_spool(job,sid,decoded,zip.size(),expanded);check(bool(sealed),"actual protected native source sealed");
        last_info=sealed.value->get()->info();const auto result=db.import_qwk_reply(bob,*sealed.value);std::cout<<name<<" accepted="<<bool(result)<<" code="<<static_cast<unsigned>(result.status.code)<<" detail="<<result.status.operation<<std::endl;return result;
    };
    check(bool(import("ordinary-public",{wire(2,false,"All Callers","Public REP","Public reply\n")})),"ordinary public REP import");
    check(bool(import("ordinary-local",{wire(0,true,"Alice","Local REP","Private local reply\n")})),"ordinary local REP import");
    const auto qwk_plan=db.begin_qwk_packet(bob,id(++operation));check(qwk_plan&&qwk_plan.value->netmail_conference,"reserved separate NetMail conference");const auto conference=*qwk_plan.value->netmail_conference;
    check(bool(import("qwk-netmail-new",{wire(conference,true,"Rob","@1:105/44","Subject: Offline new NetMail\n\nNew mail\n")})),"ordinary addressed QWKE NetMail to synthetic1:105/44");
    config=*db.ftn_configuration({true,{}}).value;
    const auto digest=[&](std::string_view bytes){app::FtnDigest result{};check(crypto_hash_sha256(result.data(),reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size())==0,"synthetic custody digest");return result;};
    app::FtnImportRequest incoming;incoming.operation=id(++operation);incoming.ingress=id(++operation);incoming.network="FIDO";incoming.expected_configuration_revision=config.revision;
    incoming.header.origin=*app::parse_ftn_address("1:105/44@fidonet");incoming.header.destination=*app::parse_ftn_address("1:105/49@fidonet");incoming.header.year=2026;incoming.header.month=11;incoming.header.day=6;
    const std::string remote_name="Remote Person Long Full Name";
    auto& message=incoming.message;message.origin_net=105;message.origin_node=44;message.destination_net=105;message.destination_node=49;message.date="06 Nov 26  09:00:00";message.to="Bob";message.from=remote_name;message.subject="Incoming NetMail";message.text="\1MSGID: 1:105/44 55667788\r\1CHRS: CP437 2\rOriginal incoming body\r";
    auto inspected=app::inspect_ftn_message(incoming.header,message);check(bool(inspected),"synthetic received metadata valid");auto material=app::ftn_message_fingerprint_material(message,*inspected);incoming.identity=digest(material.identity);incoming.content=digest(material.content);
    const auto packet=app::encode_ftn_packet({incoming.header,{message}});check(bool(packet),"native packet encoder for synthetic custody");app::FtnIngressReceipt ingress;ingress.id=incoming.ingress;ingress.session=id(++operation);ingress.identity=digest("NG34 ingress");ingress.network="FIDO";ingress.link="PEER";ingress.proof=app::FtnIngressProof::inbound_cram;ingress.configuration_revision=config.revision;ingress.file={"ng34.pkt",packet->size(),1793970000};
    check(bool(db.begin_ftn_ingress({true,{}},ingress))&&db.advance_ftn_ingress({true,{}},ingress.id,0,packet->size(),digest(*packet),app::FtnIngressState::received,{}).succeeded(),"synthetic host custody reaches production receipt boundary");
    auto received=db.import_ftn_message({true,{}},incoming);if(!received)std::cerr<<"incoming detail="<<received.status.operation<<'\n';check(received&&received.value->message,"received FTN private message through production import");
    auto remote_packet=prepare("received-ftn-netmail");check(remote_packet.result.ok()&&remote_packet.progress&&remote_packet.progress->delivered.size()==3,"received FTN/private/public export without Read mutation");
    const auto reference=db.qwk_reference(bob,received.value->message->id);check(bool(reference),"permanent received NetMail reference");
    check(bool(import("qwk-netmail-reply",{wire(conference,true,remote_name,"@1:105/44","To: "+remote_name+"\nSubject: Offline NetMail reply\n\nReply body\n",*reference.value)})),"reply to downloaded NetMail uses full QWKE recipient/proven original network");
    const auto full_header="[80]\r\nIn-Reply-To: "+app::qwk_message_identifier(policy.board_id,*reference.value)+"\r\n";
    check(bool(import("single-network-full-header-only-reply",{wire(conference,true,remote_name,"@1:105/44","To: "+remote_name+"\nSubject: Header-only reply\n\nBody\n")},full_header)),"supported full-header parent works on one network");
    auto other=network;other.code="OTHER";other.name="Second synthetic network";other.address="1:205/49@other";config.networks.push_back(other);auto other_link=link;other_link.network="OTHER";other_link.address="1:105/44@other";config.links.push_back(other_link);check(bool(db.change_ftn_configuration({true,{}},id(++operation).bytes,config)),"two explicitly routable synthetic networks");
    const auto messages_before_choice=scalar(file,"SELECT count(*) FROM sfx_local_messages");const auto chosen=import("ranked-new-netmail",{wire(conference,true,"Rob","@1:105/44","Subject: Ranked new\n\nBody\n")});
    check(chosen&&scalar(file,"SELECT count(*) FROM sfx_local_messages")==messages_before_choice+1,"new NetMail creates exactly one message among two eligible networks");
    check(scalar(file,"SELECT count(*) FROM sfx_ftn_message_targets WHERE message_id=(SELECT max(message_id) FROM sfx_local_messages) AND network='FIDO' AND source_aka='1:105/49@fidonet'")==1,"destination zone and net select matching local source AKA");
    check(db.checkpoint().succeeded(),"writer barrier before import reconciliation");const auto accepted=db.qwk_import_outcome(bob,last_info);check(accepted&&accepted.value&&accepted.value->has_value(),"ranked import retains durable reconciliation receipt");
    check(bool(import("explicit-domain-new-netmail",{wire(conference,true,"Rob","@1:105/44@fidonet","Subject: Explicit domain\n\nBody\n")})),"explicit domain selects only its configured network");
    check(bool(import("two-network-fixed-reference-reply",{wire(conference,true,remote_name,"@1:105/44","To: "+remote_name+"\nSubject: Fixed reply\n\nBody\n",*reference.value)})),"fixed reply reference scopes original network");
    const auto full_reply=import("two-network-full-header-only-reply",{wire(conference,true,remote_name,"@1:105/44","To: "+remote_name+"\nSubject: Linked full-header reply\n\nBody\n")},full_header);
    check(bool(full_reply),"supported full-header-only reply scopes the same owned original network");
    const auto foreign_link=app::qwk_message_identifier("OTHER",*reference.value);check(!foreign_link.empty(),"valid distinct board identifier yields real foreign link");
    for(const auto& bad_link:{std::string("<garbage>"),foreign_link}){
        const auto before=scalar(file,"SELECT count(*) FROM sfx_local_messages");
        check(!import("unusable-full-header-link",{wire(conference,true,remote_name,"@1:105/44","To: "+remote_name+"\nSubject: Invalid link\n\nBody\n")},"[80]\r\nIn-Reply-To: "+bad_link+"\r\n")&&scalar(file,"SELECT count(*) FROM sfx_local_messages")==before,"malformed or foreign link cannot disambiguate networks or create messages");
    }
    check(!import("conflicting-full-header-link",{wire(conference,true,remote_name,"@1:105/44","To: "+remote_name+"\nSubject: Conflicting link\n\nBody\n",*reference.value+1)},full_header),"conflicting numeric/full-header reference is refused");
    const auto missing_ranked=import("missing-full-header-original",{wire(conference,true,remote_name,"@1:105/44","To: "+remote_name+"\nSubject: Missing original\n\nBody\n")},"[80]\nIn-Reply-To: "+app::qwk_message_identifier(policy.board_id,999999)+"\n");
    check(missing_ranked&&missing_ranked.value->missing_parents==1,"missing original uses independently authorized ranked destination and reports missing parent");
    auto other_incoming=incoming;other_incoming.operation=id(++operation);other_incoming.ingress=id(++operation);other_incoming.expected_configuration_revision=db.ftn_configuration({true,{}}).value->revision;
    other_incoming.message.to="Alice";other_incoming.message.text="\1MSGID: 1:105/44 55667789\r\1CHRS: CP437 2\rOther caller private original\r";
    const auto other_inspected=app::inspect_ftn_message(other_incoming.header,other_incoming.message);check(bool(other_inspected),"other caller metadata");const auto other_material=app::ftn_message_fingerprint_material(other_incoming.message,*other_inspected);other_incoming.identity=digest(other_material.identity);other_incoming.content=digest(other_material.content);
    const auto other_packet=app::encode_ftn_packet({other_incoming.header,{other_incoming.message}});check(bool(other_packet),"other private host packet");auto other_ingress=ingress;other_ingress.id=other_incoming.ingress;other_ingress.session=id(++operation);other_ingress.identity=digest("NG34 other inbox ingress");other_ingress.configuration_revision=other_incoming.expected_configuration_revision;other_ingress.file={"other.pkt",other_packet->size(),1793970000};
    check(bool(db.begin_ftn_ingress({true,{}},other_ingress))&&db.advance_ftn_ingress({true,{}},other_ingress.id,0,other_packet->size(),digest(*other_packet),app::FtnIngressState::received,{}).succeeded(),"other inbox custody");const auto other_received=db.import_ftn_message({true,{}},other_incoming);check(other_received&&other_received.value->message,"real received private message for another caller");const auto other_reference=db.qwk_reference(alice,other_received.value->message->id);check(bool(other_reference),"only intended caller can allocate other private original reference");
    const auto before_other=scalar(file,"SELECT count(*) FROM sfx_local_messages"),before_receipts=scalar(file,"SELECT count(*) FROM sfx_qwk_rep_receipts");
    check(!import("other-inbox-full-header-reference",{wire(conference,true,remote_name,"@1:105/44","To: "+remote_name+"\nSubject: Other inbox reference\n\nBody\n")},"[80]\nIn-Reply-To: "+app::qwk_message_identifier(policy.board_id,*other_reference.value)+"\n")&&scalar(file,"SELECT count(*) FROM sfx_local_messages")==before_other&&scalar(file,"SELECT count(*) FROM sfx_qwk_rep_receipts")==before_receipts,"other caller private original cannot choose network or create messages/receipt");
    const auto no_route=import("no-route-netmail",{wire(conference,true,"Rob","@1:105/44@unconfigured","Subject: No route\n\nBody\n")});
    check(!no_route&&no_route.status.operation=="No configured FTN route for reply destination"&&sfx::apps::runtime::qwk_import_notice(no_route.status,true).find("No configured FTN network")!=std::string::npos,"missing route has bounded safe diagnosis");
    config.networks.pop_back();config.links.pop_back();config.revision=db.ftn_configuration({true,{}}).value->revision;
    check(bool(db.change_ftn_configuration({true,{}},id(++operation).bytes,config)),"restore single-network fixture for independent line-ending cases");
    const auto missing_ok=import("unambiguous-missing-parent",{wire(conference,true,"Rob","@1:105/44","Subject: Independent valid destination\n\nBody\n")},"[80]\nIn-Reply-To: "+app::qwk_message_identifier(policy.board_id,999999)+"\n");check(missing_ok&&missing_ok.value->missing_parents==1,"independently authorized unambiguous destination preserves missing-parent import");
    if(network_only){check(db.shutdown().succeeded(),"focused network fixture stopped");std::cout<<"PASS NG34 network "<<checks<<'\n';return 0;}
    for(unsigned n=1;n<2208;++n)post(chat,"Catalog message "+std::to_string(n),"Representative public text\n");
    auto medium=prepare("2208-public-catalog");check(medium.result.ok()&&medium.storage&&medium.progress&&medium.progress->delivered.size()==2210&&!medium.progress->more,"complete2208 public plus two unread private exports without stranded remainder");
    check(scalar(file,"SELECT sum(opened) FROM sfx_message_recipient_state WHERE recipient_account_id=3")==0&&scalar(file,"SELECT count(*) FROM sfx_qwk_continuation")==0,"large preparation leaves private Read and delivery continuation unchanged");
    {std::ofstream archive(root/L"prepared-2208.qwk",std::ios::binary);archive.write(medium.zip.data(),static_cast<std::streamsize>(medium.zip.size()));}
    const std::string pi_body=std::string("Supported character ")+static_cast<char>(0xe3)+" remains on this line\nNext line\n";
    const auto pi_id=post(chat,"CP437 pi",pi_body);
    auto pi=prepare("classic-pi-success");check(pi.result.ok()&&pi.storage&&pi.progress&&pi.progress->delivered.size()==2211&&!pi.progress->more,"classic fallback exports every selected message including pi");
    const auto pi_members=download_members(pi.zip);const auto& pi_dat=pi_members.at("MESSAGES.DAT");
    const auto pi_header=pi_dat.find("CP437 pi");check(pi_header!=std::string::npos&&pi_header>=71,"classic packet contains actual offending fixture header");
    const auto pi_offset=pi_header-71;const std::string classic_body="Supported character * remains on this line\xe3Next line\xe3";
    check(pi_offset%128==0&&pi_dat[pi_offset+122]==static_cast<char>(0xe1)&&pi_dat.substr(pi_offset+128,classic_body.size())==classic_body,"actual builder ZIP preserves framing/newlines and replaces only content pi");
    check(db.quote_source(bob,pi_id).value->body.canonical_lf==pi_body&&scalar(file,"SELECT sum(opened) FROM sfx_message_recipient_state WHERE recipient_account_id=3")==0&&scalar(file,"SELECT count(*) FROM sfx_qwk_continuation")==0,"classic preparation leaves stored message, Read flags and delivery continuation unchanged");
    {std::ofstream archive(root/L"prepared-classic-pi.qwk",std::ios::binary);archive.write(pi.zip.data(),static_cast<std::streamsize>(pi.zip.size()));}
    prefs=db.qwk_preferences(bob);auto unicode=prefs.value->preferences;unicode.encoding=app::QwkEncoding::utf8;unicode.full_headers=true;check(bool(db.save_qwk_preferences(bob,prefs.value->revision,unicode)),"explicit reader-compatible UTF8 selection");
    auto utf8=prepare("utf8-pi-success");check(utf8.result.ok()&&utf8.storage&&utf8.progress&&utf8.progress->delivered.size()==2211&&!utf8.progress->more,"explicit UTF8 preserves every selected message including pi");
    check(download_members(utf8.zip).at("MESSAGES.DAT").find("Supported character \xcf\x80 remains on this line\nNext line\n")!=std::string::npos&&db.quote_source(bob,pi_id).value->body.canonical_lf==pi_body,"UTF8 actual ZIP and original storage retain pi");
    // QWKE1.02 explicitly permits CR or QWK-pi terminated kludge lines.
    // A long recipient needs the kludge; the fixed25-cell field is insufficient.
    const auto cr_new=import("qwk-netmail-cr-only-new",{wire(conference,true,"Rob","@1:105/44","Subject: CR-only new subject\r\rNew body\r")});
    const bool cr_new_subject=scalar(file,"SELECT count(*) FROM sfx_local_messages WHERE CAST(subject AS TEXT)='CR-only new subject'")==1;
    std::cout<<"CR-only-new subject-preserved="<<cr_new_subject<<'\n';
    const auto cr_only=import("qwk-netmail-cr-only-reply",{wire(conference,true,remote_name,"@1:105/44","To: "+remote_name+"\rSubject: CR-only NetMail reply\r\rReply body\r",*reference.value)});
    const bool cr_reply_subject=scalar(file,"SELECT count(*) FROM sfx_local_messages WHERE CAST(subject AS TEXT)='CR-only NetMail reply'")==1;
    const auto mixed=import("qwk-netmail-mixed",{wire(conference,true,"Rob","@1:105/44","Subject: Mixed kludge ending\r\n\rTo: Keep as body\nFrom: Keep too\r\nEnd\r")});
    const auto mixed_id=scalar(file,"SELECT coalesce(max(message_id),0) FROM sfx_local_messages WHERE CAST(subject AS TEXT)='Mixed kludge ending'");
    bool mixed_body=false;if(mixed_id){const auto mixed_source=db.quote_source(bob,{mixed_id});mixed_body=mixed&&mixed_source&&mixed_source.value->body.canonical_lf=="To: Keep as body\nFrom: Keep too\nEnd\n";}
    std::cout<<"mixed-ending remaining-body-preserved="<<mixed_body<<'\n';
    const auto before_invalid=scalar(file,"SELECT count(*) FROM sfx_local_messages");
    const auto forged=import("qwk-netmail-cr-forged-from",{wire(2,false,"All Callers","Should rollback","First message\n"),wire(conference,true,"Rob","@1:105/44","From: Alice\rSubject: Forged\r\rBody\r")});
    const bool forged_refused=!forged&&scalar(file,"SELECT count(*) FROM sfx_local_messages")==before_invalid;
    std::cout<<"CR-forged From whole-packet-refused="<<forged_refused<<'\n';
    const auto duplicate=import("qwk-netmail-cr-duplicate-field",{wire(conference,true,"Rob","@1:105/44","Subject: First\rSubject: Second\r\rBody\r")});
    const bool duplicate_refused=!duplicate&&scalar(file,"SELECT count(*) FROM sfx_local_messages")==before_invalid;
    std::cout<<"CR-duplicate-field-refused="<<duplicate_refused<<'\n';
    check(bool(cr_new)&&cr_new_subject&&bool(cr_only)&&cr_reply_subject,"valid CR-only QWKE new/reply NetMail preserves subject and full recipient");
    check(mixed_body,"mixed endings consume only kludges/one separator and preserve ordinary body");
    check(forged_refused&&duplicate_refused,"CR support preserves full author checks/duplicate fields/atomic import");
    const auto add1=post(chat,"Batch first","Batch body\n"),add2=post(chat,"Batch second","Batch body\n");
    const auto refs_before=scalar(file,"SELECT count(*) FROM sfx_qwk_message_reference");
    {internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);c.exec(("CREATE TRIGGER ng34_batch_fault BEFORE INSERT ON sfx_qwk_message_reference WHEN NEW.message_id="+std::to_string(add2.value)+" BEGIN SELECT RAISE(ABORT,'NG34 owned reference fault'); END").c_str(),"owned bounded reference-batch fault");}
    check(!db.qwk_references(bob,{add1,add2})&&scalar(file,"SELECT count(*) FROM sfx_qwk_message_reference")==refs_before,"batch writer failure rolls back all reference allocations");
    {internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);c.exec("DROP TRIGGER ng34_batch_fault","remove only this new fixture fault");}
    auto first=std::async(std::launch::async,[&]{return db.qwk_references(bob,{add1,add2});});auto second=std::async(std::launch::async,[&]{return db.qwk_references(bob,{add1,add2});});
    const auto refs1=first.get(),refs2=second.get();check(refs1&&refs2&&*refs1.value==*refs2.value&&scalar(file,"SELECT count(*) FROM sfx_qwk_message_reference")==refs_before+2,"concurrent misses allocate one stable mapping per message");
    check(db.qwk_reference(bob,add1).value==std::optional<std::uint32_t>{refs1.value->front()},"single existing-reference read shares exact stable identity");
    check(!db.qwk_references(bob,std::vector<app::MessageId>(33,add1))&&!db.qwk_references(bob,{}),"production reference API enforces nonempty max32");
    auto forged_actor=bob;forged_actor.session=alice.session;check(!db.qwk_references(forged_actor,{add1})&&!db.qwk_reference(forged_actor,add1),"existing fast paths recheck current actor rather than return cached authority");
    auto denied=post({app::MessageKind::private_local,{},2,{}},"Other Inbox","Other private body\n");const auto refs_guard=scalar(file,"SELECT count(*) FROM sfx_qwk_message_reference");
    check(!db.qwk_references(bob,{add1,denied})&&scalar(file,"SELECT count(*) FROM sfx_qwk_message_reference")==refs_guard,"inaccessible item cannot trigger fallback allocation or leak private reference");
    const auto bounded_policy=db.save_qwk_policy({true,{}},id(++operation),policy.revision,policy.board_id,7);check(bool(bounded_policy),"bounded seven-record packet policy");
    auto limited=prepare("seven-record-boundary");check(limited.result.ok()&&limited.progress&&limited.progress->delivered.size()==7&&limited.progress->more&&limited.progress->final_queues.front().after_arrival>limited.progress->initial.queues.front().after_arrival,"count boundary delivers only encoded seven records and retains remainder");
    unsigned checkpoints{};auto cancelled=arc::prepare_qwk_download(db,bob,id(++operation),meta,date,[&]{return ++checkpoints>27;});
    check(!cancelled.result.ok()&&!cancelled.progress&&scalar(file,"SELECT sum(opened) FROM sfx_message_recipient_state WHERE recipient_account_id=3")==0,"cancellation across prefetch/batch/encoding has no delivered progress or Read effects");
    bool batch_finished=false;measured.after_batch=[&](const auto&){batch_finished=true;};
    auto cancelled_after_batch=arc::prepare_qwk_download(measured,bob,id(++operation),meta,date,[&]{return batch_finished;});measured.after_batch={};
    check(batch_finished&&cancelled_after_batch.result.error==arc::Error::cancelled&&!cancelled_after_batch.progress&&scalar(file,"SELECT count(*) FROM sfx_qwk_continuation")==0,"explicit cancellation immediately after batch allocation returns no delivered cursors");
    app::MessageId online_read{};measured.after_batch=[&](const auto& ids){if(!online_read.value){online_read=ids.front();check(bool(db.open_message(bob,online_read)),"ordinary online read between prefetch and encoding");}};
    auto concurrent_read=prepare("online-read-between-prefetch");measured.after_batch={};
    check(concurrent_read.result.ok()&&concurrent_read.progress&&concurrent_read.progress->delivered.size()==7&&std::none_of(concurrent_read.progress->delivered.begin(),concurrent_read.progress->delivered.end(),[&](const auto& item){return item.message==online_read;}),"forward online progress rebases pending IDs without generic refusal or skipped eligible messages");
    bool public_read=false;measured.after_batch=[&](const auto& ids){if(!public_read&&std::find(ids.begin(),ids.end(),first_public)!=ids.end()){check(bool(db.open_message(bob,first_public)),"ordinary public online read advances highest-opened during prefetch");public_read=true;}};
    auto concurrent_public=prepare("public-read-between-prefetch");measured.after_batch={};
    check(public_read&&concurrent_public.result.ok()&&concurrent_public.progress&&concurrent_public.progress->delivered.size()==7&&std::none_of(concurrent_public.progress->delivered.begin(),concurrent_public.progress->delivered.end(),[&](const auto& item){return item.message==first_public;}),"public highest-opened progress rebases pending IDs while preserving remaining eligible messages");
    {internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);c.exec("UPDATE sfx_local_echoes SET reading_minimum=999,revision=revision+1 WHERE echo_id=2","owned fixture current area access change");}
    check(!db.qwk_references(bob,{add1})&&!db.qwk_reference(bob,add1),"already-issued references still enforce changed current area read access");
    check(sfx::apps::runtime::qwk_import_notice({app::StorageErrorCode::invalid_argument,"C:\\private\\not-for-caller"},false).find("could not be confirmed")!=std::string::npos&&sfx::apps::runtime::qwk_import_notice({app::StorageErrorCode::invalid_argument,"C:\\private\\not-for-caller"},true).find("private")==std::string::npos,"caller notices distinguish unknown outcome and never expose raw operation paths");
    arc::QwkPreparedDownload busy;busy.result.error=arc::Error::source_changed;busy.storage={app::StorageErrorCode::busy,"private diagnostic"};check(sfx::apps::runtime::qwk_preparation_notice(busy).find("storage is busy")!=std::string::npos,"busy storage is not falsely diagnosed as changed settings");
    busy.storage.code=app::StorageErrorCode::io;check(sfx::apps::runtime::qwk_preparation_notice(busy).find("storage is unavailable")!=std::string::npos,"I/O refusal is not falsely diagnosed as changed settings");
    busy.storage.code=app::StorageErrorCode::conflict;check(sfx::apps::runtime::qwk_preparation_notice(busy).find("changed during preparation")!=std::string::npos,"real revision conflict retains changed-state diagnosis");
    std::vector<std::string> notices;
    for(const auto code:{app::StorageErrorCode::none,app::StorageErrorCode::busy,app::StorageErrorCode::io,app::StorageErrorCode::conflict,app::StorageErrorCode::invalid_argument}){
        busy.storage.code=code;notices.push_back(sfx::apps::runtime::qwk_preparation_notice_text(busy));notices.push_back(sfx::apps::runtime::qwk_import_notice_text(busy.storage,true));notices.push_back(sfx::apps::runtime::qwk_import_notice_text(busy.storage,false));
    }
    notices.push_back(sfx::apps::runtime::qwk_preparation_notice_text(pi));
    notices.push_back(sfx::apps::runtime::qwk_import_notice_text({app::StorageErrorCode::invalid_argument,"FTN network is ambiguous; use online NetMail to select the network"},true));
    for(const auto& text:notices){const auto wrapped=sfx::apps::runtime::qwk_notice_lines(text);std::istringstream lines(wrapped);std::string line;while(std::getline(lines,line))check(line.size()<=76,"fixed QWK notice fits76 columns");
        const auto words=[](const std::string& input){std::istringstream stream(input);std::vector<std::string> out;std::string word;while(stream>>word)out.push_back(word);return out;};check(words(text)==words(wrapped),"notice word wrapping preserves every complete word and punctuation");}
    PROCESS_MEMORY_COUNTERS memory{};memory.cb=sizeof(memory);check(GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory))!=FALSE,"measure native test peak working set");std::cout<<"PEAK_WORKING_SET_BYTES "<<memory.PeakWorkingSetSize<<'\n';
    check(db.shutdown().succeeded(),"production storage stops");std::cout<<"PASS NG34 baseline "<<checks<<" checks; source="<<root.string()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"FAIL NG34 after "<<checks<<": "<<e.what()<<'\n';return 1;}
