// Actual isolated ShellLink files, never the Windows Desktop or Start Menu.
#include "sfx/platform/win32/installation_security.hpp"
#include "../../src/apps/product_version.h"
#include <shlobj.h>
#include <propsys.h>
#include <initguid.h>
#include <propkey.h>
#include <array>
#include <algorithm>
#include <iostream>
#include <stdexcept>
namespace {
namespace ins=sfx::platform::win32::installation;
using namespace ins;using Path=std::filesystem::path;
unsigned checks{};bool inject_race{};
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
bool absent(const Path& path){if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES)return false;const auto error=GetLastError();require(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND,"test_path_unavailable");return true;}
void unmarked(const Path& path,const Path& target,const wchar_t* description=SFX_PRODUCT_CONTROL_PANEL_DISPLAY_VERSION_WIDE){
    IShellLinkW* link{};require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_IShellLinkW,reinterpret_cast<void**>(&link))),"test_link_create");
    link->SetPath(target.c_str());link->SetWorkingDirectory(target.parent_path().c_str());link->SetDescription(description);
    IPersistFile* file{};const auto queried=link->QueryInterface(IID_IPersistFile,reinterpret_cast<void**>(&file));
    require(SUCCEEDED(queried),"test_link_persist");const auto saved=file->Save(path.c_str(),TRUE);file->Release();link->Release();require(SUCCEEDED(saved),"test_link_save");
}
BOOL publish_with_race(LPCWSTR source,LPCWSTR destination,DWORD flags){
    if(inject_race){inject_race=false;const auto sd=mutable_descriptor(operator_sid());create_file(destination,{'u','n','o','w','n','e','d'},sd.get());}
    return MoveFileExW(source,destination,flags);
}
#define MoveFileExW publish_with_race
#include "../../src/apps/setup/desktop_shortcuts.inc"
#undef MoveFileExW
}
int main(int argc,char** argv){try{
    check(argc==2,"fresh isolated shortcut fixture root required");
    const Path root=std::filesystem::absolute(argv[1]);check(absent(root),"preserve prior shortcut fixtures");
    const auto sd=mutable_descriptor(operator_sid());create_directory(root,sd.get());create_directory(root/L"Bin",sd.get());
    const auto target=root/L"Bin"/L"SFXControlPanel.exe";create_file(target,{'f','i','x','t','u','r','e'},sd.get());
    const std::wstring identity=L"0123456789abcdef0123456789abcdef",other=L"fedcba9876543210fedcba9876543210";
    const std::wstring legacy=L"Spitfire-X BBS (Fixture-01234567).lnk";
    check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"ShellLink COM initializes");
    const auto folder=[&](const wchar_t* name){const auto p=root/name;create_directory(p,sd.get());return p;};
    const auto fresh=folder(L"Fresh");desktop_shortcut_changes(fresh,target,legacy,identity,false,true);
    check(owned_desktop_shortcut(fresh/desktop_caption(1),target,identity),"fresh caption is plain and protected by non-caption identity");
    const auto first_hash=file_sha256(fresh/desktop_caption(1));desktop_shortcut_changes(fresh,target,legacy,identity,false,true);
    check(first_hash==file_sha256(fresh/desktop_caption(1)),"maintenance reuses owned plain link idempotently");
    const auto upgrade=folder(L"Upgrade");unmarked(upgrade/legacy,target,L"Spitfire-X BBS v1.0.0-beta.6");
    desktop_shortcut_changes(upgrade,target,legacy,identity,false,true);
    check(absent(upgrade/legacy)&&owned_desktop_shortcut(upgrade/desktop_caption(1),target,identity),"verified legacy caption retires after new owned publication");
    const auto unowned=folder(L"Unowned");unmarked(unowned/desktop_caption(1),target);const auto unowned_hash=file_sha256(unowned/desktop_caption(1));
    desktop_shortcut_changes(unowned,target,legacy,identity,false,true);
    check(unowned_hash==file_sha256(unowned/desktop_caption(1))&&absent(unowned/desktop_caption(2)),"manual same-target link reused without creating a duplicate");
    create_desktop_shortcut(unowned/desktop_caption(2),target,identity);
    const auto stale=unowned/(L"Spitfire-X BBS ("+root.filename().wstring()+L"-deadbeef).lnk");
    unmarked(stale,target);
    desktop_shortcut_changes(unowned,target,legacy,identity,false,true);
    check(absent(unowned/desktop_caption(2))&&absent(stale)&&unowned_hash==file_sha256(unowned/desktop_caption(1)),
        "maintenance retires the owned numeric and older Setup links while keeping the manual link");
    const auto elsewhere=folder(L"OtherInstall");create_desktop_shortcut(elsewhere/desktop_caption(1),target,other);const auto other_hash=file_sha256(elsewhere/desktop_caption(1));
    desktop_shortcut_changes(elsewhere,target,legacy,identity,false,true);
    check(other_hash==file_sha256(elsewhere/desktop_caption(1))&&owned_desktop_shortcut(elsewhere/desktop_caption(2),target,identity),"another installation marker is preserved even for same executable target");
    const auto malformed=folder(L"Malformed");create_file(malformed/desktop_caption(1),{'b','a','d'},sd.get());unmarked(malformed/legacy,target,L"Owner's shortcut");
    desktop_shortcut_changes(malformed,target,legacy,identity,false,true);
    check(read_file(malformed/desktop_caption(1),3)==std::vector<std::uint8_t>({'b','a','d'})&&!absent(malformed/legacy),"malformed plain link and uncertain legacy ownership remain untouched");
    const auto raced=folder(L"Race");unmarked(raced/legacy,target);const auto old_hash=file_sha256(raced/legacy);inject_race=true;bool refused{};
    try{desktop_shortcut_changes(raced,target,legacy,identity,false,true);}catch(const std::exception&){refused=true;}
    check(refused&&read_file(raced/desktop_caption(1),7)==std::vector<std::uint8_t>({'u','n','o','w','n','e','d'})&&old_hash==file_sha256(raced/legacy),"lost publication race preserves both new unowned destination and verified legacy link");
    desktop_shortcut_changes(unowned,target,legacy,identity,true,false);
    check(absent(unowned/desktop_caption(2))&&unowned_hash==file_sha256(unowned/desktop_caption(1)),"uninstall removes only marked owned links and preserves manual same-target caption");
    CoUninitialize();std::cout<<"PASS "<<checks<<" isolated actual ShellLink caption/ownership/collision checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
