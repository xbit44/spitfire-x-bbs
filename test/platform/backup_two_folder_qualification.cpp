// Bounded new disposable installation only. Genuine production Setup/UAC,
// archive, destination policy and Runtime host; no trust/permission bypass.
#include "setup_engine.hpp"
#include "../../src/apps/runtime/alpha_runtime_host.hpp"
#include "sfx/platform/win32/backup_collections.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/bbs_time_zone_provider.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/archive/backup_service.hpp"
#include <objbase.h>
#include <shellapi.h>
#include <winioctl.h>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <thread>
#include <limits>
#include <algorithm>
#include <map>

namespace inst=sfx::platform::win32::installation;
namespace win=sfx::platform::win32;
namespace app=sfx::application;
namespace arc=sfx::archive;
namespace setup=sfx::apps::setup;
namespace fs=std::filesystem;
inst::Path root(L"C:\\xtest-sfx-backup-two-folder-r2-20261011");
const inst::Path fault_root(L"C:\\xtest-sfx-backup-two-folder-fault-r2-20261011");
const inst::Path rebuild_root(L"C:\\xtest-sfx-backup-two-folder-rebuild-r1-20261011");
const inst::Path metadata_root(L"C:\\xtest-sfx-backup-two-folder-metadata-r3-20261011");
unsigned checks{};
void check(bool ok,const char* label){inst::require(ok,label);++checks;std::cout<<"PASS "<<label<<std::endl;}
std::wstring owner(){return inst::operator_sid();}
std::string transaction(unsigned number){std::ostringstream s;s<<std::hex<<std::uppercase<<std::setw(32)<<std::setfill('0')<<number;return s.str();}
arc::Timestamp stamp(unsigned second){
    std::ostringstream n;n<<std::setw(2)<<std::setfill('0')<<second;
    return {"20261011-1200"+n.str(),"2026-10-11T12:00:"+n.str(),"+00:00","2026-10-11T12:00:"+n.str()+"Z"};
}
void put(const fs::path& path,const std::vector<std::uint8_t>& bytes){
    auto sd=inst::mutable_descriptor(owner());inst::create_file(path,bytes,sd.get());
    check(win::WindowsStorageDaclPolicy(owner()).apply(path,win::StorageDaclObjectKind::file).succeeded(),"test private file protection");
}
void two_destinations(){
    std::vector<std::wstring> names;
    for(const auto& entry:fs::directory_iterator(root/L"Backup"))
        if(entry.is_directory())names.push_back(entry.path().filename().native());
    std::sort(names.begin(),names.end());
    check(names==std::vector<std::wstring>({L"Scheduled",L"Upgrades"}),"exact two permanent top-level backup destinations");
}
void perform(bool fresh,bool backup,bool cancel=false){
    check(!inst::elevated(),"ordinary Setup coordinator");
    if(fresh)check(!fs::exists(root),"fresh named root absent");
    else inst::verify_runtime_installation(root,owner());
    auto plan=setup::inspect_plan(root,false);
    check(plan.operation==(fresh?setup::Operation::fresh:setup::Operation::maintenance),"production Setup plan");
    if(!fresh&&!backup)plan.backup.clear();
    const auto before=fresh?0:plan.previous->generation;
    const auto saved_before=fresh?std::optional<app::SchedulerConfiguration>{}:win::read_scheduler_configuration_file(root,owner()).value;
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);check(SUCCEEDED(com),"Setup COM");
    struct Com{~Com(){CoUninitialize();}} release;
    bool cancelled{},failed{};
    try {
        check(setup::execute(plan,{false,false},nullptr,[&](const std::wstring& progress){
            std::wcout<<progress<<std::endl;
            if(cancel && progress.find(L"Creating and verifying")!=std::wstring::npos)cancelled=true;
        },[&]{return !cancelled;}),"genuine Windows Setup completion");
    }catch(const std::exception& e){std::cout<<"RETAINED Setup failure: "<<e.what()<<std::endl;failed=true;}
    check(failed==cancel,"only requested cancellation fails");
    inst::verify_runtime_installation(root,owner());
    const auto receipt=inst::read_receipt(root,owner());
    check(receipt.committed && receipt.generation==before+(cancel?0:1),"committed exact generation");
    two_destinations();
    if(saved_before){
        const auto saved_after=win::read_scheduler_configuration_file(root,owner());
        check(saved_after.value&&app::serialize_scheduler_configuration(*saved_after.value)==app::serialize_scheduler_configuration(*saved_before),"saved schedule destination count time enabled and revision remain exact");
    }
    if(!fresh&&!cancel){
        check(!fs::exists(root/L"Bin"/(L"Previous-"+plan.transaction)),"no Bin transaction history");
        check(!fs::exists(root/L"Recovery"/(L"Setup-"+plan.transaction)),"committed temporary program recovery retired");
        check(backup?arc::inspect_backup(plan.backup).result.ok():plan.backup.empty(),"truthful Yes/No archive choice");
    }
    std::wcout<<L"GENERATION "<<receipt.generation<<L" ID "<<receipt.id<<std::endl;
}
void seed_worker(){
    check(inst::elevated(),"legacy fixture requires Windows consent");
    const auto receipt=inst::read_receipt(root,owner());inst::verify_structure(receipt);
    check(receipt.committed&&receipt.generation==1,"only fresh exact disposable fixture");
    auto structural=inst::structural_descriptor(owner()),content=inst::mutable_descriptor(owner());
    const auto previous=root/L"Bin"/L"Previous-legacy-backup-test";
    inst::create_directory(previous,structural.get());
    inst::create_file(previous/L"history.txt",{'o','l','d','-','p','r','o','g','r','a','m'},structural.get());
    for(const auto& directory:{root/L"Backups",root/L"conf"}) {
        inst::create_directory(directory,content.get());
        inst::create_file(directory/L"preserve.txt",{'l','e','g','a','c','y'},content.get());
        check(win::WindowsStorageDaclPolicy(owner()).apply(directory/L"preserve.txt",win::StorageDaclObjectKind::file).succeeded(),"legacy private file");
    }
    for(const auto* name:{L"Legacy-Flat-test",L"Legacy-Programs-test",L"Legacy-Backups-test"}) {
        const auto directory=root/L"Backup"/name;
        inst::create_directory(directory,content.get());
        inst::create_file(directory/L"history.txt",{'t','e','s','t','-','h','i','s','t','o','r','y'},content.get());
    }
}
void approved_worker(const std::wstring& arguments){
    wchar_t executable[32768]{};check(GetModuleFileNameW(nullptr,executable,32768)>0,"fixture launcher identity");
    SHELLEXECUTEINFOW launch{};launch.cbSize=sizeof(launch);launch.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;
    launch.lpVerb=L"runas";launch.lpFile=executable;launch.lpParameters=arguments.c_str();launch.nShow=SW_HIDE;
    check(ShellExecuteExW(&launch)&&launch.hProcess,"legacy fixture Windows consent");
    inst::Handle process(launch.hProcess);DWORD code{};
    check(WaitForSingleObject(process.get(),60000)==WAIT_OBJECT_0&&GetExitCodeProcess(process.get(),&code)&&code==0,"legacy seed worker exits");
}
void seed(){
    approved_worker(L"--seed-worker");
    arc::ManagedBackupRequest request;request.installation_root=root;request.kind=arc::BackupKind::manual;
    request.destination_directory=root/L"Backup"; // deliberately seeded old flat TEST history
    request.transaction_id=transaction(100);request.source_generation="1";request.created=stamp(1);
    const auto result=arc::create_managed_backup(request);check(result.result.ok(),"verified legacy flat ZIP");
    check(win::WindowsStorageDaclPolicy(owner()).apply(result.archive_path,win::StorageDaclObjectKind::file).succeeded(),"flat ZIP private");
}
void cleanup_worker(const std::wstring& identity,DWORD parent){
    check(inst::elevated(),"disposable history worker genuine Windows consent");
    const auto receipt=inst::read_receipt(root,owner());inst::verify_structure(receipt);
    check(receipt.committed&&receipt.generation==1&&receipt.id==identity,"exact fresh TEST installation identity");
    inst::Handle coordinator(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parent));
    check(coordinator&&WaitForSingleObject(coordinator.get(),0)==WAIT_TIMEOUT&&
        inst::token_sid(inst::process_token(parent).get())==owner(),"same-owner live cleanup coordinator");
    inst::Handle guard(OpenMutexW(SYNCHRONIZE,FALSE,win::installation_object_names(root).mutex_name.c_str()));
    check(guard!=nullptr,"stopped installation guard remains held by coordinator");
    check(fs::is_empty(root/L"Work")&&!fs::exists(root/L"Recovery"),"no active or failed recovery cleanup target");
    std::vector<inst::Handle> ancestors;
    for(const auto& path:{root,root/L"Backup",root/L"Bin",root/L"Work"})
        ancestors.push_back(inst::open_object(path,true,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE));
    struct Object {fs::path path;inst::Handle handle;};std::vector<Object> objects;
    const auto pin=[&](const fs::path& path,bool directory,const std::string& expected){
        auto file=inst::open_object(path,directory,GENERIC_READ|READ_CONTROL|DELETE,
            FILE_SHARE_READ|(directory?FILE_SHARE_WRITE:0));
        std::string hash;
        if(!directory){LARGE_INTEGER size{};check(GetFileSizeEx(file.get(),&size)&&size.QuadPart>=0&&size.QuadPart<16*1024*1024,"bounded TEST history file size");
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));DWORD read{};
            check(ReadFile(file.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)&&read==bytes.size(),"held TEST history read");
            hash=inst::sha256(bytes);check(hash==expected,"exact seeded history digest, not filename-only deletion");}
        std::cout<<"REVIEWED TARGET "<<path.u8string()<<" ID "<<inst::file_id(file.get())<<" VOLUME "<<inst::volume_serial(file.get())<<" SHA256 "<<hash<<std::endl;
        objects.push_back({path,std::move(file)});
    };
    std::map<fs::path,std::pair<std::wstring,std::string>> directories;
    directories[root/L"Bin"/L"Previous-legacy-backup-test"]={L"history.txt","old-program"};
    directories[root/L"Backups"]={L"preserve.txt","legacy"};
    for(const auto* name:{L"Legacy-Flat-test",L"Legacy-Programs-test",L"Legacy-Backups-test"})
        directories[root/L"Backup"/name]={L"history.txt","test-history"};
    std::vector<inst::Handle> parents;
    for(const auto& item:directories){
        auto parent_pin=inst::open_object(item.first,true,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
        std::vector<std::wstring> names;for(const auto& entry:fs::directory_iterator(item.first))names.push_back(entry.path().filename().native());
        check(names==std::vector<std::wstring>({item.second.first}),"complete seeded history inventory; unknown files refuse cleanup");
        pin(item.first/item.second.first,false,inst::sha256(std::vector<std::uint8_t>(item.second.second.begin(),item.second.second.end())));
        parent_pin.reset();pin(item.first,true,{});
    }
    const auto flat=root/L"Backup"/arc::managed_backup_filename(arc::BackupKind::manual,stamp(1));
    const auto inspected=arc::inspect_backup(flat);
    check(inspected.result.ok()&&inspected.metadata.transaction_id==transaction(100)&&inspected.metadata.source_generation=="1", "seeded flat archive exact transaction/generation");
    pin(flat,false,inst::file_sha256(flat));
    // Every exact object is validated and pinned before the first native delete.
    // No wildcard, recursive shell deletion, source-tree, current data or unknown target.
    for(auto& object:objects){FILE_DISPOSITION_INFO removed{TRUE};
        check(SetFileInformationByHandle(object.handle.get(),FileDispositionInfo,&removed,sizeof(removed))!=FALSE,"native exact held-object retirement");
        std::cout<<"REMOVED TARGET "<<object.path.u8string()<<std::endl;object.handle.reset();}
}
void cleanup(){
    inst::verify_runtime_installation(root,owner());win::OfflineInstallationGuard guard;
    check(guard.acquire(root,owner()).succeeded(),"stopped exclusive disposable TEST history cleanup");
    const auto receipt=inst::read_receipt(root,owner());check(receipt.generation==1,"only newly seeded TEST history, never prior evidence fixture");
    std::vector<fs::path> planned;
    for(const auto& directory:{root/L"Bin"/L"Previous-legacy-backup-test",root/L"Backups",
        root/L"Backup"/L"Legacy-Flat-test",root/L"Backup"/L"Legacy-Programs-test",root/L"Backup"/L"Legacy-Backups-test"}){
        auto pin=inst::open_object(directory,true);std::vector<fs::path> children;
        for(const auto& entry:fs::directory_iterator(directory))children.push_back(entry.path());
        check(children.size()==1,"parent inventories complete exact disposable tree before Windows consent");
        for(const auto& file:children){auto handle=inst::open_object(file,false);
            std::cout<<"REVIEWED TARGET "<<file.u8string()<<" ID "<<inst::file_id(handle.get())<<" VOLUME "<<inst::volume_serial(handle.get())<<" SHA256 "<<inst::file_sha256(file)<<std::endl;planned.push_back(file);}
        std::cout<<"REVIEWED TARGET "<<directory.u8string()<<" ID "<<inst::file_id(pin.get())<<" VOLUME "<<inst::volume_serial(pin.get())<<std::endl;planned.push_back(directory);
    }
    const auto flat=root/L"Backup"/arc::managed_backup_filename(arc::BackupKind::manual,stamp(1));
    std::cout<<"REVIEWED TARGET "<<flat.u8string()<<" SHA256 "<<inst::file_sha256(flat)<<std::endl;planned.push_back(flat);
    approved_worker(L"--cleanup-worker "+receipt.id+L" "+std::to_wstring(GetCurrentProcessId()));
    for(const auto& path:planned){check(!fs::exists(path),"exact reviewed TEST history object removed");std::cout<<"REMOVED TARGET "<<path.u8string()<<std::endl;}
    two_destinations();check(fs::exists(root/L"conf"/L"preserve.txt"),"current legacy configuration is NOT disposable history");
}
void fault_worker(const std::wstring& transaction_id){
    root=fault_root;check(inst::elevated(),"fault fixture Windows consent");
    const auto receipt=inst::read_receipt(root,owner());inst::verify_structure(receipt);
    check(receipt.committed&&receipt.generation==1&&transaction_id.size()==32&&transaction_id.find_first_not_of(L"0123456789ABCDEF")==std::wstring::npos,"exact fresh fault identity");
    auto sd=inst::structural_descriptor(owner());
    for(const auto* suffix:{L".new-",L".restore-"})
        inst::create_file(root/L"Bin"/(std::wstring(inst::programs[0])+suffix+transaction_id),{'c','o','l','l','i','s','i','o','n'},sd.get());
}
void fault(){
    root=fault_root;inst::verify_runtime_installation(root,owner());auto plan=setup::inspect_plan(root,false);
    check(plan.previous&&plan.previous->generation==1,"fresh fault plan");
    approved_worker(L"--fault-worker "+plan.transaction);
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);check(SUCCEEDED(com),"fault Setup COM");
    struct Com{~Com(){CoUninitialize();}} release;
    bool failed=false;try{(void)setup::execute(plan,{false,false},nullptr,[](const std::wstring& p){std::wcout<<p<<std::endl;},[]{return true;});}
    catch(const std::exception& e){std::cout<<"RETAINED genuine installation failure: "<<e.what()<<std::endl;failed=true;}
    check(failed,"protected program and rollback collision refuses");
    const auto receipt=inst::read_receipt(root,owner());check(!receipt.committed,"durable incomplete receipt stays closed");
    const auto recovery=root/L"Recovery"/(L"Setup-"+plan.transaction);
    for(std::size_t i=0;i<plan.previous->program_count;++i)
        check(inst::file_sha256(recovery/inst::programs[i])==plan.previous->program_hashes[i],"indispensable recovery program exact");
    check(inst::read_file(recovery/L"receipt.recovery",inst::maximum_receipt_bytes)==inst::encode_receipt(*plan.previous),"previous recovery receipt exact");
    check(arc::inspect_backup(plan.backup).result.ok()&&!fs::exists(fs::path(plan.backup.native()+L".sfx-owned")),"failed update archive retained but not retention eligible");
    bool refused=false;try{inst::verify_runtime_installation(root,owner());}catch(const std::exception&){refused=true;}
    check(refused,"restarted installation validation fails closed");
    std::cout<<"FAULT MATERIALS RETAINED "<<recovery.u8string()<<std::endl;
}
fs::path create(win::BackupCollection& collection,unsigned number,bool mark=true,unsigned retain=2){
    arc::ManagedBackupRequest request;request.installation_root=root;request.destination_directory=collection.directory;
    request.kind=number%2?arc::BackupKind::manual:arc::BackupKind::scheduled;
    request.transaction_id=transaction(number);request.source_generation=std::to_string(inst::read_receipt(root,owner()).generation);
    request.created=stamp(number);const auto made=arc::create_managed_backup(request);
    check(made.result.ok(),"private selected destination publication");
    check(win::WindowsStorageDaclPolicy(owner()).apply(made.archive_path,win::StorageDaclObjectKind::file).succeeded(),"archive DACL");
    if(mark)(void)win::finish_backup_collection(root,owner(),collection,made.archive_path,request.kind,retain);
    return made.archive_path;
}
void collections(){
    inst::verify_runtime_installation(root,owner());win::OfflineInstallationGuard guard;
    check(guard.acquire(root,owner()).succeeded(),"stopped exclusive collection qualification");
    const auto config=win::read_scheduler_configuration_file(root,owner());
    check(config.value&& !config.value->enabled&&config.value->backup_destination.empty()&&config.value->retained_backups==10,"fresh schedule Off and defaults");
    auto collection=win::open_backup_collection(root,owner(),arc::BackupKind::manual);
    check(collection.directory==root/L"Backup"/L"Scheduled","ordinary default Scheduled");
    put(collection.directory/L"unrelated.zip",{'n','o','t','-','s','f','x'});
    put(collection.directory/L"SFX-backup-20261011-120059.zip.partial",{'p','a','r','t','i','a','l'});
    const auto unknown=create(collection,2,false);
    const auto unknown_hash=inst::file_sha256(unknown);
    std::vector<fs::path> archives;
    for(unsigned n=3;n<7;++n)archives.push_back(create(collection,n));
    check(!fs::exists(archives[0])&&!fs::exists(archives[1])&&fs::exists(archives[2])&&fs::exists(archives[3]),"manual scheduled shared newest two");
    check(inst::file_sha256(unknown)==unknown_hash&&fs::exists(collection.directory/L"unrelated.zip")&&fs::exists(collection.directory/L"SFX-backup-20261011-120059.zip.partial"),"unknown partial unrelated untouched");
    const auto retained_hash=inst::file_sha256(archives[3]);
    arc::ManagedBackupRequest cancelled;cancelled.installation_root=root;cancelled.destination_directory=collection.directory;
    cancelled.transaction_id=transaction(9);cancelled.created=stamp(9);cancelled.cancelled=[]{return true;};
    check(arc::create_managed_backup(cancelled).result.error==arc::Error::cancelled,"cancelled publication refused");
    check(inst::file_sha256(archives[3])==retained_hash,"cancel leaves usable history exact");
    const auto marker=fs::path(archives[2].native()+L".sfx-owned");
    {auto file=inst::open_object(marker,false,GENERIC_WRITE,0);DWORD written{};check(WriteFile(file.get(),"X",1,&written,nullptr)&&written==1&&FlushFileBuffers(file.get()),"corrupt marker fixture");}
    const auto invalid_owned_hash=inst::file_sha256(archives[2]);
    (void)create(collection,7,true,1);
    check(inst::file_sha256(archives[2])==invalid_owned_hash&&!fs::exists(archives[3]),"invalid owner marker excluded; eligible oldest pruned");
    for(const auto& destination:{std::string("C:\\Missing-SFX-parent-20261010\\backups"),(root/L"Bin"/L"backups").u8string(),(root/L"Data"/L"backups").u8string(),std::string("Z:\\SFX-backups"),std::string("\\\\server\\share")}){
        bool refused=false;try{(void)win::open_backup_collection(root,owner(),arc::BackupKind::manual,destination);}catch(const std::exception&){refused=true;}
        check(refused,"unsafe unavailable path no fallback");
    }
    const auto second=root/L"Backup"/L"Scheduled"/L"Alternate-local";
    auto alternate=win::open_backup_collection(root,owner(),arc::BackupKind::manual,second.u8string());
    const auto alternate_file=create(alternate,10,true,1);
    check(fs::exists(unknown)&&fs::exists(archives[2])&&fs::exists(alternate_file),"destination change preserves old history");
    auto saved=*config.value;saved.format_version=2;saved.revision++;saved.backup_destination=second.u8string();saved.retained_backups=1;
    check(win::publish_scheduler_configuration_file(root,owner(),saved).succeeded(),"save destination count atomically");
    const auto read=win::read_scheduler_configuration_file(root,owner());
    check(read.value&&read.value->backup_destination==second.u8string()&&!read.value->enabled&&read.value->retained_backups==1,"saved choices persist while Off");
}
void legacy_settings(){
    app::SchedulerConfiguration config;config.format_version=1;config.enabled=true;config.local_hour=5;config.local_minute=17;config.revision=90;
    check(win::publish_scheduler_configuration_file(root,owner(),config).succeeded(),"saved legacy enabled schedule");
}
void settings(const std::string& destination){
    inst::verify_runtime_installation(root,owner());win::OfflineInstallationGuard guard;
    check(guard.acquire(root,owner()).succeeded(),"stopped backup settings guard");
    const auto saved=win::read_scheduler_configuration_file(root,owner());check(saved.value.has_value(),"existing saved backup settings");
    auto config=*saved.value;config.format_version=2;config.revision++;config.enabled=false;
    config.backup_destination=destination;config.retained_backups=1;
    check(win::publish_scheduler_configuration_file(root,owner(),config).succeeded(),"explicit fixture destination saved");
}
void refusal_worker(){
    check(inst::elevated(),"refusal fixture Windows consent");
    const auto receipt=inst::read_receipt(root,owner());inst::verify_structure(receipt);
    check(receipt.committed&&receipt.generation==1,"exact fresh refusal fixture identity");
    auto descriptor=inst::structural_descriptor(owner());
    inst::create_directory(root/L"Backup"/L"Scheduled"/L"Refusal-Unwritable",descriptor.get());
}
void collection_failures(){
    inst::verify_runtime_installation(root,owner());win::OfflineInstallationGuard guard;
    check(guard.acquire(root,owner()).succeeded(),"stopped refusal collection guard");
    put(root/L"Backup"/L"Scheduled"/L"Refusal-File",{'n','o','t','-','a','-','f','o','l','d','e','r'});
    const auto file_hash=inst::file_sha256(root/L"Backup"/L"Scheduled"/L"Refusal-File");
    for(const auto& path:{root/L"Backup"/L"Scheduled"/L"Refusal-File",root/L"Backup"/L"Scheduled"/L"Refusal-Unwritable"}){
        bool refused=false;try{(void)win::open_backup_collection(root,owner(),arc::BackupKind::manual,path.u8string());}catch(const std::exception& e){refused=true;std::cout<<"REFUSAL "<<e.what()<<std::endl;}
        check(refused,"file collision or unwritable destination refused without repair");
    }
    check(inst::file_sha256(root/L"Backup"/L"Scheduled"/L"Refusal-File")==file_hash,"collision object unchanged");
    auto collection=win::open_backup_collection(root,owner(),arc::BackupKind::manual,(root/L"Backup"/L"Scheduled"/L"Failure-Collection").u8string());
    const auto archive=create(collection,21,true,1);const auto old_hash=inst::file_sha256(archive);
    arc::ManagedBackupRequest request;request.installation_root=root;request.destination_directory=collection.directory;
    request.kind=arc::BackupKind::manual;request.transaction_id=transaction(22);request.created=stamp(21);
    check(!arc::create_managed_backup(request).result.ok(),"same archive filename never overwritten");
    check(inst::file_sha256(archive)==old_hash,"failed publication preserves verified history");
    const auto unmarked=create(collection,23,false);const auto marker=fs::path(unmarked.native()+L".sfx-owned");
    put(marker,{'u','n','r','e','l','a','t','e','d'});const auto marker_hash=inst::file_sha256(marker);
    bool refused=false;try{(void)win::finish_backup_collection(root,owner(),collection,unmarked,arc::BackupKind::manual,1);}catch(const std::exception&){refused=true;}
    check(refused&&inst::file_sha256(marker)==marker_hash&&inst::file_sha256(archive)==old_hash&&fs::exists(unmarked),"owner marker collision retains both verified archives and unrelated object");
}
void bootstrap(){
    inst::verify_runtime_installation(root,owner());
    sfx::apps::runtime::AlphaRuntimeHost host(root);const auto started=host.start();
    std::cout<<"FIRST RUN RESPONSE "<<static_cast<unsigned>(started.code)<<std::endl;
    check(started.code==app::LocalControlResponseCode::setup_required,"fresh supported first-run state");
    app::AlphaBootstrapRequest request;request.configuration.bbs_name="Backup Redesign Fixture";
    request.configuration.bbs_time_zone="Pacific Standard Time";request.configuration.logical_nodes=4;
    request.configuration.telnet.bind_address="127.0.0.1";request.configuration.telnet.port=25281;
    request.configuration.telnet.bot_gate_enabled=false;request.configuration.botgate.enabled=false;
    request.configuration.ssh.enabled=true;request.configuration.ssh.bind_address="127.0.0.1";request.configuration.ssh.port=25282;
    request.sysop_name="SSH Fixture";request.sysop_password="Sfx-test-password-123";request.system_password="Sfx-system-password-456";
    check(host.bootstrap(request).code==app::LocalControlResponseCode::ok,"real first-run bootstrap");
    for(const auto* name:{"Caller One","Caller Two","Caller Three","Caller Four"}){
        app::LocalAccountRequest create;create.action=app::LocalAccountAction::create_user;create.name=name;create.expected_revision=1;
        create.security_level=10;create.purge_allowed=true;create.password=create.confirmation="Sfx-test-password-123";
        check(host.administer_account(create).code==app::LocalAccountResultCode::ok,"synthetic caller through supported owner API");
    }
    check(host.shutdown().code==app::LocalControlResponseCode::ok,"bootstrap orderly stop");
}
void metadata(){
    root=metadata_root;perform(true,false);
    win::OfflineInstallationGuard guard;check(guard.acquire(root,owner()).succeeded(),"isolated metadata classification stopped guard");
    auto inspect=[&](bool expected,const char* label){
        const auto state=win::alpha_installation_has_persistent_state(root,owner());
        check(state&&state.value&&*state.value==expected,label);
    };
    inspect(false,"only exact empty private backup destinations permit first-run classification");
    auto descriptor=inst::mutable_descriptor(owner());
    const auto unknown=root/L"Backup"/L"Unknown-empty-test";
    inst::create_directory(unknown,descriptor.get());inspect(true,"unknown empty backup folder remains persistent evidence");
    check(fs::is_empty(unknown)&&RemoveDirectoryW(unknown.c_str()),"remove only just-created exact empty classification control");
    const auto proof=root/L"Backup"/L"Scheduled"/L"metadata-proof.txt";
    put(proof,{'p','r','o','o','f'});inspect(true,"nonempty scheduled folder remains persistent evidence");
    check(inst::read_file(proof,5)==std::vector<std::uint8_t>({'p','r','o','o','f'})&&DeleteFileW(proof.c_str()),"remove only exact just-created proof control");
    const auto upgrades=root/L"Backup"/L"Upgrades";
    const std::wstring target=(root/L"Logs").native(),substitute=L"\\??\\"+target;
    const WORD subbytes=static_cast<WORD>(substitute.size()*2),printbytes=static_cast<WORD>(target.size()*2),offset=subbytes+2;
    std::vector<BYTE> buffer(16+subbytes+2+printbytes+2,0);const DWORD tag=IO_REPARSE_TAG_MOUNT_POINT;const WORD length=static_cast<WORD>(buffer.size()-8);
    std::memcpy(buffer.data(),&tag,4);std::memcpy(buffer.data()+4,&length,2);std::memcpy(buffer.data()+10,&subbytes,2);
    std::memcpy(buffer.data()+12,&offset,2);std::memcpy(buffer.data()+14,&printbytes,2);
    std::memcpy(buffer.data()+16,substitute.data(),subbytes);std::memcpy(buffer.data()+16+offset,target.data(),printbytes);
    auto pin=inst::open_object(upgrades,true,FILE_WRITE_ATTRIBUTES|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE);DWORD returned{};
    check(fs::is_empty(upgrades)&&DeviceIoControl(pin.get(),FSCTL_SET_REPARSE_POINT,buffer.data(),static_cast<DWORD>(buffer.size()),nullptr,0,&returned,nullptr),"actual isolated exact metadata-child junction control");
    inspect(true,"redirected named backup child is persistent evidence without following it");
    std::array<BYTE,8> remove{};std::memcpy(remove.data(),&tag,4);
    check(DeviceIoControl(pin.get(),FSCTL_DELETE_REPARSE_POINT,remove.data(),static_cast<DWORD>(remove.size()),nullptr,0,&returned,nullptr),"retire only just-created junction metadata on same held test handle");pin.reset();
    inspect(false,"empty private metadata state remains exact after junction control");
    // Keep the structural DACL negative control last: it deliberately denies
    // ordinary WRITE_DAC. Preserve this hostile fixture, never bypass its policy
    // just to reset a test object.
    const auto scheduled=root/L"Backup"/L"Scheduled";auto wrong=inst::structural_descriptor(owner());
    check(SetFileSecurityW(scheduled.c_str(),DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,wrong.get())!=FALSE,"isolated incorrect child DACL control");
    inspect(true,"empty incorrectly protected scheduled child is not a first-run waiver");two_destinations();
}
void rebuild_source_worker(){
    root=rebuild_root;check(inst::elevated(),"rebuild source fixture genuine Windows consent");
    const auto receipt=inst::read_receipt(root,owner());inst::verify_structure(receipt);
    check(receipt.committed&&receipt.generation==1,"only new disposable rebuild fixture");
    auto descriptor=inst::mutable_descriptor(owner());
    inst::create_directory(root/L"Unknown-source",descriptor.get());
    inst::create_file(root/L"Unknown-source"/L"notes.txt",{'o','w','n','e','r','-','u','n','k','n','o','w','n'},descriptor.get());
    const auto notes=root/L"Unknown-source"/L"notes.txt";
    inst::Handle stream(CreateFileW((notes.native()+L":owner-proof").c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));DWORD bytes{};
    check(stream.get()!=INVALID_HANDLE_VALUE&&WriteFile(stream.get(),"stream-marker",13,&bytes,nullptr)&&bytes==13&&FlushFileBuffers(stream.get()),"original unknown source named-stream metadata");stream.reset();
    check(SetFileAttributesW(notes.c_str(),FILE_ATTRIBUTE_READONLY)!=FALSE,"original unknown source read-only metadata");
    auto program=inst::open_object(root/L"Bin"/inst::programs[4],false,GENERIC_WRITE,0);
    check(WriteFile(program.get(),"X",1,&bytes,nullptr)&&bytes==1&&FlushFileBuffers(program.get()),"deliberate provenance mismatch on exact TEST source; never execute it");
}
void rebuild(){
    root=rebuild_root;perform(true,false);bootstrap();
    {win::OfflineInstallationGuard guard;check(guard.acquire(root,owner()).succeeded(),"stopped rebuild source preparation");approved_worker(L"--rebuild-source-worker");}
    const auto old_db=inst::file_sha256(root/L"Data"/L"SFX.DB"),old_program=inst::file_sha256(root/L"Bin"/inst::programs[4]);
    auto plan=setup::inspect_plan(root,false);check(plan.operation==setup::Operation::rebuild&&!plan.previous,"existing unverified-source automatic rebuild behavior unchanged");
    check(plan.backup.parent_path()==root/L"Backup"/L"Upgrades","rebuild ZIP uses approved Upgrades destination");
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);check(SUCCEEDED(com),"rebuild Setup COM");struct Com{~Com(){CoUninitialize();}} release;
    check(setup::execute(plan,{false,false},nullptr,[](const std::wstring& p){std::wcout<<p<<std::endl;},[]{return true;}),"existing automatic rebuild/import succeeds with genuine UAC");
    const auto source=root/L"Backup"/L"Upgrades"/(L"Migration-Source-"+plan.transaction);
    auto source_pin=inst::open_object(source,true);check(inst::file_id(source_pin.get())==plan.reviewed_root_id&&inst::volume_serial(source_pin.get())==plan.reviewed_volume,"intact original source object preserved, not reconstructed");source_pin.reset();
    check(inst::file_sha256(source/L"Data"/L"SFX.DB")==old_db&&inst::file_sha256(source/L"Bin"/inst::programs[4])==old_program,"original database and untrusted program exact; no source program adoption");
    check(inst::file_sha256(root/L"Data"/L"SFX.DB")==old_db,"real source database imported byte-exact");
    const auto notes=source/L"Unknown-source"/L"notes.txt";
    check((GetFileAttributesW(notes.c_str())&FILE_ATTRIBUTE_READONLY)!=0,"source read-only metadata retained");
    inst::Handle stream(CreateFileW((notes.native()+L":owner-proof").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));char bytes[13]{};DWORD read{};
    check(stream.get()!=INVALID_HANDLE_VALUE&&ReadFile(stream.get(),bytes,13,&read,nullptr)&&read==13&&std::string(bytes,13)=="stream-marker","intact source named stream retained");stream.reset();
    const auto imported=root/L"Backup"/L"Upgrades"/(L"ImportedRoot-"+plan.transaction)/L"Unknown-source"/L"notes.txt";
    check(inst::read_file(imported,100)==std::vector<std::uint8_t>({'o','w','n','e','r','-','u','n','k','n','o','w','n'}),"unknown source content retained inside Upgrades");
    const auto archive=arc::inspect_backup(plan.backup);check(archive.result.ok()&&archive.metadata.backup_type=="setup-migration-source","accurate distinct rebuild source archive type");
    const auto archive_hash=inst::file_sha256(plan.backup);
    for(unsigned n=0;n<4;++n){Sleep(1100);perform(false,true);}
    check(inst::file_sha256(plan.backup)==archive_hash&&fs::exists(source)&&fs::exists(imported),"newest-three ordinary retention never prunes rebuild source or unknown files");
    unsigned ordinary{};for(const auto& entry:fs::directory_iterator(root/L"Backup"/L"Upgrades"))
        if(entry.path().extension()==L".zip"&&fs::exists(fs::path(entry.path().native()+L".sfx-owned")))++ordinary;
    check(ordinary==3,"repeated updates keep exactly three managed ordinary upgrade ZIPs");two_destinations();
    std::cout<<"REBUILD SOURCE "<<source.u8string()<<"\nREBUILD ARCHIVE "<<plan.backup.u8string()<<std::endl;
}
void restore(){
    inst::verify_runtime_installation(root,owner());win::OfflineInstallationGuard guard;
    check(guard.acquire(root,owner()).succeeded(),"real stopped restore guard");
    const auto config=win::read_scheduler_configuration_file(root,owner());check(config.value.has_value(),"restore selected destination");
    auto collection=win::open_backup_collection(root,owner(),arc::BackupKind::manual,config.value->backup_destination,false);
    fs::path selected;fs::file_time_type newest=fs::file_time_type::min();
    for(const auto& entry:fs::directory_iterator(collection.directory)){
        if(entry.path().extension()!=L".zip")continue;
        const auto inspection=arc::inspect_backup(entry.path());if(!inspection.result.ok())continue;
        if(std::none_of(inspection.files.begin(),inspection.files.end(),[](const auto& f){return f.path=="Data/SFX.DB";}))continue;
        if(entry.last_write_time()>newest){newest=entry.last_write_time();selected=entry.path();}
    }
    check(!selected.empty(),"verified SQLite archive selected");
    put(root/L"Config"/L"restore-proof.txt",{'p','r','i','o','r','-','s','t','a','t','e'});
    arc::RestoreRequest request;request.installation_root=root;request.archive_path=selected;
    const auto restore_id=inst::unique_id();
    for(const auto ch:restore_id){check((ch>=L'0'&&ch<=L'9')||(ch>=L'A'&&ch<=L'F'),"exact restore transaction character");request.transaction_id.push_back(static_cast<char>(ch));}
    request.source_generation=std::to_string(inst::read_receipt(root,owner()).generation);request.pre_restore_created=arc::current_timestamp();
    request.secure_live_directory=[&](const fs::path& path){const auto status=win::WindowsStorageDaclPolicy(owner()).apply(path,win::StorageDaclObjectKind::directory);return status?arc::Result{}:arc::Result{arc::Error::publication_failed,status.operation};};
    request.secure_live_file=[&](const fs::path& path){const auto status=win::WindowsStorageDaclPolicy(owner()).apply(path,win::StorageDaclObjectKind::file);return status?arc::Result{}:arc::Result{arc::Error::publication_failed,status.operation};};
    const auto result=arc::restore_backup(request);check(result.result.ok()&&result.live_state_changed,"production mutable-data restore completes");
    check(!fs::exists(root/L"Config"/L"restore-proof.txt")&&inst::read_file(result.retained_previous_state/L"Config"/L"restore-proof.txt",100)==std::vector<std::uint8_t>({'p','r','i','o','r','-','s','t','a','t','e'}),"prior state preserved; snapshot published");
    check(arc::inspect_backup(result.pre_restore_archive).result.ok(),"independent pre-restore ZIP verified");
    inst::verify_runtime_installation(root,owner());
    std::cout<<"RESTORE ARCHIVE "<<selected.u8string()<<"\nRESTORE PREVIOUS "<<result.retained_previous_state.u8string()<<std::endl;
}
void schedule(){
    inst::verify_runtime_installation(root,owner());win::OfflineInstallationGuard guard;
    check(guard.acquire(root,owner()).succeeded(),"stopped schedule settings guard");
    const auto saved=win::read_scheduler_configuration_file(root,owner());check(saved.value.has_value(),"saved schedule before deliberate edit");
    auto rules=win::WindowsRegistryBbsTimeZoneRuleProvider::create("Pacific Standard Time");check(rules!=nullptr,"actual BBS timezone rules");
    class Clock final:public app::IClock{public:std::int64_t unix_seconds()const noexcept override{return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}std::uint64_t monotonic_millis()const noexcept override{return GetTickCount64();}} clock;
    const auto next=clock.unix_seconds()/60*60+120;app::BbsLocalTimeAdapters local(rules->configuration(),*rules,clock);
    const auto wall=local.format_utc_seconds(next);check(wall&&wall->size()>=19,"real next-minute BBS civil time");
    auto config=*saved.value;config.format_version=2;config.revision++;config.enabled=true;
    config.local_hour=static_cast<std::uint8_t>(std::stoi(wall->substr(11,2)));config.local_minute=static_cast<std::uint8_t>(std::stoi(wall->substr(14,2)));
    check(win::publish_scheduler_configuration_file(root,owner(),config).succeeded(),"save enabled daily schedule to same destination/count");
    std::cout<<"SCHEDULE UTC "<<next<<std::endl;
}
void runtime_backup(){
    inst::verify_runtime_installation(root,owner());
    const auto settings=win::read_scheduler_configuration_file(root,owner());check(settings.value.has_value(),"saved backup settings");
    sfx::apps::runtime::AlphaRuntimeHost host(root);check(host.start().code==app::LocalControlResponseCode::ok,"installed host starts saved BBS");
    check(host.run_scheduler_now({}).code==app::LocalControlResponseCode::ok,"manual backup accepted independently");
    bool completed=false;
    for(unsigned n=0;n<120;++n){const auto status=host.status();if(status.scheduler.last_result==app::ScheduledJobResult::success&&!status.scheduler.active){completed=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(250));}
    const auto status=host.status();std::cout<<"BACKUP DETAIL "<<status.scheduler.last_detail<<std::endl;
    check(completed,"real consistent SQLite backup completion");
    check(host.shutdown().code==app::LocalControlResponseCode::ok,"Runtime orderly stop after backup");
}
int wmain(int argc,wchar_t**argv){try{
    if(argc==4&&std::wstring(argv[1])==L"--worker"){
        const auto pid=std::stoull(argv[3]);inst::require(pid&&pid<=std::numeric_limits<DWORD>::max()&&std::to_wstring(pid)==argv[3],"worker PID");return setup::worker(argv[2],static_cast<DWORD>(pid));
    }
    if(argc==3&&std::wstring(argv[1])==L"--fault-worker"){fault_worker(argv[2]);return 0;}
    if(argc==4&&std::wstring(argv[1])==L"--cleanup-worker"){
        const auto parent=std::stoull(argv[3]);check(parent&&parent<=std::numeric_limits<DWORD>::max()&&std::to_wstring(parent)==argv[3],"cleanup parent PID");cleanup_worker(argv[2],static_cast<DWORD>(parent));return 0;
    }
    inst::require(argc==2,"one bounded action required");const std::wstring action=argv[1];
    if(action==L"--seed-worker")seed_worker();
    else if(action==L"--rebuild-source-worker")rebuild_source_worker();
    else if(action==L"--refusal-worker")refusal_worker();
    else{check(!inst::elevated(),"ordinary qualification token");
        if(action==L"--fresh")perform(true,false);
        else if(action==L"--bootstrap")bootstrap();
        else if(action==L"--fault-fresh"){root=fault_root;perform(true,false);}
        else if(action==L"--fault")fault();
        else if(action==L"--yes")perform(false,true);
        else if(action==L"--no")perform(false,false);
        else if(action==L"--cancel")perform(false,true,true);
        else if(action==L"--seed")seed();
        else if(action==L"--cleanup")cleanup();
        else if(action==L"--rebuild")rebuild();
        else if(action==L"--layout")two_destinations();
        else if(action==L"--metadata")metadata();
        else if(action==L"--collections")collections();
        else if(action==L"--legacy-settings")legacy_settings();
        else if(action==L"--settings-alternate")settings((root/L"Backup"/L"Scheduled"/L"Alternate-local").u8string());
        else if(action==L"--settings-unavailable")settings("Z:\\SFX-backup-redesign-unavailable-20261010");
        else if(action==L"--settings-unwritable")settings((root/L"Backup"/L"Scheduled"/L"Refusal-Unwritable").u8string());
        else if(action==L"--prepare-refusals")approved_worker(L"--refusal-worker");
        else if(action==L"--collection-failures")collection_failures();
        else if(action==L"--runtime-backup")runtime_backup();
        else if(action==L"--restore")restore();
        else if(action==L"--schedule")schedule();
        else inst::require(false,"unknown bounded action");
    }
    std::cout<<"BACKUP QUALIFICATION PASS "<<checks<<std::endl;return 0;
}catch(const std::exception&e){std::cerr<<"BACKUP QUALIFICATION FAIL "<<e.what()<<std::endl;return 1;}}
