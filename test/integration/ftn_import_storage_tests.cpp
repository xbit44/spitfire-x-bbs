// Actual managed SQLite import transactions; ingress authentication and custody
// are synthetic host-created ports. This is not a native BinkP/peer test.
#define main retained_import_file_fixture_main
#include "file_storage_tests.cpp"
#undef main
#include "sfx/application/ftn_work.hpp"
#include "sfx/application/message_session.hpp"
#include "sfx/test/terminal_stream_fake.hpp"
#include "../unit/ftn_legacy_fixtures.hpp"
#include <sodium.h>
namespace {
app::FtnDigest digest(std::string_view s){app::FtnDigest d{};check(crypto_hash_sha256(d.data(),reinterpret_cast<const unsigned char*>(s.data()),s.size())==0,"test SHA256 available");return d;}
void fingerprint(app::FtnImportRequest& r){const auto m=app::inspect_ftn_message(r.header,r.message);check(bool(m),"independent fixture metadata valid");const auto material=app::ftn_message_fingerprint_material(r.message,*m);r.identity=digest(material.identity);r.content=digest(material.content);}
app::FtnImportRequest request(unsigned n,std::int64_t revision,bool echo=true){
    app::FtnImportRequest r;r.operation=id(100+n);r.ingress=id(200+n);r.expected_configuration_revision=revision;r.network="TEST";
    r.header.origin=*app::parse_ftn_address("2:100/2@test");r.header.destination=*app::parse_ftn_address("2:100/1@test");r.header.year=2026;r.header.month=10;r.header.day=29;
    auto& m=r.message;m.origin_net=100;m.origin_node=2;m.destination_net=100;m.destination_node=1;m.date="29 Oct 26  12:00:00";m.to=echo?"All":"Alice";m.from="Remote Person";m.subject="Remote subject";
    m.text=(echo?"AREA:TEST.ECHO\r":"")+std::string("\1MSGID: 2:100/2 ")+std::to_string(n)+"\r\1CHRS: CP437 2\rLiteral @BBS@ and ANSI \x1b[31mred\x1b[0m\rSecond line\r";
    fingerprint(r);return r;
}
void custody(storage::SqliteStorage& db,const app::FtnImportRequest& r,app::FtnIngressProof proof=app::FtnIngressProof::inbound_cram){
    const auto wire=app::encode_ftn_packet({r.header,{r.message}});check(bool(wire),"independent packet encodes");app::FtnIngressReceipt receipt;receipt.id=r.ingress;receipt.session=id(300+r.operation.bytes[15]);receipt.identity=digest(std::string("custody")+std::to_string(r.ingress.bytes[15]));receipt.network="TEST";receipt.link=proof==app::FtnIngressProof::nonsecure?"":"PEER";receipt.proof=proof;receipt.configuration_revision=r.expected_configuration_revision;receipt.outbound=proof==app::FtnIngressProof::outbound_tcp;receipt.file={"test.pkt",wire->size(),1700000000};
    if(proof==app::FtnIngressProof::nonsecure)receipt.network.clear();
    if(proof==app::FtnIngressProof::inbound_tcp)receipt.link="EMPTY";
    const auto created=db.begin_ftn_ingress({true,{}},receipt);if(!created)std::cerr<<"fixture ingress "<<static_cast<unsigned>(r.operation.bytes[15])<<": "<<created.status.operation<<'\n';check(bool(created),"synthetic complete ingress reserved");check(db.advance_ftn_ingress({true,{}},receipt.id,0,wire->size(),digest(*wire),app::FtnIngressState::received,{}).succeeded(),"synthetic native custody completion recorded");
}
}
int main(int argc,char** argv){try{
    check(argc==2,"fresh import fixture root required");const std::filesystem::path root=argv[1];check(!std::filesystem::exists(root),"preserve previous fixtures");sfx::test::FakeStoragePathPolicy paths;auto s=settings(root);const auto file=s.data_directory/"SFX.DB";
    {storage::SqliteStorage initial(s,paths);check(initial.start().succeeded(),"fresh final22 schema starts");check(initial.shutdown().succeeded(),"stop before owned synthetic actors");}seed(file);
    storage::SqliteStorage db(s,paths);check(db.start().succeeded(),"managed import writer ready");const app::MessageConfigurationActor owner{true,{}};const app::MessageActor alice{id(2),2,1,1},bob{id(3),3,1,1};
    auto c=*db.ftn_configuration(owner).value;app::FtnNetwork n;n.code="TEST";n.name="Synthetic Test";n.address="2:100/1@test";n.enabled=true;c.networks.push_back(n);app::FtnLink l;l.network="TEST";l.code="PEER";l.name="Synthetic Peer";l.address="2:100/2@test";l.host="127.0.0.1";l.enabled=true;l.credentials.session={1}; /* synthetic sealed-credential port, not DPAPI/native auth */c.links.push_back(l);app::FtnMessageEcho e;e.network="TEST";e.tag="TEST.ECHO";e.description="Synthetic Echo";c.message_echoes.push_back(e);c.permissions.push_back({"TEST","PEER","TEST.ECHO",false,true,true,false});
    auto empty=l;empty.code="EMPTY";empty.address="2:100/3@test";empty.credentials.session.clear();empty.credentials.packet={2};c.links.push_back(empty);c.permissions.push_back({"TEST","EMPTY","TEST.ECHO",false,true,true,false});
    check(bool(db.change_ftn_configuration(owner,id(20).bytes,c)),"native mapping and explicit receive permission committed");c=*db.ftn_configuration(owner).value;
    // Simulated historical receipts, using actual B-decoder-qualified old
    // layouts and this fixture's legitimate catalog counters. Not old Runtime
    // execution or an owner database claim.
    std::vector<std::pair<app::FtnTransferToken,app::FtnConfiguration>> legacy_requests;
    check(db.shutdown().succeeded(),"stop writer before owned historical receipt fixtures");
    {internal::SqliteConnection history(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
        for(auto old:{sfx::test::ftn_legacy::v2,sfx::test::ftn_legacy::v3,sfx::test::ftn_legacy::v4}){
            const auto version=old[0];for(unsigned i=0;i<8;++i){old[9+i]=static_cast<std::uint8_t>(static_cast<std::uint64_t>(c.message_catalog_revision)>>(i*8));old[17+i]=static_cast<std::uint8_t>(static_cast<std::uint64_t>(c.file_catalog_revision)>>(i*8));}
            const auto old_config=app::decode_ftn_configuration(old);check(bool(old_config),"legacy receipt input remains valid in its defined format");const auto token=id(80+version).bytes;const auto hash=digest(std::string_view(reinterpret_cast<const char*>(old.data()),old.size()));internal::SqliteStatement q(history.get(),"INSERT INTO sfx_ftn_configuration_receipts VALUES(?1,?2,?3)","owned historical receipt fixture, not production authority");q.bind(1,token);q.bind(2,hash);q.bind(3,c.revision);q.step();legacy_requests.push_back({token,*old_config});
        }}
    check(db.start().succeeded(),"managed writer reopens historical receipt fixtures");
    for(const auto& old:legacy_requests){const auto replay=db.change_ftn_configuration(owner,old.first,old.second);check(replay&&replay.value->replayed&&replay.value->applied_revision==c.revision,"unchanged legacy operation receipt reconciles after codec5 extension");auto altered=old.second;altered.settings.tls_port=24601;check(!db.change_ftn_configuration(owner,old.first,altered),"new TLS setting cannot masquerade as an old-format operation retry");}
    auto r=request(1,c.revision);custody(db,r);check(!db.import_ftn_message({},r),"wire input cannot acquire owner import authority");auto imported=db.import_ftn_message(owner,r);if(!imported)std::cerr<<imported.status.operation<<'\n';check(imported&&imported.value->message&&!imported.value->replayed,"remote echo commits native content and immutable receipt");const auto message=imported.value->message->id;
    const auto toss_progress=db.ftn_toss_progress(owner,r.ingress);check(toss_progress&&toss_progress.value->size()==1&&toss_progress.value->front().message_index==r.message_index&&toss_progress.value->front().identity==r.identity&&toss_progress.value->front().content==r.content,"owner reads immutable same-receipt identity/content progress before restart replay");check(!db.ftn_toss_progress({},r.ingress)&&!db.ftn_toss_progress(owner,id(65000)),"caller/unknown receipt cannot inspect private toss checkpoints");
    const auto opened=db.open_message(alice,message);check(opened&&opened.value->header.remote_author&&!opened.value->header.author&&opened.value->header.from=="Remote Person","remote author distinctly retained without caller impersonation");check(opened.value->original_source&&opened.value->original_source->bytes.find("@BBS@")!=std::string::npos&&opened.value->original_source->bytes.find("AREA:")==std::string::npos,"literal display source independent of FTN controls and token execution");
    check(bool(db.qwk_reference(alice,message)),"remote creation receipt supports stable authorized QWK reference");check(db.import_ftn_message(owner,r).value->replayed,"lost import reply reconciles exact operation");auto changed=r;changed.message.subject="Changed";fingerprint(changed);check(!db.import_ftn_message(owner,changed),"same operation cannot mutate imported content");
    auto preferences=*db.qwk_preferences(bob).value;preferences.preferences.own_public_posts=false;check(bool(db.save_qwk_preferences(bob,preferences.revision,preferences.preferences)),"QWK own-public-post filter explicitly Off");const auto packet=db.begin_qwk_packet(bob,id(1000));check(bool(packet),"remote-aware finite QWK plan");const auto queue=std::find_if(packet.value->queues.begin(),packet.value->queues.end(),[&](const auto& item){return item.echo==opened.value->header.target.echo;});check(queue!=packet.value->queues.end(),"mapped remote echo appears in authorized QWK catalog");const auto next=db.next_qwk_message(bob,*packet.value,static_cast<std::size_t>(queue-packet.value->queues.begin()));check(next&&next.value->has_value()&&next.value->value().header.identity.id==message,"NULL remote author passes QWK own-post exclusion without fabricated caller zero");
    auto duplicate=request(2,c.revision);duplicate.message=r.message;fingerprint(duplicate);custody(db,duplicate);check(db.import_ftn_message(owner,duplicate).value->replayed,"same network/origin MSGID deduplicates another custody receipt");
    const auto duplicate_progress=db.ftn_toss_progress(owner,duplicate.ingress);check(duplicate_progress&&duplicate_progress.value->size()==1&&duplicate_progress.value->front().identity==r.identity&&duplicate_progress.value->front().content==r.content,"duplicate custody has its own durable progress bound to original immutable content");
    auto collision=request(3,c.revision);collision.message=r.message;collision.message.text+="Different content\r";fingerprint(collision);custody(db,collision);check(!db.import_ftn_message(owner,collision),"MSGID collision with changed content holds instead of overwriting");
    const auto arrival_fence=db.message_caller_statistics(alice,{}).value->last_arrival;
    auto private_mail=request(4,c.revision,false);custody(db,private_mail);const auto received=db.import_ftn_message(owner,private_mail);check(received&&received.value->message,"incoming private FTN delivered to exact real local recipient");const auto mid=received.value->message->id;
    const auto arrivals=db.message_arrival_page(alice,arrival_fence,100);check(arrivals&&arrivals.value->arrivals.size()==1&&arrivals.value->arrivals.front().message==mid,"committed NetMail reaches online arrival dispatcher");
    const auto sender=db.message_arrival_sender(alice,mid);check(sender&&sender.value->has_value()&&**sender.value=="Remote Person","authorized atomic sender snapshot");const auto other=db.message_arrival_sender(bob,mid);check(other&&!other.value->has_value(),"another recipient cannot inspect arrival sender");
    check(db.message_arrival_eligible(alice,mid).value.value()&&scalar(file,("SELECT opened FROM sfx_message_recipient_state WHERE message_id="+std::to_string(mid.value)).c_str())==0,"arrival and sender queries leave NetMail unread");
    check(db.import_ftn_message(owner,private_mail).value->replayed&&db.message_arrival_page(alice,arrival_fence,100).value->arrivals.size()==1,"import replay creates no duplicate notification identity");
    check(bool(db.open_message(alice,mid))&&!db.open_message(bob,mid),"remote private content stays within native recipient privacy");check(!db.message_arrival_sender(alice,mid).value->has_value(),"opened NetMail is suppressed before notification presentation");
    auto unknown=request(5,c.revision,false);unknown.message.to="No such caller";fingerprint(unknown);custody(db,unknown);check(!db.import_ftn_message(owner,unknown),"unmatched private recipient never silently falls back to Sysop");
    auto insecure=request(6,c.revision);custody(db,insecure,app::FtnIngressProof::outbound_tcp);check(!db.import_ftn_message(owner,insecure),"outbound TCP M_OK cannot become authenticated echo ingress");
    auto direct=request(7,c.revision,false);custody(db,direct,app::FtnIngressProof::nonsecure);check(bool(db.import_ftn_message(owner,direct)),"nonsecure ingress limited to explicitly allowed local NetMail");
    auto bad=request(8,c.revision);custody(db,bad);bad.header.password[0]=1;check(!db.import_ftn_message(owner,bad),"packet credential bytes never enter writer import request");
    auto unverified=request(10,c.revision);unverified.header.origin.node=3;unverified.message.origin_node=3;fingerprint(unverified);custody(db,unverified,app::FtnIngressProof::inbound_tcp);check(!db.import_ftn_message(owner,unverified),"configured unverified inbound identity alone grants no Echo permission");
    auto item_verified=request(11,c.revision);item_verified.header.origin.node=3;item_verified.message.origin_node=3;item_verified.packet_password_verified=true;fingerprint(item_verified);custody(db,item_verified,app::FtnIngressProof::inbound_tcp);check(bool(db.import_ftn_message(owner,item_verified)),"separate synthetic packet credential admits only its authorized item");check(!app::ftn_verified_peer(app::FtnIngressProof::inbound_tcp,false)&&!app::ftn_queue_offer_allowed(app::FtnIngressProof::inbound_tcp,false),"item acceptance never upgrades peer proof or unrelated queue pickup");
    check(db.shutdown().succeeded(),"import writer stopped");check(scalar(file,"SELECT count(*) FROM sfx_local_messages WHERE author_account_id IS NULL")==4,"only four native remote messages committed");check(scalar(file,"SELECT count(*) FROM sfx_ftn_import_receipts")==4,"duplicate/collision/refusal cannot create extra import identities");check(scalar(file,"SELECT count(*) FROM sfx_message_commit_receipts")==0,"trusted imports do not fabricate caller posting receipts");check(scalar(file,"SELECT count(*) FROM sfx_message_arrival_intents")==2,"only genuinely addressed private messages create arrival intents");check(scalar(file,"SELECT count(*) FROM sfx_ftn_import_write_scope")==0,"no import write authority escapes transaction");check(scalar(file,"SELECT count(*) FROM pragma_foreign_key_check")==0,"all native and FTN references valid");
    check(db.start().succeeded()&&db.import_ftn_message(owner,r).value->replayed,"restart preserves operation reconciliation without duplicate arrival");
    const app::MessageActor sysop{id(1),1,1,1};
    const auto post=[&](app::MessageTarget target,unsigned operation,std::optional<app::MessageId> parent={}){
        const auto context=db.begin_message(alice,target,parent);if(!context)std::cerr<<"begin addressed FTN: "<<context.status.operation<<'\n';check(bool(context),"native caller receives validated FTN composition context");app::CommitMessageRequest draft{alice,id(operation),*context.value,"Local authored subject","Native local body\n"};const auto saved=db.commit_message(draft);if(!saved)std::cerr<<"commit addressed FTN: "<<saved.status.operation<<'\n';check(bool(saved),"native body/target/receipt/scan intent commit together");check(db.commit_message(draft).value->id==saved.value->id,"uncertain native save retries cannot enqueue twice");return *saved.value;
    };
    app::MessageTarget remote;remote.kind=app::MessageKind::private_ftn;remote.remote=app::MessageRemoteTarget{"TEST","2:100/1@test","2:100/2@test","Remote Person"};
    const auto outgoing=post(remote,900);const auto sent=db.open_message(alice,outgoing.id);check(sent&&sent.value->header.author==alice.account&&!sent.value->header.target.recipient&&sent.value->header.target.remote&&sent.value->header.remote_delivery=="Queued","outgoing author is real caller, target is remote and status only Queued");check(!db.open_message(bob,outgoing.id)&&db.open_message(sysop,outgoing.id),"outgoing history is private to author and authorized Sysop");check(!db.qwk_reference(alice,outgoing.id),"unsupported remote offline destination cannot acquire local QWK inbox reference");
    const auto private_reply=post(remote,901,received.value->message->id);check(db.open_message(alice,private_reply.id).value->header.parent==received.value->message->id,"private remote reply preserves legitimate incoming1 to outgoing2 parent");
    auto public_target=remote;public_target.kind=app::MessageKind::public_echo;public_target.echo=opened.value->header.target.echo;const auto public_reply=post(public_target,902,message);const auto public_view=db.open_message(bob,public_reply.id);check(public_view&&public_view.value->header.to=="Remote Person"&&!public_view.value->header.target.recipient,"public reply keeps immutable remote To without false local recipient");
    auto local_forgery=remote;local_forgery.kind=app::MessageKind::private_local;local_forgery.recipient=3;check(!db.begin_message(alice,local_forgery),"remote metadata cannot grant a local recipient authority");check(!db.begin_message(bob,remote,received.value->message->id),"another caller cannot reply through a private remote conversation");check(!db.begin_message(alice,public_target),"remote public To requires its real original reply identity");
    app::MessageQueryOptions sent_options;sent_options.mode=app::MessageQueryMode::sent;const auto sent_context=db.begin_message_query(alice,sent_options);check(bool(sent_context),"native sent-history context available");const auto sent_headers=db.query_message_headers(alice,*sent_context.value,{},20);check(sent_headers&&sent_headers.value->entries.size()==2,"native From You history includes both outgoing private FTN messages");
    check(!db.delete_message(sysop,id(903),outgoing.id),"outstanding NetMail cannot be lost through message deletion");check(db.open_message(alice,outgoing.id).value->header.remote_delivery=="Queued","refused deletion rolls source/target/receipt back intact");
    std::string outgoing_msgid;
    {internal::SqliteConnection read(file,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);
        internal::SqliteStatement q(read.get(),"SELECT msgid FROM sfx_ftn_message_targets WHERE message_id=?1","read actual immutable outgoing ID for reply fixture");
        q.bind(1,outgoing.id.value);check(q.row(),"outgoing message has immutable MSGID");
        const auto bytes=q.blob(0);outgoing_msgid.assign(bytes.begin(),bytes.end());
        check(!outgoing_msgid.empty()&&!q.row(),"one outgoing MSGID supplies the reply fixture");}
    auto answer=request(9,c.revision,false);answer.message.text="\1REPLY: "+outgoing_msgid+"\r"+answer.message.text;fingerprint(answer);custody(db,answer);const auto reply_import=db.import_ftn_message(owner,answer);if(!reply_import)std::cerr<<"reply import: "<<reply_import.status.operation<<'\n';check(reply_import&&reply_import.value->message,"remote response imports without local author substitution");check(db.open_message(alice,reply_import.value->message->id).value->header.parent==outgoing.id,"incoming1 reply binds immutable network/origin/MSGID to outgoing2 parent");
    // Exercise the importer's parent-presence state through real transactions.
    const auto import_reply=[&](unsigned sequence,const std::string& reply){
        auto incoming=request(sequence,c.revision);
        const auto area_end=incoming.message.text.find('\r');
        check(incoming.message.text.rfind("AREA:",0)==0&&area_end!=std::string::npos,"public reply fixture retains its leading AREA line");
        incoming.message.text.insert(area_end+1,"\1REPLY: "+reply+"\r");
        fingerprint(incoming);custody(db,incoming);
        const auto result=db.import_ftn_message(owner,incoming);
        if(!result)std::cerr<<"parent regression import: "<<result.status.operation<<'\n';
        check(result&&result.value->message,"parent regression reply imports");
        const auto view=db.open_message(bob,result.value->message->id);
        check(bool(view),"parent regression reply is readable in its public echo");
        return view.value->header;
    };
    const auto missing_parent=import_reply(31,"2:100/99 absent");
    check(!missing_parent.parent,"unmatched REPLY stays an independent message");
    const auto unique_parent=import_reply(32,"2:100/2 1");
    check(unique_parent.parent==message,"one permitted public parent is retained");
    auto second_parent=request(33,c.revision);
    second_parent.header.origin.node=3;second_parent.message=r.message;
    second_parent.message.origin_node=3;second_parent.packet_password_verified=true;
    fingerprint(second_parent);custody(db,second_parent,app::FtnIngressProof::inbound_tcp);
    const auto second_import=db.import_ftn_message(owner,second_parent);
    check(second_import&&second_import.value->message&&!second_import.value->replayed&&
        !(second_import.value->message->id==message),"same MSGID from a second authorized origin creates distinct candidate");
    const auto ambiguous_parent=import_reply(34,"2:100/2 1");
    check(!ambiguous_parent.parent,"ambiguous permitted REPLY identities never choose an arbitrary parent");
    const auto checked=db.check_message_integrity(owner,id(910),{});if(!checked)std::cerr<<"integrity: "<<checked.status.operation<<'\n';check(checked&&checked.value->clean,"real shared integrity check accepts legitimate typed remote reply edges and receipts");
    c=*db.ftn_configuration(owner).value;c.settings.mode=app::FtnMailerMode::disabled;check(bool(db.change_ftn_configuration(owner,id(911).bytes,c)),"native FTN disabled for paused queue case");post(remote,912);check(scalar(file,"SELECT count(*) FROM sfx_ftn_message_intents WHERE state=0")==4,"valid local NetMail remains durably queued while FTN is Disabled");
    unsigned ui_operation=1200;
    for(const bool ansi:{false,true}){
        const auto before=scalar(file,"SELECT count(*) FROM sfx_local_messages WHERE kind=2");sfx::test::DeterministicInteractiveByteStreamFake stream;stream.script_non_secret_text(ansi?"N\r\nRemote Person\r\n2:100/2@test\r\nUI subject\r\nBody\x1aY\r\n":"N\r\nRemote Person\r\n2:100/2@test\r\nUI subject\r\nBody\r\n/S\r\nY\r\n");app::StreamCallerTerminal terminal(stream);app::MessageComposePorts compose{db,terminal,[&]{return std::optional<app::MessageActor>{alice};},[&]{return id(ui_operation++);},[ansi]{return app::MessageEditorDimensions{ansi,80,24};},[&]{return terminal.read_editor_byte();},[&](std::string_view text){return stream.write_some(text).outcome==app::InteractiveStreamOutcome::completed;}};app::MessageCallerSession session({compose,[]{return std::string("Alice");},[](std::int64_t){return std::string("2026-10-29");},[]{return true;},{}});
        check(session.initialize()&&session.action(app::MenuAction::message_enter).has_value(),"real caller session/editor code runs deterministic ANSI/plain NetMail script");check(scalar(file,"SELECT count(*) FROM sfx_local_messages WHERE kind=2")==before+1,"generated N flow commits exactly one private FTN message");const auto& output=stream.output();check(output.find("NetMail")!=std::string::npos&&output.find("2:100/2@test")!=std::string::npos&&output.find("2:100/1@test")!=std::string::npos&&output.find("Link delivery is pending")!=std::string::npos,"caller sees named network, full immutable target/source and truthful queued feedback");
    }
    for(bool ansi:{false,true})for(bool escape:{false,true}){
        sfx::test::DeterministicInteractiveByteStreamFake stream;stream.script_non_secret_text(std::string("\r\nBlank-subject NetMail body")+(ansi&&escape?"":"\r\n"));
        if(escape){if(ansi){stream.script_non_secret_text("\x1b");stream.script_read_outcome(app::InteractiveStreamOutcome::timed_out);}else stream.script_non_secret_text("/?\r\n");stream.script_non_secret_text("S\r\nY\r\n");}else stream.script_non_secret_text("/s\r\nY\r\n");
        app::StreamCallerTerminal terminal(stream);app::MessageComposePorts compose{db,terminal,[&]{return std::optional<app::MessageActor>{alice};},[&]{return id(ui_operation++);},[ansi]{return app::MessageEditorDimensions{ansi,80,24};},[&]{return terminal.read_editor_byte();},[&](std::string_view text){return stream.write_some(text).outcome==app::InteractiveStreamOutcome::completed;}};
        compose.accepted_key=[&](bool cr){terminal.finish_editor_key(cr);};
        check(app::compose_local_message(compose,remote,"Remote Person","Alice")==app::MessageComposeOutcome::saved,"blank NetMail subject saves through /s and FSE Escape/plain Help Save");
        const auto saved=db.open_message(alice,{scalar(file,"SELECT max(message_id) FROM sfx_local_messages")});check(saved&&saved.value->header.subject.empty()&&saved.value->body.canonical_lf=="Blank-subject NetMail body\n"&&saved.value->header.remote_delivery=="Queued","empty NetMail subject and body retained exactly without claiming delivery");
    }
    check(db.shutdown().succeeded()&&db.start().succeeded(),"typed target and native outbox survive real owner restart");check(db.open_message(alice,outgoing.id).value->header.remote_delivery=="Queued","restart does not fabricate link acknowledgement");check(db.shutdown().succeeded(),"reopened fixture stopped");std::cout<<"PASS "<<checks<<" actual SQLite FTN import/NetMail checks (synthetic ingress proof/custody)\n";return 0;
}catch(const std::exception& ex){std::cerr<<"FAIL "<<checks<<": "<<ex.what()<<'\n';return 1;}}
