#include "sfx/platform/win32/backup_collections.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include <iostream>
namespace win=sfx::platform::win32;
namespace ins=win::installation;
namespace arc=sfx::archive;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;ins::require(ok,why);}
arc::BackupInspection fresh(){
    arc::BackupInspection b;b.metadata.backup_type="setup-upgrade";b.metadata.source_generation="1";
    b.files={{"Config/JOKER.DAT",0,{}},{"Config/otherbbs.txt",0,{}},{"Display/main.ans",100,{}}};return b;
}
void content(){
    const auto base=fresh();check(win::pre_setup_backup_contents(base,2),"pre-setup metadata recognized");
    for(const auto* path:{"Data/SFX.DB","Config/SFX.conf","Config/SFXSecurity.conf","Data/unknown","FTN/unknown","Logs/old.log","Node/identity","Files/content","SFX-FILE-CONTENTS.txt"}){
        auto b=base;b.files.push_back({path,0,{}});check(!win::pre_setup_backup_contents(b,2),"any persisted/unknown role refuses exemption even empty");
    }
    for(const auto* generation:{"","0","2","3","01","-1","1x","18446744073709551616"}){
        auto b=base;b.metadata.source_generation=generation;check(!win::pre_setup_backup_contents(b,2),"noncanonical/nonprior generation refused");
    }
    for(const auto* type:{"manual","scheduled","pre-restore",""}){
        auto b=base;b.metadata.backup_type=type;check(!win::pre_setup_backup_contents(b,2),"only Setup pool recognized");
    }
    for(unsigned i=0;i<2;++i){auto b=base;b.files[i].size=1;check(!win::pre_setup_backup_contents(b,2),"nonempty Config stock is persistent");}
    auto b=base;b.files.erase(b.files.begin());check(!win::pre_setup_backup_contents(b,2),"missing stock metadata refused");
    b=base;b.files.push_back(b.files[0]);check(!win::pre_setup_backup_contents(b,2),"duplicate metadata refused");
    b=base;b.result.error=arc::Error::manifest_invalid;check(!win::pre_setup_backup_contents(b,2),"unverified inventory refused");
}
void native(){
    const auto root=ins::canonical_root(L"C:\\Program Files\\SFX-Section14-Test-20261030-r4");
    check(!ins::elevated(),"ordinary native qualification");const auto owner=ins::operator_sid();
    ins::verify_runtime_installation(root,owner);
    check(!std::filesystem::exists(root/L"Config"/L"SFX.conf")&&!std::filesystem::exists(root/L"Data"/L"SFX.DB"),"unchanged pre-bootstrap fixture");
    win::OfflineInstallationGuard stopped;check(stopped.acquire(root,owner).succeeded(),"hold stopped Runtime guard");
    const auto archive=root/L"Backup"/L"Upgrades"/L"SFX-setup-backup-20261030-045622.zip";
    const auto marker=ins::Path(archive.native()+L".sfx-owned");
    const auto before=ins::file_sha256(archive),before_marker=ins::file_sha256(marker);
    check(before=="612FEC5FEAC73544D28459A793E8EE7EB63E4745549B4D864A52F01D8A79C101"&&before_marker=="B6835BB08FA2BCC2AE288B31D59B85DFB4DE485AE999CA1F33DEF7461D46EC85","exact retained owner backup");
    auto inspect=[&](bool expected){const auto r=win::alpha_installation_has_persistent_state(root,owner);check(r&&r.value&&*r.value==expected,"actual startup classification");};
    inspect(false);
    {
        std::vector<ins::Handle> leases;check(win::verified_pre_setup_upgrade_backups(root,owner,leases)&&!leases.empty(),"actual entire archive/marker native verification");
        const HANDLE write=CreateFileW(archive.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        const auto error=GetLastError();if(write!=INVALID_HANDLE_VALUE)CloseHandle(write);
        check(write==INVALID_HANDLE_VALUE&&error==ERROR_SHARING_VIOLATION,"successful proof retains deny-write archive lease");
    }
    {
        auto unavailable=ins::open_object(archive,false,GENERIC_READ,0);
        inspect(true); // same archive cannot be read; no bypass or mutation
    }
    inspect(false);
    // Only newly created exact controls are retired, by their owned handle.
    // No Runtime is launched and no SQLite/bootstrap is performed in this test.
    for(const auto& path:{root/L"Data"/L"SFX.DB",root/L"Backup"/L"Upgrades"/L"sfx-pre-setup-negative-control.tmp"}){
        check(!std::filesystem::exists(path),"negative control never overwrites an object");
        auto descriptor=ins::mutable_descriptor(owner);const std::vector<std::uint8_t> bytes{'t','e','s','t'};
        ins::create_file(path,bytes,descriptor.get());
        auto pin=ins::open_object(path,false,GENERIC_READ|DELETE,FILE_SHARE_READ);
        inspect(true);
        LARGE_INTEGER size{};std::uint8_t actual[4]{};DWORD got{};
        check(GetFileSizeEx(pin.get(),&size)&&size.QuadPart==4&&
            ReadFile(pin.get(),actual,4,&got,nullptr)&&got==4&&
            std::vector<std::uint8_t>(actual,actual+4)==bytes,"only exact synthetic control retained on same owned handle");
        FILE_DISPOSITION_INFO disposition{TRUE};check(SetFileInformationByHandle(pin.get(),FileDispositionInfo,&disposition,sizeof(disposition))!=FALSE,"retire only owned synthetic control by handle");pin.reset();
        check(!std::filesystem::exists(path),"owned negative control settled");inspect(false);
    }
    check(ins::file_sha256(archive)==before&&ins::file_sha256(marker)==before_marker,"original backup and ownership bytes untouched");
    check(!std::filesystem::exists(root/L"Data"/L"SFX.DB")&&!std::filesystem::exists(root/L"Config"/L"SFX.conf"),"pre-bootstrap state preserved");
}
}
int main(int argc,char** argv)try{
    check(argc==1||(argc==2&&std::string(argv[1])=="--native"),"exact test mode");content();if(argc==2)native();
    std::cout<<"PASS "<<checks<<" pre-setup backup classification checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}
