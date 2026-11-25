// Test-only extension of the frozen structural feasibility helpers. The old
// executable and its entry point remain unchanged. Never operates on SFX.
#define main prior_structural_feasibility_entry
#include "consolidation_structural_acl_feasibility.cpp"
#undef main
#include <cstdint>

namespace {
constexpr DWORD packet_magic=0x53465848;
struct Packet { DWORD magic, pid; std::uint64_t handle; DWORD valid, flags; };

fs::path test_parent() {
    const auto cwd=fs::current_path();
    require(fs::is_regular_file(cwd/L"test/platform/consolidation_lifetime_handle_probe.cpp"),"checkout required");
    const auto parent=cwd/L"build/consolidation-security-probe";
    for(auto p=parent;;p=p.parent_path()) {
        const DWORD attr=GetFileAttributesW(p.c_str());
        require(attr!=INVALID_FILE_ATTRIBUTES && !(attr&FILE_ATTRIBUTE_REPARSE_POINT),"non-reparse fixture ancestors");
        if(p==p.parent_path()) break;
    }
    return parent;
}

void send_packet(HANDLE output,HANDLE pin) {
    Packet message{packet_magic,GetCurrentProcessId(),reinterpret_cast<std::uintptr_t>(pin),0,0};
    message.valid=GetHandleInformation(pin,&message.flags)!=FALSE;
    DWORD done=0;
    require(WriteFile(output,&message,sizeof(message),&done,nullptr) && done==sizeof(message),"write bounded child reply");
}

bool receive(HANDLE input,void* value,DWORD length) {
    const auto deadline=GetTickCount64()+5000;
    while(GetTickCount64()<deadline) {
        DWORD available=0;
        if(!PeekNamedPipe(input,nullptr,0,nullptr,&available,nullptr)) return false;
        if(available>=length) { DWORD done=0; return ReadFile(input,value,length,&done,nullptr) && done==length; }
        Sleep(5);
    }
    return false;
}

int child(const std::string& leaf,bool protect) {
    require(!leaf.empty() && leaf.rfind("lifetime-handles-",0)==0 && leaf.size()<100 &&
        leaf.find_first_not_of("lifetimehands-0123456789")==std::string::npos,"bounded fixture name only");
    const auto parent=test_parent(); const auto sid=identity(); auto wanted=descriptor(sid,true);
    const auto path=parent/fs::path(leaf)/L"Config";
    Handle pin(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    require(pin.get()!=INVALID_HANDLE_VALUE,"child acquired directory pin");
    BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileInformationByHandle(pin.get(),&info) && (info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) &&
        !(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) && exact(path,wanted.get()),"child validates protected real boundary");
    if(protect) require(SetHandleInformation(pin.get(),HANDLE_FLAG_PROTECT_FROM_CLOSE,HANDLE_FLAG_PROTECT_FROM_CLOSE),
        "set documented protect-from-close flag");
    const auto input=GetStdHandle(STD_INPUT_HANDLE),output=GetStdHandle(STD_OUTPUT_HANDLE);
    send_packet(output,pin.get());
    for(unsigned i=0;i<3;++i) {
        char command=0; require(receive(input,&command,1),"bounded child command wait");
        if(command=='x') break;
        require(command=='?',"recognized child status command"); send_packet(output,pin.get());
    }
    // A deliberately closed remote handle must not be closed after possible reuse.
    DWORD flags=0;
    if(GetHandleInformation(pin.get(),&flags)) {
        if(flags&HANDLE_FLAG_PROTECT_FROM_CLOSE)
            require(SetHandleInformation(pin.get(),HANDLE_FLAG_PROTECT_FROM_CLOSE,0),"intentional final pin retirement");
    } else (void)pin.release();
    return 0;
}

struct PipePair { Handle read,write; };
PipePair pipe_pair() {
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),nullptr,TRUE}; HANDLE r=nullptr,w=nullptr;
    require(CreatePipe(&r,&w,&attributes,0),"create fixture pipe"); return {Handle(r),Handle(w)};
}

bool probe(bool protect) {
    const auto parent=test_parent(); const auto sid=identity();
    auto structure=descriptor(sid,false),boundary=descriptor(sid,true);
    const auto leaf="lifetime-handles-"+std::to_string(GetCurrentProcessId())+"-"+
        std::to_string(GetTickCount64())+(protect?"-1":"-0");
    const auto root=parent/fs::path(leaf); make_directory(root); make_directory(root/L"Config");
    make_file(root/L"Config/SFX.boundary"); seal(root/L"Config/SFX.boundary",structure.get());
    seal(root/L"Config",boundary.get()); seal(root,structure.get());
    std::wcout<<L"Fixture: "<<root.native()<<L'\n';
    std::cout<<"Protect-from-close="<<protect<<"; same-user non-elevated child, default process security.\n"<<std::flush;
    auto commands=pipe_pair(),replies=pipe_pair();
    require(SetHandleInformation(commands.write.get(),HANDLE_FLAG_INHERIT,0) &&
        SetHandleInformation(replies.read.get(),HANDLE_FLAG_INHERIT,0),"parent-only pipe ends");
    wchar_t self[MAX_PATH]{}; const auto length=GetModuleFileNameW(nullptr,self,MAX_PATH);
    require(length && length<MAX_PATH,"fixture executable path");
    const auto leaf_w=fs::path(leaf).native();
    std::wstring command=L"\""+std::wstring(self)+L"\" --child-pin "+leaf_w+(protect?L" 1":L" 0");
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=commands.read.get(); startup.hStdOutput=replies.write.get(); startup.hStdError=replies.write.get();
    PROCESS_INFORMATION created{};
    require(CreateProcessW(self,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,
        fs::current_path().c_str(),&startup,&created),"launch only own disposable child");
    CloseHandle(created.hThread);
    // Do NOT use the creator's automatically privileged process handle for the probe.
    Handle watch(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,created.dwProcessId));
    CloseHandle(created.hProcess); commands.read.reset(); replies.write.reset();
    require(watch.get()!=nullptr,"fresh limited child monitoring handle");
    Packet ready{}; require(receive(replies.read.get(),&ready,sizeof(ready)) && ready.magic==packet_magic &&
        ready.pid==created.dwProcessId && ready.valid,"real child reports live pin");
    if(protect) require((ready.flags&HANDLE_FLAG_PROTECT_FROM_CLOSE)!=0,"child flag is actually set");
    opaque_reparse_attempt(root/L"Config",true,true);
    Handle access(OpenProcess(PROCESS_DUP_HANDLE,FALSE,created.dwProcessId));
    const DWORD open_error=access?ERROR_SUCCESS:GetLastError();
    std::cout<<"Fresh OpenProcess(PROCESS_DUP_HANDLE): "<<(access?"GRANTED":"DENIED")<<" error="<<open_error<<'\n';
    BOOL duplicated=FALSE; DWORD duplicate_error=0;
    if(access) {
        HANDLE copy=nullptr;
        duplicated=DuplicateHandle(access.get(),reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(ready.handle)),
            GetCurrentProcess(),&copy,0,FALSE,DUPLICATE_SAME_ACCESS|DUPLICATE_CLOSE_SOURCE);
        duplicate_error=duplicated?ERROR_SUCCESS:GetLastError();
        if(copy) {
            DWORD flags=0;
            if(GetHandleInformation(copy,&flags) && (flags&HANDLE_FLAG_PROTECT_FROM_CLOSE))
                SetHandleInformation(copy,HANDLE_FLAG_PROTECT_FROM_CLOSE,0);
            CloseHandle(copy);
        }
    }
    std::cout<<"DuplicateHandle(close source): result="<<duplicated<<" error="<<duplicate_error<<'\n'<<std::flush;
    char query='?'; DWORD done=0;
    require(WriteFile(commands.write.get(),&query,1,&done,nullptr)&&done==1,"query still-running child");
    Packet after{}; require(receive(replies.read.get(),&after,sizeof(after)) && after.magic==packet_magic &&
        after.pid==ready.pid && after.handle==ready.handle,"same child responds after attempted remote close");
    DWORD exit_code=0;
    require(GetExitCodeProcess(watch.get(),&exit_code)&&exit_code==STILL_ACTIVE,"child remains alive during probe");
    std::cout<<"Child still active; original pin valid="<<after.valid<<'\n';
    // Only the probe's own nonempty Config boundary is ever tagged. No redirection target.
    opaque_reparse_attempt(root/L"Config",after.valid!=0,true);
    char stop='x'; require(WriteFile(commands.write.get(),&stop,1,&done,nullptr)&&done==1,"request child shutdown");
    commands.write.reset();
    require(WaitForSingleObject(watch.get(),5000)==WAIT_OBJECT_0 && GetExitCodeProcess(watch.get(),&exit_code)&&
        exit_code==0,"child cleanly exited; no remaining fixture worker");
    std::cout<<"CASE COMPLETE: pin "<<(after.valid?"survived":"was lost WHILE CHILD REMAINED ALIVE")<<".\n";
    return after.valid!=0;
}
}

int main(int argc,char** argv) {
    try {
        if(argc==4 && std::string(argv[1])=="--child-pin") {
            require(std::string(argv[3])=="0" || std::string(argv[3])=="1","bounded protection selector");
            return child(argv[2],std::string(argv[3])=="1");
        }
        require(argc==1,"no fixture target arguments allowed");
        const bool ordinary=probe(false),protected_handle=probe(true);
        std::cout<<"RESULT: ordinary pin="<<ordinary<<", protected pin="<<protected_handle<<", "<<checks<<" parent checks.\n";
        std::cout<<"TEST-ONLY LIFETIME PROBE; no complete startup/UAC/platform qualification claim.\n";
        return protected_handle?0:1;
    } catch(const std::exception& error) {
        std::cerr<<"FIXTURE FAILED after "<<checks<<" checks: "<<error.what()<<'\n'; return 2;
    }
}
