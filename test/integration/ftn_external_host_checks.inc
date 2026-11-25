// Finite interactive engineering host, driven by the owned stock-BinkD test.
// Uses real engine, scanner, tosser, SQLite and private custody; synthetic local
// callers/capability receipt are explicit and no normal installation is claimed.
#include "ftn_external_files.hpp"
#include <fstream>
namespace ext=sfx::platform::external_ftn;
namespace {
void external_fixture_write(const std::filesystem::path& file,std::string_view text){
    std::ofstream out(file,std::ios::binary|std::ios::trunc);out.write(text.data(),static_cast<std::streamsize>(text.size()));out.close();check(bool(out),"fixture output flushed");
#ifndef _WIN32
    check(chmod(file.c_str(),0600)==0,"fixture output private");
#endif
}
std::int64_t external_fixture_count(Fixture& board,const char* query){
    sfx::adapters::storage::internal::SqliteConnection db(board.root/"Data/SFX.DB",SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);
    return db.scalar_int64(query,"read controlled engine result");
}
void external_start(Fixture& board){
#ifdef _WIN32
    board.engine=std::make_unique<runtime::AlphaFtnEngine>(board.receipt,*board.db,*board.db,id(60001),board.sounds);
#else
    board.engine=std::make_unique<runtime::LinuxFtnEngine>(board.receipt,*board.db,*board.db,id(60001),fixture_clock,board.sounds);
#endif
    board.engine->configure(board.config);
    await([&]{return board.engine->snapshot().active_revision==board.config.revision;},"external engine revision activates");
    const auto state=board.engine->snapshot();check(state.diagnostic=="ftn_external_processing_active"&&!state.classic_listening&&!state.tls_listening&&active(state)==0,"External mode starts processing without Native transport");
    check(!board.engine->poll("TEST","PEER")&&board.engine->scan(),"External Poll refuses and Scan is available");
}
#include "ftn_external_unknown_checks.inc"
#include "ftn_external_multi_checks.inc"
#include "ftn_external_file_checks.inc"
#include "ftn_external_packet_checks.inc"
#include "ftn_external_areafix_checks.inc"
#include "ftn_external_stored_checks.inc"
}
int main(int argc,char** argv){try{
#ifdef _WIN32
    check(argc==2||argc==3,"fresh external fixture root required");const auto mode=argc==3?std::string(argv[2]):std::string{};
#else
    check(argc==3||argc==4,"fresh external root and current DNS helper required");lookup_helper=std::filesystem::absolute(argv[2]);const auto mode=argc==4?std::string(argv[3]):std::string{};
#endif
    const auto root=std::filesystem::absolute(argv[1]).lexically_normal();check(!std::filesystem::exists(root),"retain all previous external cases");std::filesystem::create_directories(root.parent_path());
    Fixture board(root,1,2,true,true);board.engine->stop();board.engine.reset();
    if(mode=="nonsecure")return external_unknown_fixture(board);
    if(mode=="multi"||mode=="alias-negative")return external_multi_fixture(board,mode=="alias-negative");
    if(mode=="many")return external_multi_fixture(board,false,true);
    if(mode=="packet-p0")return external_packet_fixture(board);
    if(mode=="packet-retention")return external_packet_fixture(board,true);
    if(mode=="areafix")return external_areafix_fixture(board);
    if(mode=="stored")return external_stored_fixture(board);
    if(mode=="files")return external_file_fixture(board);
    check(mode.empty(),"known external fixture mode");
    board.config.settings.mode=app::FtnMailerMode::external_binkd;board.config.settings.tls_listener_enabled=true;board.config.settings.tls_certificate.clear();board.config.settings.tls_private_key.clear();
    auto& link=board.config.links.front();link.credentials={};link.host.clear();link.port=0;link.contact_on_mail=true;link.poll_minutes=1;link.additional_addresses={"2:100/22@test"};
    app::FtnMessageEcho echo;echo.network="TEST";echo.tag="EXTERNAL.TEST";echo.description="External engineering echo";board.config.message_echoes.push_back(echo);
    board.config.permissions.push_back({"TEST","PEER","EXTERNAL.TEST",false,true,true,false});board.save(2);external_start(board);
    // A real caller commit must run through the existing scanner before handoff.
    const app::MessageActor alice{id(2),2,1,1};app::MessageTarget target;target.kind=app::MessageKind::private_ftn;target.remote=app::MessageRemoteTarget{"TEST","2:100/1@test","2:100/2@test","Bob"};
    auto compose=board.db->begin_message(alice,target,{});check(bool(compose),"outgoing private caller compose context");
    check(bool(board.db->commit_message({alice,id(900),*compose.value,"External outgoing NetMail","Local NetMail through real scanner and stock BinkD\n"})),"outgoing private message committed");
    target={};target.kind=app::MessageKind::public_echo;target.echo={board.config.message_echoes.front().local_echo};
    compose=board.db->begin_message(alice,target,{});check(bool(compose),"outgoing echo caller compose context");
    check(bool(board.db->commit_message({alice,id(901),*compose.value,"External outgoing EchoMail","Local EchoMail through real scanner and stock BinkD\n"})),"outgoing echo committed");
    app::FtnPacket packet;packet.header.origin=*app::parse_ftn_address("2:100/2@test");packet.header.destination=*app::parse_ftn_address("2:100/1@test");packet.header.year=2026;packet.header.month=11;packet.header.day=10;
    app::FtnPackedMessage message;message.origin_net=message.destination_net=100;message.origin_node=2;message.destination_node=1;message.date="10 Nov 26  12:00:00";message.from="Remote Fixture";message.to="All";message.subject="External incoming EchoMail";message.text="AREA:EXTERNAL.TEST\r\1MSGID: 2:100/2 external-echo-1\r\1CHRS: CP437 2\rIncoming stock EchoMail body\r";packet.messages.push_back(message);
    message.attributes=1;message.to="Alice";message.subject="External incoming NetMail";message.text="\1INTL 2:100/1 2:100/2\r\1MSGID: 2:100/2 external-private-1\r\1CHRS: CP437 2\rIncoming stock NetMail body\r";packet.messages.push_back(message);
    const auto wire=app::encode_ftn_packet(packet);check(bool(wire),"peer packet has no packet password");external_fixture_write(root/"peer-input.pkt",*wire);
    external_fixture_write(root/"READY","external engine ready\n");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(150);bool phase1{},stopped{},resumed{};
    while(std::chrono::steady_clock::now()<deadline){
        if(!phase1&&external_fixture_count(board,"SELECT count(*) FROM sfx_local_messages WHERE CAST(subject AS TEXT) IN('External incoming EchoMail','External incoming NetMail')")==2){phase1=true;external_fixture_write(root/"PHASE1","two incoming messages imported\n");}
        if(phase1&&!stopped&&std::filesystem::exists(root/"STOP")){
            board.engine->stop();board.engine.reset();stopped=true;check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress")==1,"first stock receive has one exact custody receipt");external_fixture_write(root/"STOPPED","BBS processor stopped; independent mailer may receive\n");
        }
        if(stopped&&!resumed&&std::filesystem::exists(root/"RESUME")){
            check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress")==1,"offline hook did not open or import into BBS store");external_start(board);resumed=true;
        }
        if(resumed&&external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress WHERE state=4")==2)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    check(phase1&&stopped&&resumed,"stock exchange and stopped-receive phases completed");
    check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress WHERE state=4 AND proof=5 AND external_session_authenticated=1")==2,"two distinct same-byte hook records become authenticated external custody");
    check(external_fixture_count(board,"SELECT count(*) FROM sfx_local_messages WHERE CAST(subject AS TEXT) IN('External incoming EchoMail','External incoming NetMail')")==2,"same-byte second receipt replays without duplicate messages");
    check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_outbound WHERE state=5")>=2&&external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_outbound WHERE state=3")==0,"actual scanner outputs handed to BinkD without fake Native delivery");
    const auto state=board.engine->snapshot();check(!state.classic_listening&&!state.tls_listening&&state.sessions.empty(),"no Native listener or dialer after External restart");
    board.engine->stop();check(external_fixture_count(board,"SELECT count(*) FROM pragma_foreign_key_check")==0,"engine fixture references clean");
    check(board.db->shutdown().succeeded(),"external fixture store closed");
    external_fixture_write(root/"DONE","PASS external engine/stock driver phases\n");std::cout<<"PASS external Runtime host "<<checks<<" checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
