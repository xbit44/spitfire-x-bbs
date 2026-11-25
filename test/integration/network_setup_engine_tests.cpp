// Isolated native engine/custody fixtures. No installed apps or live hubs.
#define main preserved_engine_fixture_main
#include "ftn_engine_tests.cpp"
#undef main
#include "sfx/platform/win32/ftn_stored_scan.hpp"
#include <fstream>
namespace {
void bytes(const std::filesystem::path& path,std::string_view value){std::ofstream file(path,std::ios::binary);file.write(value.data(),static_cast<std::streamsize>(value.size()));check(bool(file),"new owned test source written");}
std::string stored(std::string_view origin,std::string_view destination){
    std::string out(190,'\0');out.replace(0,4,"Game");out.replace(36,4,"Game");out.replace(72,10,"attach.bin");
    const auto from=app::parse_ftn_address(origin),to=app::parse_ftn_address(destination);check(from&&to,"stored fixture addresses");const auto word=[&](std::size_t at,unsigned n){out[at]=static_cast<char>(n);out[at+1]=static_cast<char>(n>>8);};word(166,to->node);word(168,from->node);word(172,to->net);word(174,from->net);word(186,0x191);
    return out+"\1INTL "+std::string(destination)+" "+std::string(origin)+"\r\1FLAGS KFS\r"+'\0';
}
std::int64_t scalar(Fixture& f,const char* sql){sfx::adapters::storage::internal::SqliteConnection db(f.root/L"Data"/L"SFX.DB",SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);return db.scalar_int64(sql,"observe isolated native invariant");}
}
int main(int argc,char** argv)try{
    check(argc==2,"fresh isolated native root required");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"retain previous native evidence");std::filesystem::create_directories(root.parent_path());const auto sd=ins::mutable_descriptor(ins::operator_sid());ins::create_directory(root,sd.get());
    Fixture server(root/L"Server",1,2),client(root/L"Client",2,1);server.ready();client.ready();client.config.links[0].port=server.engine->snapshot().classic_port;client.config.links[0].contact_on_mail=true;client.save(2);client.apply();client.ready();
    check(client.config.links[0].poll_minutes==0,"automatic contact does not enable a timer");server.queue(710,"server return payload");client.queue(711,"client queued payload");
    await([&]{return scalar(client,"SELECT count(*) FROM sfx_ftn_outbound WHERE state=3")==1&&scalar(server,"SELECT count(*) FROM sfx_ftn_outbound WHERE state=3")==1;},"queued mail triggers actual authenticated loopback delivery without Poll");
    await([&]{return !active(client.engine->snapshot())&&!active(server.engine->snapshot());},"automatic session completes cleanly");check(!client.db->ftn_outbound_ready({true,{}},"TEST","PEER").value.value_or(true),"delivered work does not keep automatic contact active");
    // Explicit endpoint overrides win even with an unavailable nodelist.
    client.config.networks[0].nodelist_file=(root/L"missing.nodelist").u8string();client.save(3);client.apply();client.ready();client.queue(712,"explicit endpoint still works");check(client.engine->poll("TEST","PEER"),"manual poll retained");await([&]{return scalar(client,"SELECT count(*) FROM sfx_ftn_outbound WHERE state=3")==2;},"configured host and port bypass missing optional list");
    // The same configured peer can use an owner-selected local nodelist.
    const auto list=client.root/L"NODELIST.001";bytes(list,"Zone,2,Z,L,S,-,300\nHost,100,H,L,S,-,300\n,1,Hub,L,S,-,300,INA:127.0.0.1,IBN:"+std::to_string(server.engine->snapshot().classic_port)+"\n");client.config.networks[0].nodelist_file="NODELIST.001";client.config.links[0].host.clear();client.config.links[0].port=0;client.save(4);client.apply();client.ready();client.queue(713,"nodelist peer endpoint");check(client.engine->poll("TEST","PEER"),"nodelist supplies configured peer endpoint only");await([&]{return scalar(client,"SELECT count(*) FROM sfx_ftn_outbound WHERE state=3")==3;},"relative nodelist INA and IBN port drive actual loopback");
    client.engine->stop();server.engine->stop();
    Fixture scan(root/L"Stored",2,1);scan.ready();scan.engine->stop();const auto a=scan.root/L"GameNetMailA",b=scan.root/L"GameNetMailB";ins::create_directory(a,sd.get());ins::create_directory(b,sd.get());bytes(a/L"attach.bin","first game payload");bytes(a/L"1.MSG",stored("2:100/2","2:100/1"));bytes(b/L"attach.bin","second game payload");bytes(b/L"1.MSG",stored("888:888/49","888:888/1"));
    auto other=scan.config.networks[0];other.code="OTHER";other.name="Other";other.address="777:777/49";other.msg_netmail_enabled=true;other.msg_netmail_directory=a.u8string();scan.config.networks[0].msg_netmail_enabled=false;scan.config.networks[0].msg_netmail_directory=a.u8string();scan.config.networks.push_back(other);auto link=scan.config.links[0];link.network="OTHER";link.address="777:777/1";scan.config.links.push_back(link);scan.save(2);
    std::string cursor;const auto off=native::scan_ftn_stored_messages(*scan.db,scan.config,scan.receipt,cursor,[]{return true;});check(!off.queued&&!off.failed&&scalar(scan,"SELECT count(*) FROM sfx_ftn_outbound")==0&&std::filesystem::exists(a/L"1.MSG"),"network Off prevents scan and leaves its request untouched even in shared folder");
    scan.config.networks[0].msg_netmail_enabled=true;scan.config.networks[0].msg_netmail_directory="GameNetMailA";auto second=other;second.code="SECOND";second.name="Second";second.address="888:888/49";second.msg_netmail_directory="GameNetMailB";scan.config.networks.push_back(second);link.network="SECOND";link.address="888:888/1";scan.config.links.push_back(link);scan.save(3);
    // This direct scanner component runs with the engine stopped. Supply the
    // per-network private spool that the production engine prepares on apply.
    ins::create_directory(scan.root/L"FTN"/L"Outbound"/L"SECOND",sd.get());
    for(unsigned i=0;i<8;++i){const auto result=native::scan_ftn_stored_messages(*scan.db,scan.config,scan.receipt,cursor,[]{return true;});if(result.failed)std::cerr<<"Scan "<<i<<": "<<result.reason<<" cursor="<<cursor<<'\n';check(!result.failed,"bounded multi-source scan");}
    check(scalar(scan,"SELECT count(*) FROM sfx_ftn_outbound WHERE network='TEST'")==1&&scalar(scan,"SELECT count(*) FROM sfx_ftn_outbound WHERE network='SECOND'")==1&&scalar(scan,"SELECT count(*) FROM sfx_ftn_outbound WHERE network='OTHER'")==0,"relative/shared sources queue once in the correct network");
    const auto claimed=scan.db->claim_ftn_jobs({true,{}},"TEST","PEER",id(801),id(802),16,false);check(claimed&&claimed.value->size()==1,"synthetic exact same-link delivery claim");check(scan.db->finish_ftn_job({true,{}},claimed.value->front().id,id(802),app::FtnOutboundState::delivered,{}).succeeded(),"synthetic storage ACK for cleanup test only");
    for(unsigned i=0;i<8;++i)native::scan_ftn_stored_messages(*scan.db,scan.config,scan.receipt,cursor,[]{return true;});
    check(!std::filesystem::exists(a/L"1.MSG")&&!std::filesystem::exists(a/L"attach.bin")&&std::filesystem::exists(b/L"1.MSG")&&std::filesystem::exists(b/L"attach.bin"),"ACK-gated KillSent/KFS cleanup touches only the acknowledged source");
    runtime::close_alpha_rlogin_process();std::cout<<"PASS "<<checks<<" queued-mail/nodelist/native-MSG checks; loopback and synthetic ACK scopes only\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}
