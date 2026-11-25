// Native backup/restore qualification on a newly installed, initialized,
// otherwise-unused Section9 fixture. Synthetic payload mutations only.
#include "sfx/platform/win32/file_backup.hpp"
#include "sfx/platform/win32/offline_account_administration.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/secure_identifier_generator.hpp"
#include <iostream>
#include <algorithm>
#include <set>
namespace win=sfx::platform::win32;namespace ins=win::installation;namespace app=sfx::application;namespace arc=sfx::archive;
namespace {
unsigned checks{},stamp_id{};
void check(bool ok,const char* why){++checks;ins::require(ok,why);std::cout<<"PASS "<<why<<std::endl;}
std::string token(){std::string out;for(const auto ch:ins::unique_id())out+=static_cast<char>(ch);return out;}
std::string two(unsigned n){return n<10?"0"+std::to_string(n):std::to_string(n);}
void write(const win::FilePayloadDirectory& folder,const std::string& name,const std::string& data){auto output=folder.create(name,std::max<std::size_t>(1,data.size()));output.append(reinterpret_cast<const std::uint8_t*>(data.data()),data.size());(void)output.identity();}
void remove(const win::FilePayloadDirectory& folder,const std::string& name){auto file=folder.read(name,true);file.remove_exact();}
std::string read(const win::FilePayloadDirectory& folder,const std::string& name){auto file=folder.read(name);const auto bytes=file.read(0,65536);return {bytes.begin(),bytes.end()};}
app::FileConfiguration config(const ins::Path& root,const std::wstring& owner){const auto result=win::offline_file_configuration_catalog(root,owner);check(result&&result.value,"actual current file configuration");return *result.value;}
void enable(const ins::Path& root,const std::wstring& owner,bool value){auto current=config(root,owner);const auto found=std::find_if(current.areas.begin(),current.areas.end(),[](const auto& a){return a.area.id.value==1;});check(found!=current.areas.end(),"real General area identity");
    app::FileConfigurationChange change;win::WindowsSecureIdentifierGenerator ids;change.operation=ids.generate();change.action=app::FileConfigurationAction::area;change.expected_catalog_revision=current.policy.catalog_revision;change.area=*found;change.area.area.backup_contents=value;
    check(win::offline_change_file_configuration(root,owner,change).code==app::LocalAccountResultCode::ok,"supported saved backup-content choice");}
ins::Path save(const ins::Path& root,const std::wstring& owner,unsigned retained=1,const std::function<void()>& before_retention={}){
    win::OfflineInstallationGuard guard;check(guard.acquire(root,owner).succeeded(),"genuine stopped exclusive backup guard");const auto current=win::read_backup_file_configuration(root,owner);
    auto collection=win::open_backup_collection(root,owner,arc::BackupKind::manual);
    auto capture=win::FileBackupCapture::begin(root,owner,current,collection,arc::BackupKind::manual);
    arc::ManagedBackupRequest request;request.installation_root=root;request.destination_directory=collection.directory;request.kind=arc::BackupKind::manual;request.transaction_id=token();request.source_generation=std::to_string(ins::read_receipt(root,owner).generation);
    const auto second=two(++stamp_id);request.created={"20261017-0200"+second,"2026-10-17T02:00:"+second,"-07:00","2026-10-17T09:00:"+second+"Z"};
    capture->bind(request,win::read_backup_file_configuration(root,owner));const auto result=arc::create_managed_backup(request);check(result.result.ok(),"actual full core plus selected payload publication");check(capture->cleanup(),"exact new index scratch cleaned; leases released");
    check(win::WindowsStorageDaclPolicy(owner).apply(result.archive_path,win::StorageDaclObjectKind::file).succeeded(),"private published archive");
    if(before_retention)before_retention();(void)win::finish_backup_collection(root,owner,collection,result.archive_path,arc::BackupKind::manual,retained);return result.archive_path;
}
arc::ContentIndex index(const ins::Path& path){const auto inspection=arc::inspect_backup(path);check(inspection.result.ok(),"actual archive manifests/CRC valid");return arc::inspect_content_index(path,inspection);}
}
int wmain(int argc,wchar_t** argv)try{
    check(argc==2||(argc==3&&std::wstring(argv[2])==L"--snapshot-only"),"exact initialized fixture required");const auto root=ins::canonical_root(argv[1]);const auto owner=ins::operator_sid();
    check(!ins::elevated()&&root.parent_path()==ins::Path(L"C:\\Program Files")&&root.filename().native().rfind(L"SFX-Section9-Test-20261017-r",0)==0,"bounded own-VM fixture");ins::verify_runtime_installation(root,owner);
    if(argc==3){
        win::OfflineInstallationGuard stopped;check(stopped.acquire(root,owner).succeeded(),"real stopped installation snapshot test");
        auto work=win::FilePayloadDirectory::private_store(root/L"Work",owner);
        const auto name="snapshot-test-"+token()+".sqlite",other="snapshot-other-"+token()+".sqlite";
        write(work,other,"unrelated exact test bytes");write(work,name,"completed snapshot");
        {win::FileBackupSnapshot snapshot(work,name);check(read(work,name)=="completed snapshot","pinned snapshot remains readable by archive consumer");check(!DeleteFileW((work.path()/std::filesystem::u8path(name)).c_str()),"read pin refuses concurrent snapshot replacement");check(snapshot.cleanup()&&snapshot.cleanup(),"normal exact cleanup is idempotent");}
        check(work.absent(name),"normal snapshot removed");write(work,name,"exception snapshot");
        try{win::FileBackupSnapshot snapshot(work,name);throw std::runtime_error("synthetic content bind failure");}catch(const std::runtime_error&){}
        check(work.absent(name),"post-snapshot exception cleanup removes only pinned scratch");write(work,name,"busy snapshot");
        {win::FileBackupSnapshot snapshot(work,name);auto reader=work.read(name);check(!snapshot.cleanup()&&!snapshot.cleanup(),"sharing denial reported, no repeated destructive retry");check(read(work,name)=="busy snapshot","busy snapshot retained on cleanup failure");}
        remove(work,name);check(read(work,other)=="unrelated exact test bytes","unrelated Work file untouched");remove(work,other);
        std::cout<<"PASS "<<checks<<" native exact snapshot cleanup checks\n";return 0;
    }
    auto current=config(root,owner);check(current.areas.size()==3&&std::none_of(current.areas.begin(),current.areas.end(),[](const auto& a){return a.area.backup_contents;}),"fresh all-area payload defaults Off");
    auto library=win::FilePayloadDirectory::area(root,L"Files/General",{L"Files/Uploads",L"Files/UploadToSysop"},{L"Backup",L"Data",L"Work"},owner);check(library.names().empty(),"unused synthetic General library");
    write(library,"stable.txt","same");write(library,"changed.txt","old");write(library,"gone.txt","remove");
    const auto off=save(root,owner);check(index(off).areas.empty(),"Off point has no area content");const auto off_inspect=arc::inspect_backup(off);
    check(std::none_of(off_inspect.files.begin(),off_inspect.files.end(),[](const auto& f){return f.path.rfind("Files/",0)==0||f.path.rfind("FileContents/Objects/",0)==0;}),"in-tree library excluded when Off");
    enable(root,owner,true);const auto baseline=save(root,owner);const auto first=index(baseline);check(first.areas.size()==1&&first.files.size()==3&&std::all_of(first.files.begin(),first.files.end(),[](const auto& f){return f.archive.empty();}),"first selected area complete baseline");
    const auto unchanged=save(root,owner);const auto second=index(unchanged);check(second.files.size()==3&&std::all_of(second.files.begin(),second.files.end(),[&](const auto& f){return f.archive==baseline.filename().u8string();}),"unchanged members reference baseline, no duplicate payload");
    check(std::filesystem::exists(baseline)&&std::filesystem::exists(unchanged)&&!std::filesystem::exists(off),"retention1 retains required baseline and drops independent old Off point");
    remove(library,"changed.txt");write(library,"changed.txt","new");remove(library,"gone.txt");write(library,"new.txt","new arrival");
    const auto increment=save(root,owner);const auto third=index(increment);check(third.files.size()==3&&std::count_if(third.files.begin(),third.files.end(),[](const auto& f){return f.archive.empty();})==2,"new and changed only, removed member absent from current membership");
    check(std::filesystem::exists(baseline)&&!std::filesystem::exists(unchanged),"flattened references prune redundant increment without losing baseline");
    const auto database_before=ins::file_sha256(root/L"Data"/L"SFX.DB");
    {
        win::OfflineInstallationGuard guard;check(guard.acquire(root,owner).succeeded(),"exclusive restore guard");current=win::read_backup_file_configuration(root,owner);auto collection=win::open_backup_collection(root,owner,arc::BackupKind::manual,{},false);
        auto restore=win::FileContentRestore::preview(root,owner,current,collection,baseline,{1});check(restore->items().size()==3&&restore->destinations()[0].second==library.path(),"explicit selected-area destination preview");
        const auto result=restore->execute(current);check(result.completed&&result.restored==1&&result.identical==1&&result.conflicts==1,"restore missing only; existing identical and conflict untouched");
    }
    check(read(library,"changed.txt")=="new"&&read(library,"gone.txt")=="remove"&&read(library,"stable.txt")=="same"&&read(library,"new.txt")=="new arrival","no overwrite or removal during restore");
    check(ins::file_sha256(root/L"Data"/L"SFX.DB")==database_before,"payload-only restore leaves core catalog bytes unchanged");
    remove(library,"gone.txt");
    {
        win::OfflineInstallationGuard guard;check(guard.acquire(root,owner).succeeded(),"second preview guard");current=win::read_backup_file_configuration(root,owner);auto collection=win::open_backup_collection(root,owner,arc::BackupKind::manual,{},false);
        auto restore=win::FileContentRestore::preview(root,owner,current,collection,baseline,{1});write(library,"gone.txt","late conflict");const auto result=restore->execute(current);check(!result.completed&&result.restored==0,"late conflict refuses publication after preview");
    }
    check(read(library,"gone.txt")=="late conflict","late concurrent file preserved");
    {
        auto collection=win::open_backup_collection(root,owner,arc::BackupKind::manual,{},false);current=win::read_backup_file_configuration(root,owner);
        auto capture=win::FileBackupCapture::begin(root,owner,current,collection,arc::BackupKind::manual);write(library,"late.txt","membership changed");arc::ManagedBackupRequest request;request.installation_root=root;bool rejected{};try{capture->bind(request,current);}catch(...){rejected=true;}check(rejected,"membership change after snapshot refuses bind");capture.reset();remove(library,"late.txt");
    }
    {
        const auto descriptor=ins::mutable_descriptor(owner);ins::create_directory(library.path()/L"nested-test",descriptor.get());auto collection=win::open_backup_collection(root,owner,arc::BackupKind::manual,{},false);bool rejected{};
        try{(void)win::FileBackupCapture::begin(root,owner,win::read_backup_file_configuration(root,owner),collection,arc::BackupKind::manual);}catch(...){rejected=true;}check(rejected,"nested/unsupported library entries never silently omitted");check(RemoveDirectoryW((library.path()/L"nested-test").c_str()),"exact newly created empty test directory removed");
    }
    auto collection=win::open_backup_collection(root,owner,arc::BackupKind::manual,{},false);auto archives=win::FilePayloadDirectory::private_store(collection.directory,owner);const auto original_name=baseline.filename().u8string(),saved_name=original_name+".saved";
    {auto file=archives.read(original_name,true);file.rename_to(archives,saved_name);}
    bool broken{};try{const auto members=win::verified_backup_members(root,owner,collection,arc::BackupKind::manual);win::verify_content_dependencies(members);}catch(...){broken=true;}check(broken,"missing exact baseline fails complete-chain verification");
    {auto file=archives.read(saved_name,true);file.rename_to(archives,original_name);}
    {const auto members=win::verified_backup_members(root,owner,collection,arc::BackupKind::manual);win::verify_content_dependencies(members);check(true,"exact preserved baseline restoration recovers chain");}
    bool moved{},retention_refused{};
    try{(void)save(root,owner,1,[&]{auto file=archives.read(original_name,true);file.rename_to(archives,saved_name);moved=true;});}catch(...){retention_refused=true;}
    check(moved&&retention_refused&&std::filesystem::exists(increment),"broken baseline discovered before retention deletes any old point");
    {auto file=archives.read(saved_name,true);file.rename_to(archives,original_name);}
    {const auto members=win::verified_backup_members(root,owner,collection,arc::BackupKind::manual);win::verify_content_dependencies(members);check(true,"failed-retention published point and prior points recover with exact original baseline");}
    bool cancel_refused{};try{(void)win::FileBackupCapture::begin(root,owner,win::read_backup_file_configuration(root,owner),collection,arc::BackupKind::manual,[]{return true;});}catch(...){cancel_refused=true;}check(cancel_refused,"cancelled capture never silently falls back to core-only success");
    enable(root,owner,false);const auto final=save(root,owner);check(index(final).areas.empty()&&!std::filesystem::exists(baseline)&&!std::filesystem::exists(increment),"later Off point releases no-longer-required chain under normal retention");
    check(read(library,"changed.txt")=="new"&&read(library,"new.txt")=="new arrival","backup retention never deletes library payloads");
    std::cout<<"PASS native file-content backup/restore checks="<<checks<<"; fixture and surviving archives retained, physical alternate drive/GUI separate\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL native file backup after "<<checks<<": "<<e.what()<<'\n';return 1;}
