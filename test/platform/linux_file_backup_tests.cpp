#include "sfx/platform/linux/file_backup.hpp"
#include "sfx/platform/linux/portable_file_bindings.hpp"
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace native=sfx::platform::linux_host;
namespace app=sfx::application;
namespace arc=sfx::archive;
namespace fs=std::filesystem;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F> bool refused(F fn){try{fn();return false;}catch(const std::exception&){return true;}}
void directory(const fs::path& path){check(mkdir(path.c_str(),0700)==0,"new private test directory");}
void bytes(const native::FilePayloadDirectory& dir,const std::string& name,const std::string& text){
    auto file=dir.create(name,std::max<std::size_t>(1,text.size()));file.append(reinterpret_cast<const std::uint8_t*>(text.data()),text.size());(void)file.identity();
}
std::string content(const native::FilePayloadDirectory& dir,const std::string& name){auto file=dir.read(name);auto data=file.read(0,65536);return {data.begin(),data.end()};}
arc::Timestamp stamp(unsigned n){auto value=arc::current_timestamp();value.local="2026-11-09T12:00:"+(n<10?std::string("0"):std::string{})+std::to_string(n);value.filename_component="20261109-1200"+(n<10?std::string("0"):std::string{})+std::to_string(n);return value;}
fs::path point(const fs::path& root,std::shared_ptr<native::LinuxFilePayloadBindings> bindings,const native::BackupCollection& collection,const app::FileConfiguration& config,unsigned n,std::uint32_t retained=10){
    auto captured=native::FileBackupCapture::begin(root,bindings,config,collection,collection.kind);
    arc::ManagedBackupRequest request;request.installation_root=root;request.destination_directory=collection.directory;request.created=stamp(n);
    request.physical_layout=arc::PhysicalLayout::linux_lowercase_v1;
    request.transaction_id=std::string(31,'A')+"0123456789ABCDEF"[n%16];request.source_generation="1";
    request.kind=collection.kind;
    captured->bind(request,config);const auto result=arc::create_managed_backup(request);
    if(!result.result.ok())throw std::runtime_error("actual managed archive: "+result.result.detail);
    check(result.result.ok(),"actual managed full-core backup published");
    native::secure_new_backup_archive(collection,result.archive_path);
    struct stat protection{};check(lstat(result.archive_path.c_str(),&protection)==0&&(protection.st_mode&077)==0,"new archive protected before collection use");
    const auto inspected=arc::inspect_backup(result.archive_path);check(inspected.result.ok(),"published archive reopens");
    (void)arc::inspect_content_index(result.archive_path,inspected);
    check(captured->cleanup(),"exact successful scratch cleanup");
    const auto pruned=native::finish_backup_collection(root,bindings,collection,result.archive_path,collection.kind,retained);
    check(pruned<=n,"bounded native retention result");return result.archive_path;
}
}
int main(int argc,char** argv){try{
    check(argc==2&&geteuid()!=0,"prepared private board, normal user");const fs::path root=argv[1];
    auto bindings=native::LinuxFilePayloadBindings::open(root);check(bool(bindings),"authenticated board bindings");
    if(!fs::exists(root/"backup"))directory(root/"backup");
    check(bool(native::open_owned_directory(root/"files/general")),"prepared private General area");
    auto area=native::FilePayloadDirectory::area(root,"files/general",{}, {"config","data","backup","work"},bindings);
    bytes(area,"first.txt","native backup payload one\n");bytes(area,"second.txt","native backup payload two\n");
    app::FileConfiguration config;config.policy.catalog_revision=1;app::FileAreaConfiguration selected;
    selected.area.id.value=1;selected.area.revision=1;selected.area.name="General";selected.area.backup_contents=true;selected.storage_path="files/general";config.areas.push_back(selected);
    auto pool=native::open_backup_collection(root,bindings,arc::BackupKind::manual);
    check(pool.directory==root/"backup/scheduled","ordinary default matches accepted layout");
    check(refused([&]{(void)native::open_backup_collection(root,bindings,arc::BackupKind::manual,"C:\\Saved Windows Folder");}),"foreign saved destination refused without fallback");
    check(refused([&]{(void)native::open_backup_collection(root,bindings,arc::BackupKind::manual,(root/"data/copied").string());}),"live role destination refused");
    check(refused([&]{(void)native::open_backup_collection(root,bindings,arc::BackupKind::manual,root.parent_path().string());}),"installation ancestor destination refused");
    check(refused([&]{(void)native::open_backup_collection(root,bindings,arc::BackupKind::manual,(root/"backup/missing/leaf").string());}),"missing external parents never fabricated");
    const auto external=root.parent_path()/"custom-backups";auto custom=native::open_backup_collection(root,bindings,arc::BackupKind::manual,external.string());
    check(custom.directory==external,"custom absolute private destination honored");
    const auto first=point(root,bindings,custom,config,1);
    {
        auto members=native::verified_backup_members(root,bindings,custom,arc::BackupKind::manual);
        check(members.size()==1&&members[0].contents.files.size()==2,"owned backup verifies full file content index");
        check(members[0].contents.files[0].archive.empty()&&members[0].contents.files[1].archive.empty(),"first point contains both payloads");
    }
    const auto second=point(root,bindings,custom,config,2,1);
    {
        auto members=native::verified_backup_members(root,bindings,custom,arc::BackupKind::manual);
        check(members.size()==2&&fs::exists(first)&&fs::exists(second),"retained-one increment keeps its required baseline");
        native::verify_content_dependencies(members);check(members.back().contents.files[0].archive==first.filename().string(),"unchanged payload references exact earlier archive");
    }
    // Restore only an absent payload; a conflicting existing file is retained.
    {auto file=area.read("first.txt",true);file.remove_exact();}
    {auto file=area.read("second.txt",true);file.remove_exact();}bytes(area,"second.txt","owner changed contents\n");
    auto restore=native::FileContentRestore::preview(root,bindings,config,custom,second,{1});
    check(restore->items().size()==2&&restore->items()[0].state==native::FileContentRestoreItem::State::missing&&restore->items()[1].state==native::FileContentRestoreItem::State::conflict,"restore preview classifies missing and conflict");
    const auto restored=restore->execute(config);check(restored.completed&&restored.restored==1&&restored.conflicts==1,"real chained restore writes only absent payload");restore.reset();
    check(content(area,"first.txt")=="native backup payload one\n"&&content(area,"second.txt")=="owner changed contents\n","restored bytes exact and owner conflict untouched");
    auto identical=native::FileContentRestore::preview(root,bindings,config,custom,second,{1});
    check(identical->items()[0].state==native::FileContentRestoreItem::State::identical,"same restored bytes classified identical");identical.reset();
    // Invalid/foreign/unrecorded media is never a deletion permit.
    bytes(*custom.custody,"unknown.zip","unrecognized owner material");
    config.areas[0].area.backup_contents=false;++config.areas[0].area.revision;++config.policy.catalog_revision;
    const auto third=point(root,bindings,custom,config,3,1);
    check(!fs::exists(first)&&!fs::exists(second)&&fs::exists(third)&&content(*custom.custody,"unknown.zip")=="unrecognized owner material","independent all-Off point prunes only proved owned superseded chain");
    auto members=native::verified_backup_members(root,bindings,custom,arc::BackupKind::manual);
    check(members.size()==1&&members[0].contents.files.empty(),"all-Off archive explicitly has empty file index");members.clear();
    config.areas[0].area.backup_contents=true;++config.areas[0].area.revision;++config.policy.catalog_revision;
    auto changed=native::FileBackupCapture::begin(root,bindings,config,custom,arc::BackupKind::manual);
    bytes(area,"new.txt","new member after capture");arc::ManagedBackupRequest invalid;invalid.installation_root=root;
    check(refused([&]{changed->bind(invalid,config);}),"membership change after capture refuses incomplete backup");changed.reset();
    check(symlink("first.txt",(root/"files/general/link.txt").c_str())==0,"isolated source-link injection");
    check(refused([&]{(void)native::FileBackupCapture::begin(root,bindings,config,custom,arc::BackupKind::manual);}),"selected area link is refused, never silently skipped");
    check(unlink((root/"files/general/link.txt").c_str())==0,"remove only owned injection");
    check(refused([&]{(void)native::FileBackupCapture::begin(root,bindings,config,custom,arc::BackupKind::manual,[]{return true;});}),"cancelled capture leaves prior archive and source intact");
    check(fs::exists(third)&&content(area,"new.txt")=="new member after capture","failure preserves old archive and new source");
    const auto shared_folder=(root/"backup/upgrades").string();
    auto ordinary=native::open_backup_collection(root,bindings,arc::BackupKind::manual,shared_folder);
    const auto ordinary_point=point(root,bindings,ordinary,config,4);
    {auto preview=native::FileContentRestore::preview(root,bindings,config,ordinary,ordinary_point,{1});
        const auto result=preview->execute(config);check(result.completed&&result.identical==3,"ordinary custom Upgrades destination restores by ownership pool, not folder name");}
    auto upgrade=native::open_backup_collection(root,bindings,arc::BackupKind::setup_upgrade);
    const auto upgrade_point=point(root,bindings,upgrade,config,5);
    {auto preview=native::FileContentRestore::preview(root,bindings,config,upgrade,upgrade_point,{1});
        const auto result=preview->execute(config);check(result.completed&&result.identical==3,"genuine Setup pool restores its own point");}
    check(native::verified_backup_members(root,bindings,ordinary,arc::BackupKind::manual).size()==1&&native::verified_backup_members(root,bindings,upgrade,arc::BackupKind::setup_upgrade).size()==1,"same folder keeps ordinary and Setup provenance pools distinct");
    check(refused([&]{(void)native::FileContentRestore::preview(root,bindings,config,ordinary,upgrade_point,{1});}),"ordinary pool cannot borrow Setup restore authority");
    const auto conflict=root/"files/general/second.txt";
    check(link(conflict.c_str(),(root/"files/general/alias.txt").c_str())==0,"owned hardlink injection for restore preview");
    check(refused([&]{(void)native::FileContentRestore::preview(root,bindings,config,ordinary,ordinary_point,{1});}),"read-only conflict inspection refuses multiply linked payload");
    check(unlink((root/"files/general/alias.txt").c_str())==0,"remove only owned hardlink injection");
    check(chmod(conflict.c_str(),0660)==0,"owned shared-writable injection for restore preview");
    check(refused([&]{(void)native::FileContentRestore::preview(root,bindings,config,ordinary,ordinary_point,{1});}),"read-only conflict inspection refuses shared-writable payload");
    check(chmod(conflict.c_str(),0600)==0,"restore owned payload protection");
    {auto file=area.read("first.txt",true);file.remove_exact();}
    auto stale=native::FileContentRestore::preview(root,bindings,config,ordinary,ordinary_point,{1});
    {std::ofstream changed_file(conflict,std::ios::binary|std::ios::trunc);changed_file<<"Owner changed after preview\n";}
    const auto stale_result=stale->execute(config);
    check(!stale_result.completed&&stale_result.restored==0&&!fs::exists(root/"files/general/first.txt"),"changed existing destination invalidates preview before any missing publication");stale.reset();
    auto fresh=native::FileContentRestore::preview(root,bindings,config,ordinary,ordinary_point,{1});
    const auto fresh_result=fresh->execute(config);
    check(fresh_result.completed&&fresh_result.restored==1&&fresh_result.identical==1&&fresh_result.conflicts==1,"fresh physical inspection classifies changed bytes without overwriting conflict");
    {std::ifstream retained(conflict,std::ios::binary);std::string value((std::istreambuf_iterator<char>(retained)),{});
        check(value=="Owner changed after preview\n","fresh conflict inspection preserves exact changed payload");}
    std::cout<<"PASS "<<checks<<" native archive ownership, incremental dependencies, retention and content restore checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
