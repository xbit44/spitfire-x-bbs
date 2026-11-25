// Test-only representative FTN/ANSI fixture. No private owner packet is read.
#include "sfx/application/menu_session.hpp"
#define main retained_qwk_storage_fixture_main
#include "../integration/qwk_storage_tests.cpp"
#undef main
#include "sfx/application/ftn_work.hpp"
#include "sfx/test/qwk_profile_metrics.hpp"
#include "../../src/apps/runtime/qwk_runtime_authority.hpp"
#include <fstream>
namespace prof=sfx::test::qwk_profile;
namespace arc=sfx::archive;
namespace sfx::archive {
QwkPreparedDownload profiled_prepare_qwk_download(app::IQwkStorage&,const app::MessageActor&,app::Identifier128,app::QwkPacketMetadata,QwkDateFormatter,CancelCheck,QwkPreparationObserver);
}
namespace {
app::FtnDigest digest(std::string_view text){app::FtnDigest d{};check(crypto_hash_sha256(d.data(),reinterpret_cast<const unsigned char*>(text.data()),text.size())==0,"synthetic digest");return d;}
std::string generic_body(unsigned area,unsigned sequence){
    constexpr unsigned lengths[]{180,300,820,690,820,4240,0};std::string body;std::uint32_t random=sequence+17;
    const auto word=[&](){random^=random<<13;random^=random>>17;random^=random<<5;return std::string("word")+std::to_string(random%256);};
    if(area==6){const unsigned rows=sequence==0?420:23;for(unsigned row=0;row<rows;++row){for(unsigned col=0;col<8;++col){auto text=word();text.resize(10,' ');body+="\x1b["+std::to_string(31+(row+col)%6)+"m"+text;}body+="\x1b[0m\r";}}
    else {while(body.size()<lengths[area]){body+=word()+" ";if(body.size()%80>70)body+='\r';}body+='\r';}
    return body;
}
void populate(storage::SqliteStorage& db,const std::filesystem::path& file){
    const app::MessageConfigurationActor owner{true,{}};auto config=*db.ftn_configuration(owner).value;
    app::FtnNetwork network;network.code="PROFILE";network.name="Synthetic Profile";network.address="2:100/1@profile";network.enabled=true;config.networks.push_back(network);
    app::FtnLink link;link.network=network.code;link.code="PEER";link.name="Synthetic peer";link.address="2:100/2@profile";link.host="127.0.0.1";link.enabled=true;link.credentials.session={1};config.links.push_back(link);
    for(unsigned a=0;a<7;++a){app::FtnMessageEcho echo;echo.network=network.code;echo.tag="PROFILE."+std::to_string(a);echo.description="Synthetic area "+std::to_string(a);echo.maximum_messages=10000;echo.pipe_colors=true;echo.ansi_colors=true;config.message_echoes.push_back(echo);config.permissions.push_back({network.code,link.code,echo.tag,false,true,true,false});}
    check(bool(db.change_ftn_configuration(owner,id(100).bytes,config)),"synthetic FTN catalog commits");config=*db.ftn_configuration(owner).value;
    constexpr unsigned counts[]{20,75,493,515,500,100,632};std::vector<app::FtnPackedMessage> messages;messages.reserve(2335);
    for(unsigned area=0;area<7;++area)for(unsigned n=0;n<counts[area];++n){app::FtnPackedMessage m;m.origin_net=m.destination_net=100;m.origin_node=2;m.destination_node=1;m.date="12 Nov 26  12:00:00";m.from="Synthetic Remote";m.to="All";m.subject="Synthetic profile "+std::to_string(messages.size());m.text="AREA:PROFILE."+std::to_string(area)+"\r\1MSGID: 2:100/2 "+std::to_string(messages.size()+1)+"\r\1CHRS: CP437 2\r"+generic_body(area,n);messages.push_back(std::move(m));}
    app::FtnPacketHeader header;header.origin=*app::parse_ftn_address("2:100/2@profile");header.destination=*app::parse_ftn_address("2:100/1@profile");header.year=2026;header.month=11;header.day=12;
    const auto wire=app::encode_ftn_packet({header,messages});check(bool(wire),"generic complete synthetic FTN packet encodes");
    app::FtnIngressReceipt receipt;receipt.id=id(101);receipt.session=id(102);receipt.identity=digest("synthetic profile ingress");receipt.network=network.code;receipt.link=link.code;receipt.proof=app::FtnIngressProof::inbound_cram;receipt.configuration_revision=config.revision;receipt.file={"synthetic-profile.pkt",wire->size(),1794484800};
    check(bool(db.begin_ftn_ingress(owner,receipt))&&db.advance_ftn_ingress(owner,receipt.id,0,wire->size(),digest(*wire),app::FtnIngressState::received,{}).succeeded(),"synthetic host custody receipt");
    for(unsigned n=0;n<messages.size();++n){app::FtnImportRequest r;r.operation=id(10000+n);r.ingress=receipt.id;r.message_index=static_cast<std::uint16_t>(n);r.network=network.code;r.expected_configuration_revision=config.revision;r.header=header;r.message=messages[n];const auto inspected=app::inspect_ftn_message(r.header,r.message);check(bool(inspected),"valid synthetic FTN metadata");const auto material=app::ftn_message_fingerprint_material(r.message,*inspected);r.identity=digest(material.identity);r.content=digest(material.content);const auto imported=db.import_ftn_message(owner,r);if(!imported)std::cerr<<"IMPORT "<<n<<' '<<imported.status.operation<<'\n';check(imported&&imported.value->message,"production FTN import");if(n%250==0)std::cout<<"SEEDED "<<n<<std::endl;}
    const app::MessageActor caller{id(3),3,1,1};const auto saved=db.qwk_preferences(caller);check(bool(saved),"authenticated packet preferences");auto prefs=saved.value->preferences;prefs.word_wrap=false;prefs.full_headers=true;prefs.indexes=true;prefs.preserve_colors=true;prefs.encoding=app::QwkEncoding::cp437;prefs.private_mail=app::QwkPrivateMode::none;
    check(bool(db.save_qwk_preferences(caller,saved.value->revision,prefs)),"explicit representative preferences");check(scalar(file,"SELECT count(*) FROM sfx_local_messages")==2335,"all representative messages retained");
}
}
int main(int argc,char** argv)try{
    check(argc==3||argc==4,"seed/profile explicit isolated root and optional output");const std::string mode=argv[1];const auto root=std::filesystem::absolute(argv[2]);
    check(root.generic_string().find("qwk-packing-profile-20261112-r1")!=std::string::npos,"only this diagnostic fixture family");
    sfx::test::FakeStoragePathPolicy paths;storage::SqliteStorageSettings settings;settings.data_directory=root;settings.utc_now=[]{return std::int64_t{1794484800};};
    const auto file=root/"SFX.DB";storage::SqliteStorage db(settings,paths);
    if(mode=="--seed"){
        check(!std::filesystem::exists(root),"preserve existing baseline");std::filesystem::create_directories(root);check(db.start().succeeded()&&db.shutdown().succeeded(),"production schema created");seed(file);check(db.start().succeeded(),"production fixture opens");populate(db,file);check(db.shutdown().succeeded(),"seed closes cleanly");std::cout<<"PASS seeded2335 synthetic FTN/ANSI messages\n";return 0;
    }
    check((mode=="--profile"||mode=="--probe")&&argc==4&&std::filesystem::is_regular_file(file),"profile existing diagnostic only");const auto output=std::filesystem::absolute(argv[3]);check(!std::filesystem::exists(output),"preserve each measurement");check(db.start().succeeded(),"fixture reader/writer opens");
    if(mode=="--probe"){
        const app::MessageActor actor{id(3),3,1,1};const auto plan=db.begin_qwk_packet(actor,id(40001));check(bool(plan),"probe finite plan");std::ofstream out(output);out<<"{\"scope\":\"production API, read-only synthetic fixture, OS cache warm\",\"rows\":[";bool first=true;
        const auto row=[&](const char* name,std::size_t queue,auto fn){const auto start=std::chrono::steady_clock::now();for(unsigned n=0;n<200;++n)fn();const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();if(!first)out<<',';first=false;out<<"{\"operation\":\""<<name<<"\",\"queue\":"<<queue<<",\"calls\":200,\"wall_us\":"<<us<<'}';};
        row("complete_plan_authority",0,[&]{check(bool(db.validate_qwk_packet_authority(actor,*plan.value)),"probe current full plan authority");});
        for(std::size_t q=0;q<plan.value->queues.size();++q){row("candidate",q,[&]{check(bool(db.next_qwk_candidate(actor,*plan.value,q)),"probe candidate");});row("full_read",q,[&]{check(bool(db.next_qwk_message(actor,*plan.value,q)),"probe full read");});}
        out<<"]}\n";out.close();check(db.shutdown().succeeded(),"probe store closes");std::cout<<"PASS production API residual probe\n";return 0;
    }
    const app::MessageActor actor{id(3),3,1,1};const auto policy=db.qwk_caller_policy(actor);check(bool(policy),"initial current runtime packet authority");const app::QwkTransferRequest request{actor,policy.value->board_id,policy.value->revision};prof::State stats;prof::active=&stats;std::uint64_t callbacks{},returned_canvases{};const auto started=std::chrono::steady_clock::now();
    const auto cancelled=[&]{return prof::measured(prof::authorization,[&]{++callbacks;if(std::chrono::steady_clock::now()-started>std::chrono::minutes(10))return true;return !prof::measured(prof::qwk_policy_read,[&]{return sfx::apps::runtime::qwk_runtime_authorized(db,actor.session,actor.account,request);});});};
    const auto before_pointers=scalar(file,"SELECT count(*) FROM sfx_caller_echo_state"),before_continuation=scalar(file,"SELECT count(*) FROM sfx_qwk_continuation");
    app::QwkPacketMetadata meta;meta.board_name="Synthetic profile";meta.sysop="Synthetic";meta.caller_name="Bob";meta.created="11-12-2026,12:00:00";
    const auto observer=[&](const arc::QwkPreparationEvent& event){prof::Scope scope(prof::observer);if(event.stage==arc::QwkPreparationStage::packing)std::cout<<"PACKING"<<std::endl;return !cancelled();};
    const auto packet=arc::profiled_prepare_qwk_download(db,actor,id(40000),meta,[](std::int64_t){return arc::QwkRecordDate{"11-12-26","12:00","1794484800"};},cancelled,observer);
    check(packet.result.ok()&&packet.progress&&packet.progress->delivered.size()==2335,"complete representative packet prepared");
    prof::measured(prof::final_authority,[&]{const auto okay=db.validate_qwk_packet_authority(actor,packet.progress->initial);check(okay&&okay.value&&*okay.value,"final plan authority remains valid");});
    prof::active=nullptr;
    check(scalar(file,"SELECT count(*) FROM sfx_caller_echo_state")==before_pointers&&scalar(file,"SELECT count(*) FROM sfx_qwk_continuation")==before_continuation,"declined preparation leaves read/continuation pointers unchanged");
    const auto members=download_members(packet.zip);std::size_t expanded{};for(const auto& member:members)expanded+=member.second.size();
    std::ofstream out(output);out<<"{\"result\":\"PASS\",\"scope\":\"production core and faithful Windows storage authorization chain; not TCP caller timing or helper preflight\",\"messages\":2335,\"members\":"<<members.size()<<",\"expanded_bytes\":"<<expanded<<",\"zip_bytes\":"<<packet.zip.size()<<",\"zip_sha256\":\"";
    const auto hash=digest(packet.zip);constexpr char hex[]="0123456789abcdef";for(auto b:hash)out<<hex[b>>4]<<hex[b&15];out<<"\",\"authorization_calls\":"<<callbacks<<",\"returned_canvases_reserved\":"<<returned_canvases<<",\"timings\":";prof::write(out,stats);out<<"}\n";check(bool(out),"complete content-free profile record");out.close();
    std::ofstream zip(output.string()+".qwk",std::ios::binary);zip.write(packet.zip.data(),static_cast<std::streamsize>(packet.zip.size()));check(bool(zip),"synthetic packet retained for equivalence");
    check(db.shutdown().succeeded(),"diagnostic store closes");std::cout<<"PASS profile "<<packet.zip.size()<<" bytes; callbacks "<<callbacks<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"FAIL qwk profile after "<<checks<<": "<<e.what()<<'\n';return 1;}
