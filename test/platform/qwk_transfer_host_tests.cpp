// Production packet host + exact packet helper; synthetic decoded channel.
// Reuse the independently authored SFX CRC16 fixture encoder, not GPL sources.
#define wmain historical_message_host_tests_not_run
#include "fixture_paths.hpp"
#include "message_receive_host_tests.cpp"
#undef wmain
#include "alpha_qwk_transfer.hpp"
#include "sfx/archive/qwk_archive.hpp"
#include <archive.h>
#include <archive_entry.h>
app::QwkTransferRequest packet_request(){return {{},"SFXTEST",1};}
sfx::apps::runtime::MessageReceiveObserver observe(){return [](std::string_view phase,std::uint32_t error,std::uint32_t exit,std::size_t pending){std::cout<<"OBS "<<phase<<" error="<<error<<" exit="<<exit<<" pending="<<pending<<std::endl;};}
app::DecodedQwkTransferChannel packet_ports(Channel& c){
    auto p=c.ports();return {p.acquire,p.release,p.read,p.write,p.now,p.absolute_deadline,[&](const auto&){if(c.after_authorize)c.after_authorize();return c.permit;}};
}
void protect(const inst::Path& path,void* descriptor){
    check(SetFileSecurityW(path.c_str(),DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,descriptor)!=0,"protect ONLY new owned fixture structural object");auto h=inst::open_object(path,true);check(inst::exact_security(h.get(),descriptor),"exact structural fixture security readback");
}
std::string rep_zip(){
    std::string bytes(384,' ');bytes.replace(0,7,"SFXTEST");const auto field=[&](std::size_t at,std::size_t n,std::string_view v){bytes.replace(128+at,std::min(n,v.size()),v.substr(0,n));};
    field(1,7,"2");field(8,8,"10-13-26");field(16,5,"14:00");field(21,25,"All Callers");field(46,25,"Bob");field(71,25,"Host test");field(116,6,"2");bytes[128+122]=static_cast<char>(0xe1);bytes[128+123]=2;bytes[128+124]=0;field(128,128,"Whole host reply");
    // The shipping writer intentionally makes DOWNLOAD packets only. This
    // independent test writer creates the one permitted REP member instead.
    struct Free{void operator()(struct archive* a)const{archive_write_free(a);}};
    std::unique_ptr<struct archive,Free> writer(archive_write_new());std::string zip;
    const auto append=+[](struct archive*,void* context,const void* data,std::size_t n)->la_ssize_t{static_cast<std::string*>(context)->append(static_cast<const char*>(data),n);return static_cast<la_ssize_t>(n);};
    check(archive_write_set_format_zip(writer.get())==ARCHIVE_OK&&archive_write_set_bytes_per_block(writer.get(),0)==ARCHIVE_OK&&archive_write_open(writer.get(),&zip,nullptr,append,nullptr)==ARCHIVE_OK,"independent bounded REP ZIP writer");
    auto* entry=archive_entry_new();archive_entry_set_pathname(entry,"SFXTEST.MSG");archive_entry_set_filetype(entry,AE_IFREG);archive_entry_set_size(entry,bytes.size());
    check(archive_write_header(writer.get(),entry)==ARCHIVE_OK&&archive_write_data(writer.get(),bytes.data(),bytes.size())==static_cast<la_ssize_t>(bytes.size()),"actual bounded REP ZIP entry");archive_entry_free(entry);
    check(archive_write_close(writer.get())==ARCHIVE_OK&&!zip.empty(),"complete REP ZIP envelope");
    const auto envelope=sfx::archive::read_qwk_reply_zip(zip,"SFXTEST");check(envelope.result.ok()&&envelope.files.size()==1,"REP fixture envelope validates independently");const auto decoded=app::decode_qwk_reply("SFXTEST",{{envelope.files[0].name,envelope.files[0].bytes}});if(!decoded)std::cerr<<"Codec fixture error: "<<decoded.detail<<'\n';check(static_cast<bool>(decoded),"REP fixture codec validates independently");return zip;
}
void upload(Channel& c,std::string_view body,std::string name="TEST.REP",std::string tail={}){
    frame(c.wire,4);packet(c.wire,name+'\0'+std::to_string(body.size())+'\0','k');frame(c.wire,10);
    for(std::size_t n=0;n<body.size();n+=1024)packet(c.wire,body.substr(n,1024),n+1024<body.size()?'i':'h');
    frame(c.wire,11,static_cast<std::uint32_t>(body.size()));frame(c.wire,8);c.policy_finish_end=c.wire.size();c.wire.insert(c.wire.end(),{'O','O'});c.wire.insert(c.wire.end(),tail.begin(),tail.end());
}
#ifndef SFX_QWK_HOST_SUPPORT_ONLY
int wmain(int argc,wchar_t** argv)try{
    check(argc==4,"new build fixture parent and exact packet/message helpers required");const inst::Path parent=std::filesystem::absolute(argv[1]);
    const auto base=(sfx::test::fixture_checkout() / L"build").native();const auto prefix=parent.native();
    check(prefix.rfind(base+L"\\section8d-stage14-",0)==0,"contained fresh current-helper host test build only");std::filesystem::create_directories(parent);
    const auto root=parent/inst::unique_id();const auto owner=inst::operator_sid();const auto mutable_security=inst::mutable_descriptor(owner),structural=inst::structural_descriptor(owner);
    inst::create_directory(root,mutable_security.get());inst::create_directory(root/L"Bin",mutable_security.get());inst::create_directory(root/L"Bin"/L"Tools",mutable_security.get());inst::create_directory(root/L"Work",mutable_security.get());
    auto helper=inst::read_file(argv[2],4U*1024U*1024U);check(inst::sha256(helper)=="981A34ADBBEE712E04FE39E4A087C938763D78A25C288749A64FAB05E8D049EB","exact current packet helper, no old-helper substitution");inst::create_file(root/L"Bin"/L"Tools"/L"SFXQWKTransfer.exe",helper,structural.get());
    inst::create_file(root/L"Bin"/L"sfxexec.ini",{1,2,3},mutable_security.get());
    inst::create_file(root/L"Bin"/L"sexyz.ini",{1,2,3},mutable_security.get());
    const auto message_helper=inst::read_file(argv[3],4U*1024U*1024U);check(inst::sha256(message_helper)=="E884051DC99FA4D56DF3770CD2680EC40A22C32048440354CCE8D4E9E0D36FE8","exact shipped message-file helper");inst::create_file(root/L"Bin"/L"Tools"/L"sexyz.exe",message_helper,structural.get());
    protect(root/L"Bin"/L"Tools",structural.get());protect(root/L"Bin",structural.get());protect(root,structural.get());
    WSADATA ws{};check(WSAStartup(MAKEWORD(2,2),&ws)==0,"native Winsock");struct End{~End(){WSACleanup();}} end;
    {Channel c;sfx::apps::runtime::AlphaQwkPacketTransfer host(root,owner,packet_ports(c),observe());check(!host.available(packet_request()),"unrelated helper INI remains refused even alongside legitimate Door INI");check(DeleteFileW((root/L"Bin"/L"sexyz.ini").c_str())!=FALSE,"remove only this new negative-test file");}
    const auto empty_work=[&]{return std::filesystem::directory_iterator(root/L"Work")==std::filesystem::directory_iterator{};};const auto zip=rep_zip();
    {Channel c;c.wire=transfer("Uploaded message text\r\n");sfx::apps::runtime::AlphaMessageSourceReceiver receiver(root,owner,c.ports(),observe());check(receiver.available(request()),"message-file upload allows legitimate Door INI");const auto received=receiver.receive(request());check(received.code==app::MessageReceiveCode::completed&&received.source,"actual message-file upload completes with Door INI present");}
    for(const auto chunk:{std::size_t(1),std::size_t(4096)})for(const auto& tail:{std::string{},std::string("\r"),std::string("\n"),std::string("\r\n")}){
        Channel c;c.chunk=chunk;c.tail_coalesced=true;upload(c,zip,"TEST.REP",tail);{
            sfx::apps::runtime::AlphaQwkPacketTransfer host(root,owner,packet_ports(c),observe());check(host.available(packet_request()),"packet host exact helper/security preflight");++cases;
            auto result=host.receive(packet_request());std::cout<<"RESULT code="<<static_cast<unsigned>(result.code)<<" refusal="<<static_cast<unsigned>(result.refusal)<<" sync="<<c.synchronized<<" reads="<<c.reads<<" offset="<<c.offset<<" emitted="<<c.emitted.size()<<std::endl;check(result.code==app::QwkTransferCode::completed&&result.source&&c.synchronized,"actual completed bounded relay/envelope/codec/sealed source");
            check(result.source->verify_identity().succeeded()&&result.source->rewind().succeeded(),"host-created pinned read-only source");const auto record=result.source->next();check(record&&record.value->has_value()&&record.value->value().message.subject=="Host test","actual uploaded record exact");
            check(!empty_work(),"private job lifetime remains pinned until source release");
        }check(empty_work(),"last host source closes handles then exact nonrecursive job cleanup");
    }
    for(const auto reason:{1,2,3,4}){
        Channel c;c.tail_coalesced=true;if(reason==1)policy_offer(c,"wrong.exe","4","\r");else if(reason==2)policy_offer(c,"large.REP","16777217","\r\n");else if(reason==3)empty_offer(c,"\n");else upload(c,"not a zip","TEST.ZIP","\r");
        sfx::apps::runtime::AlphaQwkPacketTransfer host(root,owner,packet_ports(c),observe());++cases;const auto result=host.receive(packet_request());
        check(result.code==(reason==3?app::QwkTransferCode::cancelled:app::QwkTransferCode::refused)&&!result.source&&c.synchronized,"complete extension/size/empty/content refusal preserves decoded-channel synchronization");
        check(result.refusal==(reason==1?app::QwkTransferRefusal::extension:reason==2?app::QwkTransferRefusal::too_large:reason==4?app::QwkTransferRefusal::content:app::QwkTransferRefusal::none)&&empty_work(),"exact refusal reason/no business source/temporary cleanup");
    }
    for(const auto chunk:{std::size_t(1),std::size_t(4096)}){
        Channel c;c.chunk=chunk;std::array<std::size_t,5> ends{};frame(c.wire,1,0x03000000);ends[0]=c.wire.size();frame(c.wire,9);ends[1]=c.wire.size();frame(c.wire,3,static_cast<std::uint32_t>(zip.size()));ends[2]=c.wire.size();frame(c.wire,1,0x03000000);ends[3]=c.wire.size();frame(c.wire,8);ends[4]=c.wire.size();
        auto ports=packet_ports(c);ports.read=[&](std::size_t maximum){
            ++c.reads;std::size_t end=ends[0];const auto name=c.emitted.find("SFXTEST.QWK");
            const auto meta=name==std::string::npos?std::string::npos:c.emitted.find(std::string("\x18k",2),name+11);
            const auto data=meta==std::string::npos?std::string::npos:c.emitted.find(std::string("\x18k",2),meta+2);
            if(meta!=std::string::npos&&c.emitted.size()>=meta+4)end=ends[1];
            // The standard sender's FIRST data frame uses ZCRCW, even for a
            // short file. Earn its actual-offset ZACK, then EOF-ready and FIN
            // acknowledgement, instead of pre-feeding future responses.
            if(data!=std::string::npos&&c.emitted.size()>=data+4)end=ends[2];
            if(c.saw_header("**\x18" "B0b"))end=ends[3];if(c.saw_header("**\x18" "B08"))end=ends[4];
            if(c.offset>=end)return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::timed_out,{}};
            const auto n=std::min({maximum,c.chunk,end-c.offset});Bytes out(c.wire.begin()+c.offset,c.wire.begin()+c.offset+n);c.offset+=n;return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::completed,std::move(out)};
        };
        sfx::apps::runtime::AlphaQwkPacketTransfer host(root,owner,std::move(ports),observe());++cases;const auto result=host.send(packet_request(),zip);
        check(result.code==app::QwkTransferCode::completed&&c.synchronized&&empty_work(),"actual sender host exact receipts/partial writes/FIN-OO proof/owned cleanup");
        check(c.emitted.find("SFXTEST.QWK")!=std::string::npos,"safe board-id name metadata only");
    }
    for(int mode=0;mode<3;++mode){Channel c;upload(c,zip);if(mode==0)c.permit=false;if(mode==1)c.after_acquire=[&]{c.permit=false;};if(mode==2)c.after_read=[&]{c.limit=c.clock-1;};
        sfx::apps::runtime::AlphaQwkPacketTransfer host(root,owner,packet_ports(c),observe());++cases;const auto result=host.receive(packet_request());check(result.code==(mode?app::QwkTransferCode::disconnected:app::QwkTransferCode::unavailable)&&!result.source&&!c.synchronized&&empty_work(),"authority/shortened ceiling cannot yield source or false completion");
    }
    std::cout<<"FIXTURE "<<root.string()<<"\nPASS "<<checks<<" native packet-host checks/"<<cases<<" cases; synthetic decoded channel, NOT Setup/installed/Telnet/SSH/reader acceptance\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL packet host after "<<checks<<" checks/"<<cases<<" cases: "<<e.what()<<'\n';return 1;}
#endif
