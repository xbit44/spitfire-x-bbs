// Isolated native startup/race controls. No production root, setup or repair.
#define main prior_structural_feasibility_entry
#include "consolidation_structural_acl_feasibility.cpp"
#undef main

namespace {
struct Pinned {
    Handle handle;
    explicit Pinned(HANDLE value):handle(value) {}
    Pinned(Pinned&&)=default;
    Pinned& operator=(Pinned&&)=delete;
    ~Pinned() { if(handle && handle.get()!=INVALID_HANDLE_VALUE)
        SetHandleInformation(handle.get(),HANDLE_FLAG_PROTECT_FROM_CLOSE,0); }
};

bool exact_handle(HANDLE h,void* expected) {
    PSID owner=nullptr,want_owner=nullptr; PACL acl=nullptr,want_acl=nullptr;
    PSECURITY_DESCRIPTOR raw=nullptr; BOOL present=FALSE,defaulted=FALSE;
    if(GetSecurityInfo(h,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
        &owner,nullptr,&acl,nullptr,&raw)!=ERROR_SUCCESS) return false;
    Local sd(raw); SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision=0;
    if(!GetSecurityDescriptorOwner(expected,&want_owner,&defaulted) || !owner || !EqualSid(owner,want_owner) ||
        !GetSecurityDescriptorDacl(expected,&present,&want_acl,&defaulted) || !present || !acl || !want_acl ||
        !GetSecurityDescriptorControl(raw,&control,&revision) || !(control&SE_DACL_PROTECTED) ||
        acl->AceCount!=want_acl->AceCount) return false;
    std::vector<bool> consumed(acl->AceCount);
    for(unsigned i=0;i<want_acl->AceCount;++i) {
        void* want=nullptr; if(!GetAce(want_acl,i,&want)) return false; bool found=false;
        const auto bytes=static_cast<ACE_HEADER*>(want)->AceSize;
        for(unsigned j=0;j<acl->AceCount;++j) {
            void* actual=nullptr; if(!GetAce(acl,j,&actual)) return false;
            if(!consumed[j] && static_cast<ACE_HEADER*>(actual)->AceSize==bytes && !std::memcmp(want,actual,bytes)) {
                consumed[j]=true; found=true; break;
            }
        }
        if(!found) return false;
    }
    return true;
}

struct Tree {
    fs::path guard,root,target;
    std::wstring user;
    Local structure,boundary;
    std::vector<std::pair<fs::path,bool>> directories;
    Tree():user(identity()),structure(descriptor(user,false)),boundary(descriptor(user,true)) {
        const auto cwd=fs::current_path();
        require(fs::is_regular_file(cwd/L"test/platform/consolidation_lifetime_startup_probe.cpp"),"checkout required");
        const auto parent=cwd/L"build/consolidation-security-probe";
        for(auto p=parent;;p=p.parent_path()) {
            const DWORD a=GetFileAttributesW(p.c_str());
            require(a!=INVALID_FILE_ATTRIBUTES && !(a&FILE_ATTRIBUTE_REPARSE_POINT),"fixture ancestry is real");
            if(p==p.parent_path()) break;
        }
        guard=parent/(L"startup-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        root=guard/L"SFX"; target=guard/L"outside-SFX-dummy-target";
        make_directory(guard); make_directory(root); make_directory(target);
        directories={{guard,false},{root,false}};
        for(const wchar_t* leaf:{L"Bin",L"Config",L"Data",L"Displays",L"Backups",L"Logs",L"Doors",L"Work"}) {
            make_directory(root/leaf); directories.emplace_back(root/leaf,wcscmp(leaf,L"Bin")!=0);
        }
        make_file(root/L"Bin/SFXRuntime.exe"); make_file(root/L"Bin/SFXControlPanel.exe");
        seal(root/L"Bin/SFXRuntime.exe",structure.get()); seal(root/L"Bin/SFXControlPanel.exe",structure.get());
        seal(target,structure.get());
        for(auto i=directories.rbegin();i!=directories.rend();++i)
            seal(i->first,i->second?boundary.get():structure.get());
        std::wcout<<L"Fixture: "<<guard.native()<<L'\n';
    }
};

struct Pins {
    std::vector<Pinned> held;
    DWORD rejection=ERROR_SUCCESS;
    unsigned content_reads=0;
    bool acquire(const Tree& tree) {
        // Every fixture ancestor from the trusted disposable anchor is pinned
        // top-down before its child is opened. Production anchoring is NOT wired.
        require(held.empty(),"new acquisition only");
        for(const auto& entry:tree.directories) {
            Pinned pin(CreateFileW(entry.first.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            if(pin.handle.get()==INVALID_HANDLE_VALUE) { rejection=GetLastError(); held.clear(); return false; }
            BY_HANDLE_FILE_INFORMATION info{};
            if(!GetFileInformationByHandle(pin.handle.get(),&info) || !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) ||
                (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
                !exact_handle(pin.handle.get(),entry.second?tree.boundary.get():tree.structure.get())) {
                rejection=ERROR_INVALID_DATA; held.clear(); return false;
            }
            require(SetHandleInformation(pin.handle.get(),HANDLE_FLAG_PROTECT_FROM_CLOSE,HANDLE_FLAG_PROTECT_FROM_CLOSE),
                "protect retained pin from close");
            held.push_back(std::move(pin));
        }
        return true;
    }
    bool start(const Tree& tree) {
        if(!acquire(tree)) return false;
        verify_original(tree.root/L"Bin/SFXRuntime.exe");
        ++content_reads;
        return true;
    }
};

Handle writer(const fs::path& path) {
    return Handle(CreateFileW(path.c_str(),FILE_ADD_FILE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,
        OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
}
void denied_writer(const fs::path& path) {
    auto handle=writer(path); require(handle.get()==INVALID_HANDLE_VALUE && GetLastError()==ERROR_SHARING_VIOLATION,
        "live pin excludes new directory writer");
}

struct Race {
    fs::path path;
    HANDLE start,attempted,retire;
    bool opened=false;
    DWORD error=0;
};
DWORD WINAPI compete(void* value) {
    auto& race=*static_cast<Race*>(value);
    if(WaitForSingleObject(race.start,5000)!=WAIT_OBJECT_0) return 10;
    auto handle=writer(race.path); race.opened=handle.get()!=INVALID_HANDLE_VALUE;
    race.error=race.opened?ERROR_SUCCESS:GetLastError();
    SetEvent(race.attempted);
    return WaitForSingleObject(race.retire,5000)==WAIT_OBJECT_0?0:11;
}

void run() {
    Tree tree; const auto config=tree.root/L"Config";
    {
        auto old=writer(config); require(old.get()!=INVALID_HANDLE_VALUE,"pre-existing real writer control");
        Pins attempt; require(!attempt.start(tree) && attempt.rejection==ERROR_SHARING_VIOLATION &&
            attempt.held.empty() && !attempt.content_reads,"startup rejects pre-existing writer before content");
    }
    {
        Handle old(CreateFileW(config.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,
            OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        require(old.get()!=INVALID_HANDLE_VALUE,"pre-existing read-only handle control");
        Pins pins; require(pins.start(tree)&&pins.content_reads==1,"read-only pre-existing handle permits startup and actual content read");
        denied_writer(config);
        HANDLE raw=nullptr;
        const BOOL expanded=DuplicateHandle(GetCurrentProcess(),old.get(),GetCurrentProcess(),&raw,FILE_ADD_FILE,FALSE,0);
        Handle duplicate(raw);
        require(!expanded && GetLastError()==ERROR_ACCESS_DENIED,"read-only handle cannot be duplicated into writer");
    }
    unsigned startup_wins=0,writer_wins=0;
    for(unsigned i=0;i<128;++i) {
        Handle start(CreateEventW(nullptr,TRUE,FALSE,nullptr)),attempted(CreateEventW(nullptr,TRUE,FALSE,nullptr)),
            retire(CreateEventW(nullptr,TRUE,FALSE,nullptr));
        require(start&&attempted&&retire,"bounded race events");
        Race race{config,start.get(),attempted.get(),retire.get()};
        Handle thread(CreateThread(nullptr,0,compete,&race,0,nullptr)); require(thread!=nullptr,"real competing thread");
        SetEvent(start.get()); Pins pins; const bool accepted=pins.start(tree);
        const DWORD ready=WaitForSingleObject(attempted.get(),5000);
        SetEvent(retire.get());
        if(WaitForSingleObject(thread.get(),5000)!=WAIT_OBJECT_0) {
            // Terminate this test process rather than retire storage under an unjoined thread.
            std::cerr<<"TEST PROCESS ABORT: race worker did not retire\n"; ExitProcess(2);
        }
        DWORD status=0; require(ready==WAIT_OBJECT_0 && GetExitCodeThread(thread.get(),&status)&&status==0,"race worker joined");
        require(accepted!=race.opened,"startup and mutation handles cannot both win");
        if(accepted) { ++startup_wins; require(race.error==ERROR_SHARING_VIOLATION,"late writer denied"); }
        else { ++writer_wins; require(pins.rejection==ERROR_SHARING_VIOLATION && pins.held.empty() && !pins.content_reads,
            "writer-first rejects without content or leaked partial pins"); }
    }
    std::cout<<"128 real startup races: startup wins="<<startup_wins<<", writer wins="<<writer_wins<<"; no dual winners.\n";
    {
        Pins panel,runtime;
        require(panel.acquire(tree)&&runtime.acquire(tree),"overlapping Desktop and Runtime protection");
        for(const auto& entry:tree.directories) if(entry.second) {
            make_file(entry.first/L"owned.tmp"); make_file(entry.first/L"published.tmp");
            const BOOL published=MoveFileExW((entry.first/L"owned.tmp").c_str(),(entry.first/L"published.tmp").c_str(),
                MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
            const DWORD publish_error=published?ERROR_SUCCESS:GetLastError();
            std::cout<<"Normal same-directory atomic publication: result="<<published<<" error="<<publish_error<<'\n';
            require(published!=FALSE,"normal atomic content publication must work while pinned");
            require(DeleteFileW((entry.first/L"published.tmp").c_str()),"normal content cleanup");
            make_directory(entry.first/L"owned-job"); make_file(entry.first/L"owned-job/owned.tmp");
            require(DeleteFileW((entry.first/L"owned-job/owned.tmp").c_str())&&RemoveDirectoryW((entry.first/L"owned-job").c_str()),
                "owned nested content cleanup");
            denied_writer(entry.first);
        }
        runtime.held.clear(); denied_writer(config);
        Pins replacement; require(replacement.acquire(tree),"replacement Runtime reacquires under continuous Desktop pins");
        panel.held.clear(); denied_writer(config);
        // Native handle-overlap model only: no claim to test production callers.
        replacement.held.clear(); auto writable=writer(config);
        require(writable.get()!=INVALID_HANDLE_VALUE,"accepted offline boundary after final lifetime ends");
    }
    junction_attempt(config,tree.target,FILE_ADD_FILE,false);
    {
        Pins rejected;
        require(!rejected.start(tree)&&rejected.rejection==ERROR_INVALID_DATA && rejected.held.empty() &&
            !rejected.content_reads,"offline reparse rejected with zero content use and no repair");
        Handle object(CreateFileW(config.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        require(object.get()!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(object.get(),&info)&&
            (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"rejected object remains untouched for elevated repair");
        require(fs::is_empty(tree.target),"no content was redirected to the outside-SFX dummy target");
    }
    std::cout<<"STARTUP/RACE CONTROLS PASS: "<<checks<<" checks. No production/Setup/Repair/owner-platform qualification claim.\n";
}
}

int main() {
    try { run(); return 0; }
    catch(const std::exception& error) { std::cerr<<"STARTUP CONTROL FAILED after "<<checks<<" checks: "<<error.what()<<'\n'; return 1; }
}
