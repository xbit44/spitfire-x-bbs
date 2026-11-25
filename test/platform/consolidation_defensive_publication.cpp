// Defensive compatibility only. Ordinary publication of newly created dummy
// files under build. No reparse/alias creation, cross-process manipulation,
// security changes, elevation, network, production code, or installed paths.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs=std::filesystem;
namespace {
unsigned checks=0;
void check(bool value,const char* label) {
    ++checks; if(!value) throw std::runtime_error(std::string(label)+" (Win32 "+std::to_string(GetLastError())+")");
}
struct Close {
    void operator()(void* value) const {
        if(value&&value!=INVALID_HANDLE_VALUE) {
            DWORD flags=0;
            if(GetHandleInformation(value,&flags)&&(flags&HANDLE_FLAG_PROTECT_FROM_CLOSE))
                SetHandleInformation(value,HANDLE_FLAG_PROTECT_FROM_CLOSE,0);
            CloseHandle(value);
        }
    }
};
using Handle=std::unique_ptr<void,Close>;

void directory(const fs::path& path) { check(CreateDirectoryW(path.c_str(),nullptr)!=FALSE,"create fresh fixture directory"); }
void create_text(const fs::path& path,const std::string& text) {
    Handle h(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    check(h.get()!=INVALID_HANDLE_VALUE,"create only a new owned file"); DWORD done=0;
    check(WriteFile(h.get(),text.data(),static_cast<DWORD>(text.size()),&done,nullptr)&&done==text.size()&&FlushFileBuffers(h.get()),
        "write known disposable content");
}
void verify_text(const fs::path& path,const std::string& expected) {
    Handle h(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    BY_HANDLE_FILE_INFORMATION info{}; char buffer[128]{}; DWORD done=0;
    check(h.get()!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(h.get(),&info)&&
        !(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&info.nNumberOfLinks==1,
        "read only the single-link owned regular file");
    check(ReadFile(h.get(),buffer,sizeof(buffer),&done,nullptr)&&done==expected.size()&&
        !std::memcmp(buffer,expected.data(),expected.size()),"exact known content preserved/published");
}
Handle open_directory(const fs::path& path,bool lifetime) {
    Handle h(CreateFileW(path.c_str(),GENERIC_READ,lifetime?FILE_SHARE_READ:FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    check(h.get()!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(h.get(),&info)&&
        (info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),
        "open and inspect real fixture directory without following final reparse");
    if(lifetime) check(SetHandleInformation(h.get(),HANDLE_FLAG_PROTECT_FROM_CLOSE,HANDLE_FLAG_PROTECT_FROM_CLOSE)!=FALSE,
        "retain protected lifetime handle");
    return h;
}

enum class Method { move, absolute_handle, relative_handle, replace };
const char* name(Method method) {
    switch(method) {
    case Method::move:return "MoveFileEx";
    case Method::absolute_handle:return "FileRenameInfo-absolute";
    case Method::relative_handle:return "FileRenameInfo-relative";
    case Method::replace:return "ReplaceFile";
    }
    return "invalid";
}
bool publish(Method method,const fs::path& source,const fs::path& target,HANDLE parent) {
    if(method==Method::move) return MoveFileExW(source.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
    if(method==Method::replace) return ReplaceFileW(target.c_str(),source.c_str(),nullptr,0,nullptr,nullptr)!=FALSE;
    Handle file(CreateFileW(source.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(file.get()==INVALID_HANDLE_VALUE) return false;
    const auto text=method==Method::relative_handle?target.filename().native():target.native();
    const auto bytes=static_cast<DWORD>(text.size()*sizeof(wchar_t));
    std::vector<BYTE> storage(offsetof(FILE_RENAME_INFO,FileName)+bytes+sizeof(wchar_t));
    auto* info=reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    info->ReplaceIfExists=TRUE; info->RootDirectory=method==Method::relative_handle?parent:nullptr;
    info->FileNameLength=bytes; std::memcpy(info->FileName,text.c_str(),bytes+sizeof(wchar_t));
    return SetFileInformationByHandle(file.get(),FileRenameInfo,info,static_cast<DWORD>(storage.size()))!=FALSE;
}

bool one_case(const fs::path& root,Method method,bool existing,bool lifetime,unsigned index) {
    const auto path=root/(L"case-"+std::to_wstring(index)); directory(path);
    const auto source=path/L"owned-pending.txt",target=path/L"owned-published.txt";
    create_text(source,"new dummy content"); if(existing) create_text(target,"old dummy content");
    auto pin=open_directory(path,lifetime);
    BY_HANDLE_FILE_INFORMATION before{}; check(GetFileInformationByHandle(pin.get(),&before)!=FALSE,"pin identity before publication");
    const bool result=publish(method,source,target,pin.get()); const DWORD error=result?ERROR_SUCCESS:GetLastError();
    std::cout<<name(method)<<" existing="<<existing<<" lifetime="<<lifetime<<" result="<<result<<" error="<<error<<'\n';
    BY_HANDLE_FILE_INFORMATION after{}; DWORD flags=0;
    check(GetFileInformationByHandle(pin.get(),&after)&&GetHandleInformation(pin.get(),&flags)&&
        before.dwVolumeSerialNumber==after.dwVolumeSerialNumber&&before.nFileIndexHigh==after.nFileIndexHigh&&
        before.nFileIndexLow==after.nFileIndexLow&&!(after.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&
        (!lifetime||(flags&HANDLE_FLAG_PROTECT_FROM_CLOSE)),"same directory protection retained throughout normal operation");
    if(result) verify_text(target,"new dummy content");
    else { verify_text(source,"new dummy content"); if(existing) verify_text(target,"old dummy content"); }
    return result;
}

int run() {
    HANDLE token_raw=nullptr;
    check(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token_raw)!=FALSE,"read own token only");
    Handle token(token_raw); TOKEN_ELEVATION elevation{}; DWORD token_bytes=0;
    check(GetTokenInformation(token.get(),TokenElevation,&elevation,sizeof(elevation),&token_bytes)&&!elevation.TokenIsElevated,
        "ordinary non-elevated fixture execution required");
    const auto cwd=fs::current_path();
    check(fs::is_regular_file(cwd/L"test/platform/consolidation_defensive_publication.cpp"),"run from checkout");
    const auto parent=cwd/L"build/consolidation-security-probe";
    // Read-only ancestor validation for this already-owned engineering location.
    std::vector<fs::path> chain;
    for(auto path=parent;;path=path.parent_path()) { chain.push_back(path); if(path==path.parent_path()) break; }
    std::vector<Handle> ancestry;
    for(auto it=chain.rbegin();it!=chain.rend();++it) ancestry.push_back(open_directory(*it,false));
    const auto root=parent/(L"defensive-publication-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    directory(root); std::wcout<<L"Disposable root: "<<root.native()<<L'\n';
    unsigned index=0,controls=0,retained_success=0;
    for(Method method:{Method::move,Method::absolute_handle,Method::relative_handle,Method::replace}) {
        for(bool existing:{false,true}) {
            if(method==Method::replace&&!existing) continue; // API requires an existing destination: seven applicable pairs.
            controls+=one_case(root,method,existing,false,index++)?1:0;
            retained_success+=one_case(root,method,existing,true,index++)?1:0;
        }
    }
    std::cout<<"Completed "<<checks<<" checks; ordinary controls="<<controls<<"/7; retained-protection publication="<<retained_success<<"/7.\n";
    std::cout<<"No permission changes or installed-system access. Owned dummy fixtures retained; no cleanup outside them.\n";
    if(!retained_success) {
        std::cout<<"UNRESOLVED SAFETY/COMPATIBILITY GATE: tested lifetime sharing excludes required atomic publication.\n"; return 3;
    }
    std::cout<<"Limited API compatibility result only; production integration remains blocked for review.\n"; return 0;
}
}
int main(int argc,char**) {
    try { check(argc==1,"no target arguments accepted"); return run(); }
    catch(const std::exception& error) { std::cerr<<"DEFENSIVE FIXTURE INCOMPLETE: "<<error.what()<<'\n'; return 2; }
}
