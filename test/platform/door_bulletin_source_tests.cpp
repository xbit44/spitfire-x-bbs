#include "sfx/application/door_bulletin.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstring>
#ifdef _WIN32
#include "sfx/platform/win32/door_bulletin_sources.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include <aclapi.h>
#include <sddl.h>
#include <winioctl.h>
namespace native=sfx::platform::win32;namespace ins=native::installation;
#else
#include "sfx/platform/linux/door_bulletin_sources.hpp"
#include <sys/stat.h>
#include <unistd.h>
namespace native=sfx::platform::linux_host;
#endif
namespace app=sfx::application;namespace fs=std::filesystem;
namespace {
unsigned checks{};void check(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
template<class F>void refused(F f,const char* why){bool rejected{};try{f();}catch(const std::exception&){rejected=true;}check(rejected,why);}
void write(const fs::path& p,const std::string& bytes){std::ofstream f(p,std::ios::binary);f.write(bytes.data(),bytes.size());f.close();check(bool(f),"new fixture output written");
#ifndef _WIN32
check(chmod(p.c_str(),0600)==0,"owned fixture mode");
#endif
}
std::string read(const fs::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
#ifdef _WIN32
std::wstring security(const fs::path& p){auto file=ins::open_object(p,false);PSECURITY_DESCRIPTOR d{};check(GetSecurityInfo(file.get(),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,nullptr,nullptr,nullptr,nullptr,&d)==ERROR_SUCCESS,"fixture ACL captured");ins::Descriptor release(d);LPWSTR sddl{};check(ConvertSecurityDescriptorToStringSecurityDescriptorW(d,SDDL_REVISION_1,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&sddl,nullptr),"complete owner/DACL/control flags captured");ins::Descriptor release_text(sddl);return sddl;}
void junction(const fs::path& path,const fs::path& target){
    const auto descriptor=ins::mutable_descriptor(ins::operator_sid());ins::create_directory(path,descriptor.get());
    auto handle=ins::Handle(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    const std::wstring substitute=L"\\??\\"+target.native(),print=target.native();std::vector<std::uint8_t> data(16+(substitute.size()+1+print.size()+1)*sizeof(wchar_t));
    const DWORD tag=IO_REPARSE_TAG_MOUNT_POINT;const auto length=static_cast<std::uint16_t>(data.size()-8),sub_length=static_cast<std::uint16_t>(substitute.size()*2),print_offset=static_cast<std::uint16_t>(sub_length+2),print_length=static_cast<std::uint16_t>(print.size()*2);
    std::memcpy(data.data(),&tag,4);std::memcpy(data.data()+4,&length,2);std::memcpy(data.data()+10,&sub_length,2);std::memcpy(data.data()+12,&print_offset,2);std::memcpy(data.data()+14,&print_length,2);std::memcpy(data.data()+16,substitute.c_str(),(substitute.size()+1)*2);std::memcpy(data.data()+16+print_offset,print.c_str(),(print.size()+1)*2);DWORD returned{};
    check(handle&&handle.get()!=INVALID_HANDLE_VALUE&&DeviceIoControl(handle.get(),FSCTL_SET_REPARSE_POINT,data.data(),static_cast<DWORD>(data.size()),nullptr,0,&returned,nullptr),"actual isolated directory junction created without changing Windows privileges");
}
#endif
}
int main(int argc,char** argv){try{
    check(argc==2,"fresh fixture root required");const auto root=fs::absolute(argv[1]);check(!fs::exists(root),"preserve existing fixtures");fs::create_directories(root.parent_path());
#ifdef _WIN32
    const auto owner=ins::operator_sid();const auto descriptor=ins::mutable_descriptor(owner);const auto mkdir=[&](const fs::path& p){ins::create_directory(p,descriptor.get());};
#else
    check(geteuid()!=0,"run guarded native qualification as existing nonroot account");const auto mkdir=[&](const fs::path& p){check(::mkdir(p.c_str(),0700)==0,"fresh owned fixture directory");};
#endif
    for(const auto& p:{root,root/"Doors",root/"Doors"/"Game",root/"Doors"/"Game"/"Scores",root/"External"})mkdir(p);
    const auto game=root/"Doors"/"Game",score=game/"Scores"/"SCORE.TXT";const std::string bytes="Top scores\r\nAlice 100\r\n";write(score,bytes);write(root/"External"/"OUT.TXT","external approved source\r\n");
    app::DoorBulletinSource source;source.definition.id={999};source.definition.title="Scores";source.definition.source=app::DoorBulletinSourceKind::door_file;source.definition.door=256;source.definition.relative_path="Scores/SCORE.TXT";source.definition.format=app::GraphicsMode::ascii;source.definition.revision=1;source.work_directory="Doors/Game";source.door_available=true;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA before{},after{};check(GetFileAttributesExW(score.c_str(),GetFileExInfoStandard,&before),"initial content metadata");auto acl=security(score);const auto capture=[&](const app::DoorBulletinSource& s){return native::read_door_bulletin_source(root,owner,s);};
#else
    struct stat before{},after{};check(stat(score.c_str(),&before)==0,"initial content metadata");const auto capture=[&](const app::DoorBulletinSource& s){return native::read_door_bulletin_source(root,s);};
#endif
    const auto captured=capture(source);check(captured.code==app::TrustedDisplayAssetReadCode::available&&captured.bytes==bytes,"managed inherited/owned Door output accepted independently of extension");check(read(score)==bytes,"read-only capture preserves source bytes");
#ifdef _WIN32
    check(GetFileAttributesExW(score.c_str(),GetFileExInfoStandard,&after)&&CompareFileTime(&before.ftLastWriteTime,&after.ftLastWriteTime)==0&&before.nFileSizeHigh==after.nFileSizeHigh&&before.nFileSizeLow==after.nFileSizeLow,"capture preserves mtime and size");check(security(score)==acl,"capture preserves owner and complete inherited DACL/flags");
#else
    check(stat(score.c_str(),&after)==0&&before.st_size==after.st_size&&before.st_mtim.tv_sec==after.st_mtim.tv_sec&&before.st_mtim.tv_nsec==after.st_mtim.tv_nsec&&before.st_mode==after.st_mode&&before.st_uid==after.st_uid&&before.st_gid==after.st_gid,"capture preserves mtime/size/owner/permissions");
#endif
    for(const auto relative:{"../OUT.TXT","/OUT.TXT","C:/OUT.TXT","Scores/../OUT.TXT","Scores/x:stream"}){auto bad=source;bad.definition.relative_path=relative;refused([&]{capture(bad);},"absolute/traversal/ADS source refused");}
    auto missing=source;missing.definition.relative_path="MISSING.TXT";refused([&]{capture(missing);},"missing file rejected");auto absent=source;absent.door_available=false;refused([&]{capture(absent);},"removed Door is unavailable");
    write(game/"HUGE.TXT",std::string(app::kMaximumTrustedDisplaySourceBytes+1,'x'));auto huge=source;huge.definition.relative_path="HUGE.TXT";refused([&]{capture(huge);},"oversized source refused before exposure");
    write(game/"ORIGINAL.TXT","one-link only");
#ifdef _WIN32
    check(CreateHardLinkW((game/"HARD.TXT").c_str(),(game/"ORIGINAL.TXT").c_str(),nullptr),"new hardlink fixture");
#else
    check(link((game/"ORIGINAL.TXT").c_str(),(game/"HARD.TXT").c_str())==0,"new hardlink fixture");
#endif
    auto hard=source;hard.definition.relative_path="HARD.TXT";refused([&]{capture(hard);},"hardlink source refused");
    write(game/"UNSAFE.TXT","not private");auto unsafe=source;unsafe.definition.relative_path="UNSAFE.TXT";
#ifdef _WIN32
    PSECURITY_DESCRIPTOR loose{};const auto sddl=L"D:P(A;;FA;;;"+owner+L")(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;BU)";check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&loose,nullptr),"loose fixture descriptor");ins::Descriptor loose_owner(loose);check(SetFileSecurityW((game/"UNSAFE.TXT").c_str(),DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,loose),"loose fixture only ACL set");auto unsafe_acl=security(game/"UNSAFE.TXT");
#else
    check(chmod((game/"UNSAFE.TXT").c_str(),0666)==0,"loose fixture only permissions");
#endif
    refused([&]{capture(unsafe);},"unsafe source permissions refused without repair");
#ifdef _WIN32
    check(security(game/"UNSAFE.TXT")==unsafe_acl,"refusal does not repair ACL");
    {auto writer=ins::open_object(score,false,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE);refused([&]{capture(source);},"already open writer prevents snapshot capture");}
    bool denied{};const auto locked=native::read_door_bulletin_source(root,owner,source,[&]{auto writer=CreateFileW(score.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);denied=writer==INVALID_HANDLE_VALUE;if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);});check(denied&&locked.bytes==bytes,"held source lease denies concurrent write");
    bool ancestor_denied{};native::read_door_bulletin_source(root,owner,source,[&]{ancestor_denied=!MoveFileW(game.c_str(),(root/"MovedGame").c_str());});check(ancestor_denied,"all held ancestors deny rename during capture");
    const bool file_link=CreateSymbolicLinkW((game/"LINK.TXT").c_str(),(root/"External"/"OUT.TXT").c_str(),2)!=FALSE;
    if(!file_link)std::cout<<"NOT TESTED Windows file symbolic link: fixture creation refused, native error "<<GetLastError()<<"; no privilege/security changes.\n";
    junction(game/"Escape",root/"External");
#else
    struct stat unchanged{};check(stat((game/"UNSAFE.TXT").c_str(),&unchanged)==0&&(unchanged.st_mode&0777)==0666,"refusal does not repair permissions");
    refused([&]{native::read_door_bulletin_source(root,source,{},[&]{write(score,"changed source\r\n");});},"changing source rejected between reads");write(score,bytes);
    check(symlink((root/"External"/"OUT.TXT").c_str(),(game/"LINK.TXT").c_str())==0&&symlink((root/"External").c_str(),(game/"Escape").c_str())==0,"isolated symbolic-link fixtures");
#endif
    for(const auto leaf:{"LINK.TXT","Escape/OUT.TXT"}){
#ifdef _WIN32
        if(std::string_view(leaf)=="LINK.TXT"&&!file_link)continue;
#endif
        auto escaped=source;escaped.definition.relative_path=leaf;refused([&]{capture(escaped);},"file/ancestor reparse or symlink escape refused");}
    check(read(root/"External"/"OUT.TXT")=="external approved source\r\n","escape destination unchanged");
    auto external=source;external.work_directory=(root/"External").u8string();external.definition.relative_path="OUT.TXT";check(capture(external).bytes=="external approved source\r\n","current approved external work directory remains usable");
    auto default_work=source;default_work.work_directory.clear();default_work.definition.relative_path="ROOT.TXT";write(root/"ROOT.TXT","default approved working directory\r\n");check(capture(default_work).bytes=="default approved working directory\r\n","empty configured work directory means installation root");
#ifndef _WIN32
    refused([&]{native::read_door_bulletin_source(root,source,{},[&]{fs::rename(game,root/"MovedGame");mkdir(game);mkdir(game/"Scores");write(game/"Scores"/"SCORE.TXT","replacement ancestor\r\n");});},"renamed/replaced approved ancestor is rejected at final revalidation");
#endif
    std::cout<<"PASS "<<checks<<" protected Door Bulletin source checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
