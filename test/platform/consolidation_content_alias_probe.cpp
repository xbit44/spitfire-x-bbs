// Test-only local hard-link counterexample. Both endpoints are newly-created
// disposable text, confined to one fixture. No installed or third-party files.
#define main prior_structural_feasibility_entry
#include "consolidation_structural_acl_feasibility.cpp"
#undef main

namespace {
struct Unpin {
    void operator()(void* value) const {
        if(value && value!=INVALID_HANDLE_VALUE) {
            SetHandleInformation(value,HANDLE_FLAG_PROTECT_FROM_CLOSE,0); CloseHandle(value);
        }
    }
};
using Pin=std::unique_ptr<void,Unpin>;

void run() {
    const auto cwd=fs::current_path();
    require(fs::is_regular_file(cwd/L"test/platform/consolidation_content_alias_probe.cpp"),"checkout required");
    const auto parent=cwd/L"build/consolidation-security-probe";
    for(auto p=parent;;p=p.parent_path()) {
        const DWORD attr=GetFileAttributesW(p.c_str());
        require(attr!=INVALID_FILE_ATTRIBUTES && !(attr&FILE_ATTRIBUTE_REPARSE_POINT),"non-reparse fixture ancestry");
        if(p==p.parent_path()) break;
    }
    const auto user=identity(); auto structural=descriptor(user,false),boundary=descriptor(user,true);
    const auto guard=parent/(L"content-alias-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    const auto root=guard/L"SFX",config=root/L"Config",outside=guard/L"outside-SFX-dummy-only";
    const auto original=outside/L"sentinel.txt",alias=config/L"SFX.pending.conf.writing";
    make_directory(guard); make_directory(root); make_directory(config); make_directory(outside); make_file(original);
    const std::wstring sddl=L"O:"+user+L"D:P(A;;FA;;;"+user+L")(A;;FA;;;SY)(A;;FA;;;BA)";
    PSECURITY_DESCRIPTOR raw=nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&raw,nullptr),"dummy content descriptor");
    Local content(raw); seal(original,content.get());
    seal(outside,structural.get()); seal(config,boundary.get()); seal(root,structural.get()); seal(guard,structural.get());
    std::wcout<<L"Fixture: "<<guard.native()<<L'\n';
    std::vector<Pin> pins;
    for(const auto& path:{guard,root,config}) {
        Pin pin(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        require(pin.get()!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(pin.get(),&info)&&
            (info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&
            exact(path,path==config?boundary.get():structural.get()),"pinned and validated real structural object");
        require(SetHandleInformation(pin.get(),HANDLE_FLAG_PROTECT_FROM_CLOSE,HANDLE_FLAG_PROTECT_FROM_CLOSE),"protect pin from closure");
        pins.push_back(std::move(pin));
    }
    verify_original(original);
    require(CreateHardLinkW(alias.c_str(),original.c_str(),nullptr),"create real same-user file alias inside pinned Config");
    {
        Handle h(CreateFileW(alias.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        require(h.get()!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(h.get(),&info)&&
            !(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&info.nNumberOfLinks==2,
            "alias is an ordinary non-reparse file with two names");
    }
    std::cout<<"All boundary pins remain active; alias is not a reparse point.\n"<<std::flush;
    {
        // Exact opening shape used by publish_protected_text in the unchanged
        // alpha_runtime_platform.cpp, applied ONLY to the dummy alias above.
        Handle pending(CreateFileW(alias.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr));
        require(pending.get()!=INVALID_HANDLE_VALUE,"ordinary pending-content write opened");
        const char changed[]="changed disposable outside sentinel"; DWORD done=0;
        require(WriteFile(pending.get(),changed,sizeof(changed),&done,nullptr)&&done==sizeof(changed)&&FlushFileBuffers(pending.get()),
            "write dummy bytes using current pending-file opening shape");
    }
    for(const auto& pin:pins) {
        DWORD flags=0; BY_HANDLE_FILE_INFORMATION info{};
        require(GetHandleInformation(pin.get(),&flags)&&(flags&HANDLE_FLAG_PROTECT_FROM_CLOSE)&&
            GetFileInformationByHandle(pin.get(),&info)&&!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),
            "protected directory pin remained valid and real throughout the write");
    }
    require(exact(guard,structural.get())&&exact(root,structural.get())&&exact(config,boundary.get()),
        "all structural owners and ACLs unchanged");
    Handle outside_read(CreateFileW(original.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    const char expected[]="changed disposable outside sentinel"; char actual[sizeof(expected)+1]{}; DWORD done=0;
    require(outside_read.get()!=INVALID_HANDLE_VALUE&&ReadFile(outside_read.get(),actual,sizeof(actual),&done,nullptr)&&
        done==sizeof(expected)&&!std::memcmp(actual,expected,sizeof(expected)),"actual outside-SFX dummy file changed through in-tree alias");
    std::cout<<"CONFINEMENT COUNTEREXAMPLE REPRODUCED: "<<checks<<" checks; outside-SFX dummy content changed while every pin remained active.\n";
    std::cout<<"Only fixture-owned dummy text was modified. No Runtime, account, host key, installed SFX or external owner file was touched.\n";
}
}
int main() {
    try { run(); return 1; } // Reproduction means whole-tree confinement is NOT qualified.
    catch(const std::exception& error) { std::cerr<<"PROBE INCOMPLETE after "<<checks<<" checks: "<<error.what()<<'\n'; return 2; }
}
