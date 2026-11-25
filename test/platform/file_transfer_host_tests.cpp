// Actual separate general-file helper and native host/NTFS leases. Synthetic
// decoded channel, NOT an installed Runtime/Telnet/SSH/UI claim. Encoder is
// independently authored SFX test code, never compiled GPL helper source.
#define wmain historical_message_host_tests_not_run
#include "fixture_paths.hpp"
#include "message_receive_host_tests.cpp"
#undef wmain
#include "alpha_file_transfer.hpp"
#include "alpha_caller_files.hpp"
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/test/storage_fakes.hpp"
#include "sqlite_internal.hpp"
#include "sqlite_statement.hpp"
#include <array>
namespace native=sfx::apps::runtime;
namespace filewire=native::file_transfer_detail;
namespace win=sfx::platform::win32;

struct Gate {std::size_t end{};std::function<bool()> ready;};
struct Peer {
    Channel c;std::vector<Gate> gates;
    native::DecodedFileTransferChannel ports(){
        auto original=c.ports();
        return {original.acquire,[this]{if(c.after_authorize)c.after_authorize();return c.permit;},original.release,
            [this](std::size_t maximum){
                ++c.reads;if(c.after_read)c.after_read();
                if(c.disconnect)return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::disconnected,{}};
                for(const auto& gate:gates)if(c.offset<gate.end){
                    if(c.stall||!gate.ready())break;
                    const auto n=std::min({maximum,c.chunk,gate.end-c.offset});Bytes out(c.wire.begin()+c.offset,c.wire.begin()+c.offset+n);c.offset+=n;
                    return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::completed,std::move(out)};
                }return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::timed_out,{}};
            },original.write,[]{return GetTickCount64();},[this]{return c.limit;}};
    }
    bool header(unsigned type,std::size_t wanted=1)const{
        const char hex[]="0123456789abcdef";std::string prefix="**\x18" "B";prefix+=hex[type>>4];prefix+=hex[type&15];
        std::size_t found{},at{};while((at=c.emitted.find(prefix,at))!=std::string::npos){const auto end=c.emitted.find("\r\n",at);if(end==std::string::npos)return false;++found;at=end+2;}return found>=wanted;
    }
    void gate(std::function<bool()> ready){gates.push_back({c.wire.size(),std::move(ready)});}
    void offer(std::string name,std::string body,std::size_t ordinal){
        frame(c.wire,4);packet(c.wire,name+'\0'+std::to_string(body.size())+'\0','k');gate([this,ordinal]{return header(1,ordinal);});
        frame(c.wire,10);if(body.empty())packet(c.wire,{},'h');else for(std::size_t i=0;i<body.size();i+=1024)packet(c.wire,std::string_view(body).substr(i,1024),i+1024<body.size()?'i':'h');
        frame(c.wire,11,static_cast<std::uint32_t>(body.size()));gate([this,ordinal]{return header(9,ordinal);});
    }
    void finish_receive(std::size_t ready_count,std::string tail={}){
        frame(c.wire,8);gate([this,ready_count]{return header(1,ready_count);});
        c.wire.insert(c.wire.end(),{'O','O'});c.wire.insert(c.wire.end(),tail.begin(),tail.end());gate([this]{return header(8);});
    }
    void refuse_offer(std::string name,std::string size){
        frame(c.wire,4);packet(c.wire,name+'\0'+size+'\0','k');gate([this]{return header(1);});
        frame(c.wire,8);gate([this]{return header(5);});c.wire.insert(c.wire.end(),{'O','O'});gate([this]{return header(8);});
    }
    void receive_one(std::string name,std::uint32_t size,bool skip=false){
        frame(c.wire,1,0x03000000);gate([]{return true;});
        const auto metadata_end=[this,name]{const auto at=c.emitted.find(name);if(at==std::string::npos)return std::string::npos;const auto end=c.emitted.find(std::string("\x18k",2),at+name.size());return end!=std::string::npos&&c.emitted.size()>=end+4?end:std::string::npos;};
        frame(c.wire,skip?5:9);gate([metadata_end]{return metadata_end()!=std::string::npos;});
        if(!skip){
            // A short final block is ZCRCE and needs no ZACK. Exactly one full
            // 1024-byte first block is ZCRCW; acknowledge it only after output.
            if(size>=1024){frame(c.wire,3,size);gate([this,metadata_end]{const auto meta=metadata_end();if(meta==std::string::npos)return false;const auto data=c.emitted.find(std::string("\x18k",2),meta+4);return data!=std::string::npos&&c.emitted.size()>=data+4;});}
            frame(c.wire,1,0x03000000);gate([this]{return header(11);});
        }frame(c.wire,8);gate([this]{return header(8);});
    }
};
native::NativeFileTransferItem item(win::FilePayloadLease& lease,bool receiving,std::string name,std::uint64_t cap){
    return {std::move(name),cap,lease.native_object_identity(),[&lease,receiving]{return lease.duplicate_transfer_handle(receiving);}};
}
void protect_file_fixture(const inst::Path& path,void* sd){
    check(SetFileSecurityW(path.c_str(),DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,sd)!=0,"protect only new synthetic fixture");
    auto handle=inst::open_object(path,true);check(inst::exact_security(handle.get(),sd),"structural protection exact readback");
}
app::Identifier128 file_operation(unsigned n){app::Identifier128 value;value.bytes[0]=29;value.bytes[14]=static_cast<std::uint8_t>(n>>8);value.bytes[15]=static_cast<std::uint8_t>(n);return value;}
void seed_file_actors(const inst::Path& path){
    namespace sql=sfx::adapters::storage::internal;
    sql::SqliteConnection db(path,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
    db.exec("INSERT INTO sfx_accounts(account_id,display_name,name_key,security_level,account_state,revision,purge_allowed,created_utc_seconds,modified_utc_seconds) VALUES(1,'Sysop','sysop',999,'active',1,1,1700000000,1700000000),(2,'Alice','alice',10,'active',1,1,1700000000,1700000000)","synthetic native transfer actors; not authentication");
    db.exec("INSERT INTO sfx_account_statistics(account_id) SELECT account_id FROM sfx_accounts;INSERT INTO sfx_account_profiles(account_id) SELECT account_id FROM sfx_accounts;INSERT INTO sfx_account_name_reservations SELECT name_key,account_id,1,display_name,1700000000,NULL FROM sfx_accounts;UPDATE sfx_security_policy SET published_revision=revision,published_policy_hash=policy_hash;UPDATE sfx_installation_state SET staged_sysop_account_id=1,setup_state='complete',completed_utc_seconds=1700000000","complete synthetic fixture only");
    for(unsigned n=1;n<=2;++n){sql::SqliteStatement q(db.get(),"INSERT INTO sfx_account_sessions VALUES(?1,?2,?2,1700000000,0,1700000000,NULL,NULL,?1)","synthetic live session");q.bind(1,file_operation(n));q.bind(2,static_cast<std::int64_t>(n));q.step();}
}
#ifndef SFX_FILE_HOST_SUPPORT_ONLY
int wmain(int argc,wchar_t** argv)try{
    check(argc==3,"fresh owned fixture and exact helper required");
    const auto root=inst::canonical_root(argv[1]);const auto parent=(sfx::test::fixture_checkout() / L"build");
    check(root.parent_path()==parent&&root.filename().native().rfind(L"section9-file-host-r",0)==0&&!std::filesystem::exists(root),"fresh bounded fixture only");
    auto parent_pin=inst::open_object(parent,true,FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE);
    const auto owner=inst::operator_sid();const auto sd=inst::mutable_descriptor(owner),structural=inst::structural_descriptor(owner);
    inst::create_directory(root,sd.get());inst::create_directory(root/L"Bin",sd.get());inst::create_directory(root/L"Bin"/L"Tools",sd.get());inst::create_directory(root/L"Work",sd.get());inst::create_directory(root/L"Payload",sd.get());
    for(const auto& path:{root/L"Data",root/L"Data"/L"FileIntake",root/L"Files",root/L"Files"/L"General",root/L"Files"/L"Uploads",root/L"Files"/L"UploadToSysop"})inst::create_directory(path,sd.get());
    const auto helper=inst::read_file(argv[2],4*1024*1024);check(inst::sha256(helper)=="81257FC9C96AAAE5BC03FA6EEC7DFAD2BF82A5E9938F0DB52B6ADC2417807C3B","exact separate general helper");
    inst::create_file(root/L"Bin"/L"Tools"/L"SFXFileTransfer.exe",helper,structural.get());inst::create_file(root/L"Bin"/L"sfxexec.ini",{1,2,3},sd.get());protect_file_fixture(root/L"Bin"/L"Tools",structural.get());protect_file_fixture(root/L"Bin",structural.get());protect_file_fixture(root,structural.get());
    WSADATA ws{};check(WSAStartup(MAKEWORD(2,2),&ws)==0,"Winsock initialized");struct End{~End(){WSACleanup();}} end;
    const auto empty_work=[&]{return std::filesystem::directory_iterator(root/L"Work")==std::filesystem::directory_iterator{};};
    auto directory=win::FilePayloadDirectory::private_store(root/L"Payload",owner);unsigned next{};
    const auto fresh=[&](std::uint64_t cap=64ULL){return directory.create("native-"+std::to_string(++next)+".bin",cap);};
    const auto run_host=[&](Peer& peer,filewire::Direction direction,const std::vector<native::NativeFileTransferItem>& files,app::TransferProtocolPreference protocol=app::TransferProtocolPreference::zmodem){
        peer.c.limit=GetTickCount64()+12000;
        native::AlphaFileTransfer host(root,owner,peer.ports(),[](std::string_view phase,std::uint32_t error,std::uint32_t exit,std::size_t pending){std::cout<<"OBS "<<phase<<" error="<<error<<" exit="<<exit<<" pending="<<pending<<std::endl;});
        check(host.available(),"protected host/helper preflight");++cases;const auto result=host.run(direction,files,protocol);
        std::cout<<"RESULT "<<cases<<" code="<<static_cast<int>(result.code)<<" sync="<<peer.c.synchronized<<" input="<<peer.c.offset<<" output="<<peer.c.emitted.size()<<std::endl;
        if(result.code==native::NativeFileTransferCode::disconnected){const char hex[]="0123456789ABCDEF";std::cout<<"SYNTHETIC WIRE ";for(unsigned char b:peer.c.emitted)std::cout<<hex[b>>4]<<hex[b&15];std::cout<<std::endl;}
        return result;
    };
    #include "section10_xy_host_checks.inc"
    for(const auto chunk:{std::size_t(1),std::size_t(4096)})for(const auto& tail:{std::string{},std::string("\r"),std::string("\n"),std::string("\r\n")}){
        auto lease=fresh();Peer peer;peer.c.chunk=chunk;peer.offer("binary.exe",std::string("A\0B\xff",4),1);peer.finish_receive(2,tail);
        const auto result=run_host(peer,filewire::Direction::receive,{item(lease,true,"binary.exe",64)});
        check(result.code==native::NativeFileTransferCode::completed&&peer.c.synchronized&&result.files.size()==1&&result.files[0].completed&&result.files[0].bytes==4,"actual receive proven before application acceptance");
        const auto id=lease.identity();check(id.bytes==4&&lease.read(0)==Bytes({'A',0,'B',255}),"same native object actual binary bytes exact");check(empty_work(),"only job control files cleaned; payload remains");
    }
    {auto first=fresh(),second=fresh(),third=fresh();Peer peer;peer.offer("empty.dat",{},1);peer.offer("one.txt","one",2);peer.offer("caf\x82.zip","three",3);peer.finish_receive(4);
        const auto result=run_host(peer,filewire::Direction::receive,{item(first,true,"empty.dat",64),item(second,true,"one.txt",64),item(third,true,"caf\x82.zip",64)});
        check(result.code==native::NativeFileTransferCode::completed&&peer.c.synchronized&&result.files.size()==3,"actual multi-file/zero/high-byte receive");
        check(first.identity().bytes==0&&second.identity().bytes==3&&third.identity().bytes==5,"all three sizes observed after child reaped");check(empty_work(),"batch controls cleaned");}
    for(int mode=0;mode<3;++mode){auto lease=fresh();Peer peer;if(mode==0)peer.refuse_offer("large.zip","65");else if(mode==1)peer.refuse_offer("unknown.zip","4");else peer.finish_receive(1,"\r");
        const auto result=run_host(peer,filewire::Direction::receive,{item(lease,true,mode==0?"large.zip":"owned.zip",64)});
        check(result.code==native::NativeFileTransferCode::completed&&peer.c.synchronized&&result.files.size()==1&&!result.files[0].completed&&result.refused==(mode==2?0U:1U),"oversize/unknown/empty completed batch preserves session without acceptance");check(lease.identity().bytes==0&&empty_work(),"refused capability never written");}
    for(const std::uint32_t size:{0U,8U,1024U})for(bool skip:{false,true}){auto lease=fresh(2048);const std::string source(size,'D');if(size)lease.append(reinterpret_cast<const std::uint8_t*>(source.data()),source.size());const auto before=lease.identity();Peer peer;peer.receive_one("out.zip",size,skip);
        const auto result=run_host(peer,filewire::Direction::send,{item(lease,false,"out.zip",2048)});
        check(result.code==native::NativeFileTransferCode::completed&&peer.c.synchronized&&result.files.size()==1&&result.files[0].completed==!skip&&result.files[0].skipped==skip,"actual send or explicit skip has correct receipt");check(lease.identity().sha256==before.sha256&&empty_work(),"sender payload unchanged and controls cleaned");}
    {auto lease=fresh();Peer peer;peer.c.wire.assign(8,24);peer.gate([&]{return peer.header(1);});
        const auto result=run_host(peer,filewire::Direction::receive,{item(lease,true,"cancel.zip",64)});
        check(result.code==native::NativeFileTransferCode::failed&&peer.c.synchronized&&result.files.empty(),"actual helper cancel recovers text boundary without claiming a transfer");
        check(lease.identity().bytes==0&&empty_work(),"cancelled receive has no accepted payload and control scratch cleaned");}
    for(int mode=0;mode<5;++mode){auto lease=fresh();Peer peer;auto entry=item(lease,true,"safe.zip",64);auto entries=std::vector<native::NativeFileTransferItem>{entry};
        if(mode==0)entries[0].wire_name="CON.txt";
        if(mode==1)entries[0].object.file_id="0";
        if(mode==2){entries.push_back(entry);entries[1].wire_name="other.zip";}
        if(mode==3){entries.push_back(entry);entries[1].wire_name="SAFE.ZIP";}
        if(mode==4)entries[0].maximum=0;
        const auto result=run_host(peer,filewire::Direction::receive,entries);
        check(result.code==native::NativeFileTransferCode::unavailable&&!peer.c.acquired&&lease.identity().bytes==0&&empty_work(),"invalid name/object/duplicate/cap fails before binary lease");}
    for(int mode=0;mode<4;++mode){auto lease=fresh();Peer peer;peer.offer("safe.zip","source",1);peer.finish_receive(2);
        if(mode==0)peer.c.after_acquire=[&]{peer.c.permit=false;};
        if(mode==1)peer.c.after_read=[&]{peer.c.limit=GetTickCount64()-1;};
        if(mode==2)peer.c.disconnect=true;
        if(mode==3)peer.c.after_read=[&]{if(peer.header(8))peer.c.permit=false;};
        const auto result=run_host(peer,filewire::Direction::receive,{item(lease,true,"safe.zip",64)});
        check(result.code==native::NativeFileTransferCode::disconnected&&!peer.c.synchronized&&result.files.empty()&&empty_work(),"revoked/expired/disconnected channel cannot report acceptance");}
    {
        namespace storage=sfx::adapters::storage;sfx::test::FakeStoragePathPolicy policy;storage::SqliteStorageSettings settings;settings.data_directory=root/L"Data";settings.utc_now=[]{return std::int64_t{1792248000};};
        storage::SqliteStorage db(settings,policy);check(db.start().succeeded()&&db.shutdown().succeeded(),"fresh real SQLite schema");seed_file_actors(root/L"Data"/L"SFX.DB");check(db.start().succeeded(),"synthetic actors available");
        const app::FileActor caller{file_operation(2),2,1,1},sysop{file_operation(1),1,1,1};const app::FileConfigurationActor config_actor{true,{}};unsigned operation=100;
        const auto service_ports=[&](Peer& peer,app::FileActor actor){peer.c.limit=GetTickCount64()+12000;return native::CallerFileTransferPorts{db,root,owner,[actor]{return std::optional<app::FileActor>{actor};},[&]{return file_operation(operation++);},[]{return std::string("2026-10-17");},peer.ports(),{},[](const auto& row,const auto&,auto){return std::optional<native::CallerUploadReview>{{row.descriptions,row.sysop_only}};}};};
        const auto upload=[&](std::string name,std::string body,bool private_submission){Peer peer;peer.offer(name,body,1);peer.finish_receive(2);native::AlphaCallerFileTransfers service(service_ports(peer,caller));++cases;
            const std::vector<app::FileUploadItem> queue{{name,{2},{"Owned description","Long description"},false}};const auto result=service.upload(queue,private_submission);check(queue.front().destination.value==2&&queue.front().name==name,"native caller transfer never retargets/mutates normal queue");return result;};
        const auto accepted=upload("accepted.txt","uploaded-body",false);
        check(accepted.connected&&accepted.files.size()==1&&accepted.files[0].completed&&accepted.files[0].bytes==13,"actual helper-to-intake-to-catalog upload accepted");
        const auto file=accepted.files[0].file;
        check(db.file_information(caller,file).succeeded()&&!db.reserve_file_download(caller,file_operation(operation++),file,"2026-10-17"),"normal Uploads listed but ordinary download denied");
        check(db.file_caller_statistics(caller,"2026-10-17").value->counts.successful_uploads==1,"exactly one upload credited");
        const auto private_result=upload("private.txt","private",true);check(private_result.files[0].completed&&!db.file_information(caller,private_result.files[0].file)&&db.file_information(sysop,private_result.files[0].file).succeeded(),"actual separate private upload hidden from ordinary caller");
        const auto counts=db.file_caller_statistics(caller,"2026-10-17");check(counts.value->counts.successful_uploads==2&&counts.value->counts.eligible_uploads==1,"private statistics without default ratio credit");
        {
            const auto scan_policy=[&](bool enabled){const auto current=db.file_configuration(config_actor);app::FileConfigurationChange change;change.operation=file_operation(operation++);change.expected_catalog_revision=current.value->policy.catalog_revision;change.policy=current.value->policy;change.policy.scan_uploads=enabled;check(db.change_file_configuration(config_actor,change).succeeded(),"explicit synthetic scan policy for finalized notice regression");};
            scan_policy(true);
            Peer peer;peer.offer("finding.txt","harmless synthetic finding",1);peer.finish_receive(2);auto ports=service_ports(peer,caller);unsigned notices{};
            ports.scan=[](auto&,auto){return app::FileScanResult{app::FileScanVerdict::detected,{"Synthetic.Not.Malware"},"Injected result only"};};
            ports.scan_notice=[&](const auto& row,const auto& outcome){++notices;check(row.name=="finding.txt"&&outcome.scan.verdict==app::FileScanVerdict::detected&&outcome.deletion_confirmed&&outcome.journal_confirmed&&outcome.state==app::FileIntakeState::deleted,"notice callback follows actual exact staging deletion and journal, not scanner exit");};
            native::AlphaCallerFileTransfers service(std::move(ports));++cases;const auto result=service.upload({{"finding.txt",{2},{"Synthetic",""},false}},false);
            check(result.connected&&notices==1&&result.files[0].deleted&&!result.files[0].completed&&db.file_caller_statistics(caller,"2026-10-17").value->counts.successful_uploads==2,"one synthetic detection notice, no success credit or policy change");scan_policy(false);
        }
        {std::string refusal;const auto before=db.pending_file_intake(config_actor,256).value->size();
            for(const auto name:{"private.txt","absent.txt"}){Peer peer;native::AlphaCallerFileTransfers service(service_ports(peer,caller));++cases;const auto result=service.upload({{name,{3},{"Unauthorized",""},false}},false);
                check(!peer.c.acquired&&result.files.size()==1&&!result.files[0].completed,"ordinary route cannot start private area transfer");
                if(refusal.empty())refusal=result.files[0].detail;else check(refusal==result.files[0].detail,"denied private filename presence is indistinguishable");}
            check(db.pending_file_intake(config_actor,256).value->size()==before,"denied private target creates no intake or staged payload");}
        for(bool skip:{false,true}){Peer peer;peer.receive_one("accepted.txt",13,skip);native::AlphaCallerFileTransfers service(service_ports(peer,sysop));++cases;const auto result=service.download({file});
            check(result.connected&&result.files.size()==1&&result.files[0].completed==!skip&&result.files[0].skipped==skip,"actual native catalog-authorized download or skip");
            check(db.file_caller_statistics(sysop,"2026-10-17").value->counts.successful_downloads==1&&db.file_information(sysop,file).value->downloads==1,"success-only once counters; skip changes none");}
        const auto bad_zip=upload("bad.zip","not an archive",false);check(!bad_zip.files[0].completed&&bad_zip.files[0].held&&!bad_zip.files[0].deleted,"complete helper cannot accept unsafe ZIP or delete it");
        {Peer peer;peer.offer("declined.txt","declined",1);peer.finish_receive(2);auto ports=service_ports(peer,caller);ports.review=[](const auto&,const auto&,auto)->std::optional<native::CallerUploadReview>{return {};};native::AlphaCallerFileTransfers service(std::move(ports));++cases;
            const auto result=service.upload({{"declined.txt",{2},{"Declined",""},false}},false);check(result.files[0].held&&!result.files[0].completed,"caller declines metadata; actual file privately held");}
        {auto current=db.file_configuration(config_actor);app::FileConfigurationChange change;change.operation=file_operation(operation++);change.expected_catalog_revision=current.value->policy.catalog_revision;change.policy=current.value->policy;change.policy.maximum_upload_bytes=8;check(db.change_file_configuration(config_actor,change).succeeded(),"bounded caller limit for actual helper refusal");
            Peer peer;peer.refuse_offer("large.txt","9");native::AlphaCallerFileTransfers service(service_ports(peer,caller));++cases;const auto result=service.upload({{"large.txt",{2},{"Too large",""},false}},false);
            check(result.connected&&!result.files[0].completed&&result.files[0].held&&!result.files[0].deleted,"oversize refused without disconnect/catalog/counter or deletion");}
        {Peer peer;peer.offer("changed.txt","change",1);peer.finish_receive(2);auto ports=service_ports(peer,caller);
            ports.review=[&](const auto& row,const auto&,auto)->std::optional<native::CallerUploadReview>{auto current=db.file_configuration(config_actor);app::FileConfigurationChange change;change.operation=file_operation(operation++);change.expected_catalog_revision=current.value->policy.catalog_revision;change.policy=current.value->policy;change.policy.maximum_upload_bytes=9;check(db.change_file_configuration(config_actor,change).succeeded(),"configuration changes during metadata review");return native::CallerUploadReview{row.descriptions,false};};
            native::AlphaCallerFileTransfers service(std::move(ports));++cases;const auto result=service.upload({{"changed.txt",{2},{"Changed authority",""},false}},false);
            check(result.files[0].held&&!result.files[0].completed,"new configuration invalidates prepared acceptance");}
        {Peer peer;peer.c.disconnect=true;native::AlphaCallerFileTransfers service(service_ports(peer,sysop));++cases;const auto result=service.download({file});
            check(!result.connected&&result.files.size()==1&&!result.files[0].completed&&db.file_caller_statistics(sysop,"2026-10-17").value->counts.successful_downloads==1,"disconnected real sender never credits a download");}
        check(db.file_caller_statistics(caller,"2026-10-17").value->counts.successful_uploads==2,"only accepted ordinary/private uploads counted");
        check(empty_work()&&db.shutdown().succeeded(),"all transfer control scratch gone; real DB stopped normally");
    }
    std::cout<<"PASS "<<checks<<" checks/"<<cases<<" native host-helper/catalog cases; synthetic actors/channel, NOT installed Runtime/Telnet/SSH/UI/authentication acceptance\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL file host after "<<checks<<" checks/"<<cases<<" cases: "<<e.what()<<'\n';return 1;}
#endif
