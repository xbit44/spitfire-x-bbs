// Actual separate general-file helper, NTFS and SQLite. Decoded caller and
// account session authority are synthetic; installed/physical testing separate.
#define SFX_FILE_HOST_SUPPORT_ONLY
#include "fixture_paths.hpp"
#include "file_transfer_host_tests.cpp"
#include "sfx/test/sqlite_write_fault_vfs.hpp"
#include <map>
namespace fs=std::filesystem;
using WorkSnapshot=std::map<fs::path,std::vector<std::uint8_t>>;
WorkSnapshot work_files(const fs::path& work){WorkSnapshot out;for(const auto& entry:fs::recursive_directory_iterator(work))if(entry.is_regular_file())out.emplace(entry.path().lexically_relative(work),inst::read_file(entry.path(),4U*1024U*1024U));return out;}
bool keeps(const WorkSnapshot& before,const WorkSnapshot& after){for(const auto& item:before){const auto found=after.find(item.first);if(found==after.end()||found->second!=item.second)return false;}return true;}
int wmain(int argc,wchar_t** argv)try{
    check(argc==3,"new fixture and exact general-file helper required");const auto root=inst::canonical_root(argv[1]);
    check(root.parent_path()==(sfx::test::fixture_checkout() / L"build")&&root.filename().native().rfind(L"section16-file-cleanup-r",0)==0&&!fs::exists(root),"fresh contained file cleanup fixture only");
    const auto owner=inst::operator_sid();const auto content=inst::mutable_descriptor(owner),structure=inst::structural_descriptor(owner);
    for(const auto* leaf:{L"",L"Bin",L"Bin/Tools",L"Work",L"Payload",L"Data",L"Data/FileIntake",L"Data/FileRemoved",L"Files",L"Files/General",L"Files/Uploads",L"Files/UploadToSysop"}){auto directory=*leaf?(root/leaf).make_preferred():root;inst::create_directory(directory,content.get());}
    const auto helper=inst::read_file(argv[2],4U*1024U*1024U);check(inst::sha256(helper)=="81257FC9C96AAAE5BC03FA6EEC7DFAD2BF82A5E9938F0DB52B6ADC2417807C3B","exact qualified general-file helper");inst::create_file(root/L"Bin"/L"Tools"/L"SFXFileTransfer.exe",helper,structure.get());
    inst::create_file(root/L"Work"/L"foreign.tmp",{'K','E','E','P'},content.get());
    protect_file_fixture(root/L"Bin"/L"Tools",structure.get());protect_file_fixture(root/L"Bin",structure.get());protect_file_fixture(root,structure.get());
    WSADATA ws{};check(WSAStartup(MAKEWORD(2,2),&ws)==0,"native Winsock");struct End{~End(){WSACleanup();}} end;
    auto notices=std::make_shared<std::vector<std::pair<std::string,std::string>>>();
    const native::WorkProblemObserver notice=[notices](std::string resource,std::string code){notices->emplace_back(std::move(resource),std::move(code));};
    auto payload=win::FilePayloadDirectory::private_store(root/L"Payload",owner);unsigned number{};
    const auto fresh=[&]{return payload.create("owned-"+std::to_string(++number)+".bin",64);};
    auto retained=work_files(root/L"Work");
    for(const bool confirmed:{true,false}){
        auto lease=fresh();Peer peer;peer.offer("owned.bin","payload",1);peer.finish_receive(2,"\r\n");const auto prior_notices=notices->size();
        {
            native::AlphaFileTransfer host(root,owner,peer.ports(),{},notice);auto result=host.run(filewire::Direction::receive,{item(lease,true,"owned.bin",64)});
            check(result.code==native::NativeFileTransferCode::completed&&result.files.front().completed&&peer.c.synchronized,"actual wire completion and native destination proof");
            check(lease.identity().bytes==7&&fs::directory_iterator(root/L"Work")!=fs::directory_iterator{},"payload retained and control work stays live before host disposition");
            if(confirmed){check(bool(result.confirm_completed),"host completion capability returned");result.confirm_completed();}
        }
        const auto after=work_files(root/L"Work");
        if(confirmed)check(after==retained&&notices->size()==prior_notices,"confirmed result cleans exact control job and keeps payload");
        else check(after.size()>retained.size()&&keeps(retained,after)&&notices->size()==prior_notices+1,"unconfirmed result retains exact controls with one safe notice");
        retained=after;
    }
    {
        auto lease=fresh();Peer peer;peer.finish_receive(1,"\r");const auto prior_notices=notices->size();
        {native::AlphaFileTransfer host(root,owner,peer.ports(),{},notice);const auto result=host.run(filewire::Direction::receive,{item(lease,true,"cancel.bin",64)});check(result.code==native::NativeFileTransferCode::completed&&!result.files.front().completed&&peer.c.synchronized,"actual empty-batch completed caller cancellation");}
        check(work_files(root/L"Work")==retained&&notices->size()==prior_notices,"confirmed empty cancellation disposes only its control files");
    }
    {
        auto lease=fresh();Peer peer;peer.c.after_read=[&]{peer.c.permit=false;};peer.offer("held.bin","bytes",1);peer.finish_receive(2);const auto prior_notices=notices->size();
        {native::AlphaFileTransfer host(root,owner,peer.ports(),{},notice);const auto result=host.run(filewire::Direction::receive,{item(lease,true,"held.bin",64)});check(result.code!=native::NativeFileTransferCode::completed,"revoked authority cannot produce accepted completion");}
        const auto after=work_files(root/L"Work");check(after.size()>retained.size()&&keeps(retained,after)&&notices->size()==prior_notices+1,"failed/aborted helper retains controls and prior failures");retained=after;
    }
    namespace storage=sfx::adapters::storage;
    check(sfx::test::install_sqlite_write_fault_vfs(root.filename().u8string()),"exact-fixture SQLite delegating fault VFS");
    sfx::test::FakeStoragePathPolicy paths;storage::SqliteStorageSettings settings;settings.data_directory=root/L"Data";settings.utc_now=[]{return std::int64_t{1792248000};};
    storage::SqliteStorage db(settings,paths);check(db.start().succeeded()&&db.shutdown().succeeded(),"current real schema");seed_file_actors(root/L"Data"/L"SFX.DB");check(db.start().succeeded(),"synthetic live actor fixture");unsigned operation=200;
    for(const bool fault:{false,true}){
        Peer peer;const std::string name=fault?"fault.txt":"accepted.txt";peer.offer(name,"real caller upload",1);peer.finish_receive(2);const auto prior_notices=notices->size();
        native::CallerFileTransferPorts ports{db,root,owner,[]{return std::optional<app::FileActor>{app::FileActor{file_operation(2),2,1,1}};},[&]{return file_operation(++operation);},[]{return std::string("2026-10-17");},peer.ports()};ports.work_problem=notice;
        ports.review=[&](const auto&,const auto&,auto)->std::optional<native::CallerUploadReview>{if(fault)sfx::test::inject_next_sqlite_wal_write_fault(SQLITE_IOERR_WRITE);return native::CallerUploadReview{{"Native cleanup test",""},false};};
        native::AlphaCallerFileTransfers host(std::move(ports));app::FileUploadItem choice;choice.name=name;choice.destination={2};const auto result=host.upload({choice},false);const auto after=work_files(root/L"Work");
        check(result.files.size()==1&&result.files.front().received,"actual upload transferred before SQL outcome");
        if(!fault)check(result.files.front().completed&&after==retained&&notices->size()==prior_notices,"actual confirmed SQLite acceptance authorizes control disposal");
        else check(!result.files.front().completed&&after.size()>retained.size()&&keeps(retained,after)&&notices->size()==prior_notices+1&&sfx::test::pending_sqlite_write_fault()==0,"real SQL write failure preserves transfer controls and durable staging without a success claim");
        retained=after;
    }
    check(db.shutdown().succeeded(),"actual writer stopped");sfx::test::uninstall_sqlite_write_fault_vfs();
    check(work_files(root/L"Work")==retained&&retained.at(fs::path(L"foreign.tmp"))==std::vector<std::uint8_t>({'K','E','E','P'}),"all failed/unknown Work bytes remain exact");
    std::cout<<"PASS "<<checks<<" real file-helper/caller-SQL cleanup checks; synthetic actor/channel, no installed/owner acceptance\n";return 0;
}catch(const std::exception& error){sfx::test::uninstall_sqlite_write_fault_vfs();std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}
