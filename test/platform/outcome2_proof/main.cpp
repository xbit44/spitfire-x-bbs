// Owner-authorized defensive fixture, never a Setup implementation.
// Targets only new C:\xtest-sfx-o2-<generated-id> objects on a verified volume.
// Elevated mode never accepts a pathname, adopts existing structure, reads the
// preserved source, changes existing ACLs, deletes objects, or runs copied code.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <authz.h>
#include <sddl.h>
#include <shellapi.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs=std::filesystem;
namespace {
unsigned checks=0;
void require(bool ok,const char* what) { ++checks; if(!ok) throw std::runtime_error(std::string(what)+" error="+std::to_string(GetLastError())); }
struct Close { void operator()(void* h) const { if(h&&h!=INVALID_HANDLE_VALUE) CloseHandle(h); } };
struct Free { void operator()(void* p) const { if(p) LocalFree(p); } };
using Handle=std::unique_ptr<void,Close>;
using Local=std::unique_ptr<void,Free>;
// Created only after a fresh protected root is bound and validated. Failure
// diagnostics use this retained creating handle, never an untrusted log path.
Handle diagnostic;
void trace(const std::string& s) {
    if(!diagnostic||diagnostic.get()==INVALID_HANDLE_VALUE)return;
    DWORD written=0;WriteFile(diagnostic.get(),s.data(),static_cast<DWORD>(s.size()),&written,nullptr);FlushFileBuffers(diagnostic.get());
}
constexpr DWORD share_all=FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE;
constexpr DWORD dir_flags=FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT;
constexpr std::array<const wchar_t*,8> mutable_dirs{L"Config",L"Data",L"Displays",L"Backups",L"Logs",L"Files",L"Doors",L"Work"};
constexpr std::array<const wchar_t*,2> program_names{L"SFXControlPanel.exe",L"SFXRuntime.exe"};
constexpr std::array<const char*,4> package_bytes{
    "SFX O2 TRUSTED FIXTURE CP RELEASE 1\r\n", "SFX O2 TRUSTED FIXTURE RT RELEASE 1\r\n",
    "SFX O2 TRUSTED FIXTURE CP RELEASE 2\r\n", "SFX O2 TRUSTED FIXTURE RT RELEASE 2\r\n"};
constexpr const char* source_program="UNTRUSTED ADOPTED DUMMY - NEVER INSTALL OR EXECUTE\r\n";

Handle token() { HANDLE raw=nullptr; require(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_DUPLICATE,&raw)!=FALSE,"own token"); return Handle(raw); }
bool elevated(HANDLE t) { TOKEN_ELEVATION v{}; DWORD n=0; require(GetTokenInformation(t,TokenElevation,&v,sizeof(v),&n)!=FALSE,"elevation query"); return v.TokenIsElevated!=0; }
std::wstring sid(HANDLE t) {
    DWORD n=0; GetTokenInformation(t,TokenUser,nullptr,0,&n); std::vector<BYTE> data(n);
    require(GetTokenInformation(t,TokenUser,data.data(),n,&n)!=FALSE,"token user"); wchar_t* text=nullptr;
    require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&text)!=FALSE,"SID string"); Local h(text); return text;
}
std::wstring sid_string(PSID value) { wchar_t* text=nullptr; require(value&&ConvertSidToStringSidW(value,&text),"object owner SID"); Local h(text); return text; }
Handle limited(HANDLE t) {
    if(!elevated(t)) { HANDLE copy=nullptr; require(DuplicateToken(t,SecurityImpersonation,&copy)!=FALSE,"own ordinary access-check token"); return Handle(copy); }
    TOKEN_LINKED_TOKEN linked{}; DWORD n=0;
    require(GetTokenInformation(t,TokenLinkedToken,&linked,sizeof(linked),&n)!=FALSE,"split-token UAC required; other-credential elevation refuses");
    Handle original(linked.LinkedToken); require(!elevated(original.get())&&sid(t)==sid(original.get()),"same installation user across UAC");
    // TokenLinkedToken grants TOKEN_QUERY, not necessarily TOKEN_DUPLICATE.
    // Authz consumes that query-only token directly; no privilege/identity change.
    return original;
}
struct Authorization {
    AUTHZ_RESOURCE_MANAGER_HANDLE manager=nullptr;
    AUTHZ_CLIENT_CONTEXT_HANDLE context=nullptr;
    explicit Authorization(HANDLE t) {
        require(AuthzInitializeResourceManager(AUTHZ_RM_FLAG_NO_AUDIT,nullptr,nullptr,nullptr,L"SFX defensive access query",&manager)!=FALSE,"read-only local Authz manager");
        LUID id{};
        if(!AuthzInitializeContextFromToken(0,t,manager,nullptr,id,nullptr,&context)) {
            AuthzFreeResourceManager(manager); manager=nullptr; require(false,"query-only ordinary token authorization context");
        }
        require(context!=nullptr,"ordinary token authorization context");
    }
    ~Authorization(){if(context)AuthzFreeContext(context);if(manager)AuthzFreeResourceManager(manager);}
    bool denies(void* sd,DWORD requested) const {
        AUTHZ_ACCESS_REQUEST request{};request.DesiredAccess=requested;
        DWORD granted=0,error=0,sacl=0;AUTHZ_ACCESS_REPLY reply{};reply.ResultListLength=1;
        reply.GrantedAccessMask=&granted;reply.Error=&error;reply.SaclEvaluationResults=&sacl;
        require(AuthzAccessCheck(0,context,&request,nullptr,sd,nullptr,0,&reply,nullptr)!=FALSE,"read-only access evaluation");
        return error==ERROR_ACCESS_DENIED&&!(granted&requested);
    }
};
bool valid_id(const std::wstring& s) { return s.size()==24&&std::all_of(s.begin(),s.end(),[](wchar_t c){return (c>=L'0'&&c<=L'9')||(c>=L'A'&&c<=L'F');}); }
Handle open_dir(const fs::path& p,DWORD access=GENERIC_READ|READ_CONTROL,DWORD sharing=FILE_SHARE_READ|FILE_SHARE_WRITE) {
    return Handle(CreateFileW(p.c_str(),access,sharing,nullptr,OPEN_EXISTING,dir_flags,nullptr));
}
BY_HANDLE_FILE_INFORMATION inspect(HANDLE h,bool directory) {
    BY_HANDLE_FILE_INFORMATION i{};
    require(h!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(h,&i)&&!(i.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&
        ((i.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0)==directory&&(directory||i.nNumberOfLinks==1),"non-reparse bound object, single-link file"); return i;
}
bool same(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b) { return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber&&a.nFileIndexHigh==b.nFileIndexHigh&&a.nFileIndexLow==b.nFileIndexLow; }
std::wstring final_nt(HANDLE h) {
    std::array<wchar_t,32768> text{}; auto n=GetFinalPathNameByHandleW(h,text.data(),static_cast<DWORD>(text.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_NT);
    require(n&&n<text.size(),"bound NT volume path"); return {text.data(),n};
}
bool trusted_sid(const std::wstring& value) {
    return value==L"S-1-5-18"||value==L"S-1-5-32-544"||
        value==L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";
}
struct Anchor {
    Handle directory; fs::path physical; BY_HANDLE_FILE_INFORMATION identity{};
    explicit Anchor(HANDLE ordinary) {
        require(GetDriveTypeW(L"C:\\")==DRIVE_FIXED,"C must be a local fixed volume");
        wchar_t filesystem[32]{}; DWORD flags=0;
        require(GetVolumeInformationW(L"C:\\",nullptr,0,nullptr,nullptr,&flags,filesystem,32)&&
            !wcscmp(filesystem,L"NTFS")&&(flags&FILE_PERSISTENT_ACLS),"NTFS persistent ACL volume required");
        directory=open_dir(L"C:\\"); identity=inspect(directory.get(),true);
        auto nt=final_nt(directory.get()); const std::wstring prefix=L"\\Device\\HarddiskVolume";
        require(nt.rfind(prefix,0)==0&&nt.back()==L'\\'&&nt.size()>prefix.size()+1&&
            std::all_of(nt.begin()+static_cast<std::ptrdiff_t>(prefix.size()),nt.end()-1,[](wchar_t c){return c>=L'0'&&c<=L'9';}),"refuse ambiguous or unsupported volume namespace");
        physical=fs::path(L"\\\\?\\GLOBALROOT"+nt);
        auto physical_handle=open_dir(physical); require(same(identity,inspect(physical_handle.get(),true)),"physical volume identity equals observed C root");
        PSID owner=nullptr; PACL acl=nullptr; void* raw=nullptr;
        require(GetSecurityInfo(directory.get(),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
            &owner,nullptr,&acl,nullptr,&raw)==ERROR_SUCCESS,"read volume security only"); Local descriptor(raw);
        require(acl&&trusted_sid(sid_string(owner)),"volume owner must be SYSTEM, Administrators or TrustedInstaller");
        GENERIC_MAPPING map{FILE_GENERIC_READ,FILE_GENERIC_WRITE,FILE_GENERIC_EXECUTE,FILE_ALL_ACCESS};
        Authorization authorization(ordinary);
        constexpr DWORD dangerous=WRITE_DAC|WRITE_OWNER|FILE_DELETE_CHILD|FILE_WRITE_DATA|FILE_WRITE_ATTRIBUTES|FILE_WRITE_EA;
        for(DWORD bit:{DWORD(WRITE_DAC),DWORD(WRITE_OWNER),DWORD(FILE_DELETE_CHILD),DWORD(FILE_WRITE_DATA),DWORD(FILE_WRITE_ATTRIBUTES),DWORD(FILE_WRITE_EA)}) {
            require(authorization.denies(raw,bit),
                "ordinary identity cannot rewrite/redirect/delete through volume parent");
        }
        for(DWORD n=0;n<acl->AceCount;++n) {
            void* ace=nullptr; require(GetAce(acl,n,&ace)!=FALSE,"inspect parent ACE"); auto* h=static_cast<ACE_HEADER*>(ace);
            if(h->AceFlags&INHERIT_ONLY_ACE) continue;
            require(h->AceType==ACCESS_ALLOWED_ACE_TYPE||h->AceType==ACCESS_DENIED_ACE_TYPE,"unsupported parent ACE refuses");
            if(h->AceType!=ACCESS_ALLOWED_ACE_TYPE) continue;
            auto* a=static_cast<ACCESS_ALLOWED_ACE*>(ace); DWORD mask=a->Mask; MapGenericMask(&mask,&map);
            require(!(mask&dangerous)||trusted_sid(sid_string(&a->SidStart)),"no untrusted parent grant can undermine new structure");
        }
    }
    fs::path root(const std::wstring& id) const { require(valid_id(id),"fixed fixture ID only; no pathname arguments"); return physical/(L"xtest-sfx-o2-"+id); }
};
Local descriptor(const std::wstring& owner,bool mutable_boundary) {
    auto text=L"O:"+owner+L"D:P(A;;RC;;;OW)(A;;FA;;;SY)(A;;FA;;;BA)(A;;"+
        std::wstring(mutable_boundary?L"0x1200af":L"0x1200a9")+L";;;"+owner+L")";
    if(mutable_boundary) text+=L"(A;OICIIO;FA;;;SY)(A;OICIIO;FA;;;BA)(A;OICIIO;0x1301ff;;;"+owner+L")";
    void* raw=nullptr; require(ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(),SDDL_REVISION_1,&raw,nullptr)!=FALSE,"explicit descriptor at creation"); return Local(raw);
}
bool exact(HANDLE h,void* sd) {
    PSID owner=nullptr,want_owner=nullptr; PACL acl=nullptr,want_acl=nullptr; void* raw=nullptr;
    if(GetSecurityInfo(h,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,nullptr,&acl,nullptr,&raw)) return false;
    Local holder(raw); BOOL present=FALSE,def=FALSE; SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision=0;
    if(!GetSecurityDescriptorOwner(sd,&want_owner,&def)||!owner||!EqualSid(owner,want_owner)||
        !GetSecurityDescriptorDacl(sd,&present,&want_acl,&def)||!present||!acl||!want_acl||
        !GetSecurityDescriptorControl(raw,&control,&revision)||!(control&SE_DACL_PROTECTED)||acl->AceCount!=want_acl->AceCount) return false;
    for(DWORD n=0;n<acl->AceCount;++n) { void* a=nullptr; void* b=nullptr; if(!GetAce(acl,n,&a)||!GetAce(want_acl,n,&b)) return false;
        auto* ah=static_cast<ACE_HEADER*>(a); if(ah->AceSize!=static_cast<ACE_HEADER*>(b)->AceSize||std::memcmp(a,b,ah->AceSize)) return false; }
    return true;
}
void make_dir(const fs::path& p,void* sd=nullptr) { SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE}; require(CreateDirectoryW(p.c_str(),sd?&sa:nullptr)!=FALSE,"new fixture directory; collision refuses"); }
std::string read(const fs::path& p) {
    Handle h(CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    auto info=inspect(h.get(),false); require(!info.nFileSizeHigh&&info.nFileSizeLow<4096,"bounded fixture file");
    std::string s(info.nFileSizeLow,'\0'); DWORD n=0; require(ReadFile(h.get(),s.data(),static_cast<DWORD>(s.size()),&n,nullptr)&&n==s.size(),"read exact bound file"); return s;
}
void write_new(const fs::path& p,const std::string& s,void* sd=nullptr) {
    SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};
    Handle h(CreateFileW(p.c_str(),GENERIC_READ|GENERIC_WRITE|READ_CONTROL,0,sd?&sa:nullptr,CREATE_NEW,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    auto before=inspect(h.get(),false); DWORD n=0;
    require(WriteFile(h.get(),s.data(),static_cast<DWORD>(s.size()),&n,nullptr)&&n==s.size()&&FlushFileBuffers(h.get()),"create/write/flush fixed bytes via one handle");
    require(!sd||exact(h.get(),sd),"new file final security at creation"); require(same(before,inspect(h.get(),false)),"same newly created object");
}
void publish(const fs::path& bin,const wchar_t* name,const std::string& s,void* sd,const std::wstring& suffix) {
    const auto pending=bin/(std::wstring(name)+suffix); write_new(pending,s,sd);
    require(MoveFileExW(pending.c_str(),(bin/name).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"atomic publication inside freshly qualified Bin");
}
bool matched(const fs::path& bin,int generation) {
    try { return read(bin/L"pair.commit")==std::to_string(generation)&&
        read(bin/program_names[0])==package_bytes[static_cast<size_t>((generation-1)*2)]&&
        read(bin/program_names[1])==package_bytes[static_cast<size_t>((generation-1)*2+1)]; }
    catch(const std::exception&) { return false; }
}
// Elevated program input has no source-tree parameter. All bytes are compile-time
// trusted fixture-media constants, shipped verbatim alongside the probe for hashing.
void program_matrix(const fs::path& root,void* sd) {
    for(int cut=0;cut<=5;++cut) {
        const auto bin=root/(L"pair-case-"+std::to_wstring(cut)); make_dir(bin,sd); auto pin=open_dir(bin); inspect(pin.get(),true);
        publish(bin,program_names[0],package_bytes[0],sd,L".v1"); publish(bin,program_names[1],package_bytes[1],sd,L".v1");
        publish(bin,L"pair.commit","1",sd,L".v1"); require(matched(bin,1),"initial matched pair");
        // Six actual on-disk durable cut states. These are controlled returns,
        // not a claim of process-crash/power-loss qualification.
        if(cut>=1) write_new(bin/L"cp.v2",package_bytes[2],sd);
        if(cut>=2) write_new(bin/L"rt.v2",package_bytes[3],sd);
        if(cut>=3) require(MoveFileExW((bin/L"cp.v2").c_str(),(bin/program_names[0]).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"first update publish");
        if(cut>=4) require(MoveFileExW((bin/L"rt.v2").c_str(),(bin/program_names[1]).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"second update publish");
        if(cut>=5) publish(bin,L"pair.commit","2",sd,L".v2");
        require(cut<3?matched(bin,1):(cut<5?!matched(bin,1)&&!matched(bin,2):matched(bin,2)),"no mixed/uncertain pair reports success");
        if(cut==3||cut==4) { // Recovery while the same fresh boundary remains known.
            publish(bin,program_names[0],package_bytes[0],sd,L".rollback"); publish(bin,program_names[1],package_bytes[1],sd,L".rollback");
            require(matched(bin,1),"same-lifetime trusted rollback preserves pair");
        }
    }
    const auto conflict=root/L"pair-conflict";make_dir(conflict,sd);
    publish(conflict,program_names[0],package_bytes[0],sd,L".v1");
    publish(conflict,program_names[1],package_bytes[1],sd,L".v1");publish(conflict,L"pair.commit","1",sd,L".v1");
    Handle reader(CreateFileW((conflict/program_names[0]).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    inspect(reader.get(),false);write_new(conflict/L"cp.pending",package_bytes[2],sd);
    const bool refused=!MoveFileExW((conflict/L"cp.pending").c_str(),(conflict/program_names[0]).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    const DWORD refusal=GetLastError();trace("Conflicting-reader replacement: refused="+std::to_string(refused)+" error="+std::to_string(refusal)+"\r\n");
    // MoveFileEx may report ACCESS_DENIED for an open replacement destination.
    // Require real refusal and unchanged bytes; never waive a successful move.
    require(refused&&(refusal==ERROR_SHARING_VIOLATION||refusal==ERROR_ACCESS_DENIED),
        "own conflicting reader refuses replacement without bypass");
    require(matched(conflict,1),"conflict preserves original matched pair");reader.reset();
    require(MoveFileExW((conflict/L"cp.pending").c_str(),(conflict/program_names[0]).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"retry after own reader closes");
    require(!matched(conflict,1)&&!matched(conflict,2),"partial retry never activates mixed pair");
    publish(conflict,program_names[0],package_bytes[0],sd,L".rollback");require(matched(conflict,1),"trusted rollback after conflict/retry");
}
int privileged(const std::wstring& id) {
    auto t=token(); require(elevated(t.get()),"elevated mode requires actual UAC token"); auto ordinary=limited(t.get());
    Anchor anchor(ordinary.get()); const auto root=anchor.root(id); auto structural=descriptor(sid(ordinary.get()),false); auto content=descriptor(sid(ordinary.get()),true);
    SECURITY_ATTRIBUTES sa{sizeof(sa),structural.get(),FALSE};
    // No preflight traversal of an existing target, and never repair/retry it.
    if(!CreateDirectoryW(root.c_str(),&sa)) return GetLastError()==ERROR_ALREADY_EXISTS?21:22;
    auto root_pin=open_dir(root); auto root_id=inspect(root_pin.get(),true); require(exact(root_pin.get(),structural.get()),"fresh root exact final owner and ACL");
    diagnostic.reset(CreateFileW((root/L"probe-diagnostic.txt").c_str(),GENERIC_WRITE|READ_CONTROL,FILE_SHARE_READ,&sa,CREATE_NEW,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    inspect(diagnostic.get(),false);require(exact(diagnostic.get(),structural.get()),"diagnostic final security at creation");
    trace("Fresh protected root bound and validated.\r\n");
    require(!CreateDirectoryW(root.c_str(),&sa)&&GetLastError()==ERROR_ALREADY_EXISTS,"fresh-root collision refuses without repair");
    require(exact(root_pin.get(),structural.get())&&same(root_id,inspect(root_pin.get(),true)),"collision preserves descriptor and identity");
    const auto bin=root/L"Bin"; make_dir(bin,structural.get()); auto bin_pin=open_dir(bin); require(exact(bin_pin.get(),structural.get()),"fresh Bin exact final owner and ACL");
    for(auto leaf:mutable_dirs) { make_dir(root/leaf,content.get()); auto h=open_dir(root/leaf); inspect(h.get(),true); require(exact(h.get(),content.get()),"mutable boundary final descriptor; no content used elevated"); }
    publish(bin,program_names[0],package_bytes[0],structural.get(),L".trusted"); publish(bin,program_names[1],package_bytes[1],structural.get(),L".trusted");
    publish(bin,L"pair.commit","1",structural.get(),L".trusted"); require(matched(bin,1),"trusted main pair");
    program_matrix(root,structural.get());
    trace("Six durable cuts and conflict/retry/rollback complete.\r\n");
    require(exact(root_pin.get(),structural.get())&&exact(bin_pin.get(),structural.get())&&same(root_id,inspect(root_pin.get(),true)),"final bound program structure revalidation");
    write_new(root/L"probe-proof.txt","SFX O2 PROOF 1\r\nelevated_checks_before_marker="+std::to_string(checks)+
        "\r\nsix_durable_cuts=PASS\r\nconflict_retry_rollback=PASS\r\nbroader_gate=CONDITIONAL\r\n",structural.get());
    trace("Elevated phase PASS; total checks="+std::to_string(checks)+"\r\n");
    return 0;
}

struct Db { sqlite3* value=nullptr; explicit Db(const fs::path& p) { require(sqlite3_open16(p.c_str(),&value)==SQLITE_OK,"fixture SQLite open"); } ~Db(){if(value)sqlite3_close(value);} void close(){require(sqlite3_close(value)==SQLITE_OK,"database close");value=nullptr;} };
void sql(sqlite3* db,const char* s){require(sqlite3_exec(db,s,nullptr,nullptr,nullptr)==SQLITE_OK,"fixed fixture SQL");}
int value(sqlite3* db) { sqlite3_stmt* s=nullptr; require(sqlite3_prepare_v2(db,"SELECT value FROM proof;",-1,&s,nullptr)==SQLITE_OK,"read fixture value"); require(sqlite3_step(s)==SQLITE_ROW,"fixture row"); auto v=sqlite3_column_int(s,0); require(sqlite3_finalize(s)==SQLITE_OK,"statement finish");return v;}
void normal_compatibility(const fs::path& root) {
    for(auto leaf:mutable_dirs) { write_new(root/leaf/L"ordinary.txt","old"); write_new(root/leaf/L"ordinary.pending","new");
        require(MoveFileExW((root/leaf/L"ordinary.pending").c_str(),(root/leaf/L"ordinary.txt").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"unelevated atomic replacement"); require(read(root/leaf/L"ordinary.txt")=="new","published content"); }
    Db db(root/L"Data"/L"imported.db"); require(value(db.value)==7,"imported account stand-in retained");
    sql(db.value,"PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0; BEGIN IMMEDIATE; UPDATE proof SET value=9; ROLLBACK;");
    require(value(db.value)==7,"rollback"); sql(db.value,"UPDATE proof SET value=11;");
    for(auto suffix:{L"-wal",L"-shm"}) { Handle h(CreateFileW((root/L"Data"/(std::wstring(L"imported.db")+suffix)).c_str(),FILE_READ_ATTRIBUTES,share_all,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)); inspect(h.get(),false); }
    Db backup(root/L"Backups"/L"after-maintenance.db"); auto* b=sqlite3_backup_init(backup.value,"main",db.value,"main"); require(b!=nullptr,"backup start"); require(sqlite3_backup_step(b,-1)==SQLITE_DONE,"tiny backup complete");require(sqlite3_backup_finish(b)==SQLITE_OK,"backup finish"); require(value(backup.value)==11,"backup exact");backup.close();db.close();
    Db again(root/L"Data"/L"imported.db");require(value(again.value)==11,"database reopen");again.close();
}
std::wstring make_id() { SYSTEMTIME now{};GetSystemTime(&now); std::wostringstream s; s<<std::uppercase<<std::hex<<std::setfill(L'0')<<std::setw(8)<<GetCurrentProcessId()<<std::setw(8)<<GetTickCount()<<std::setw(8)<<(now.wDay*1000000U+now.wHour*10000U+now.wMinute*100U+now.wSecond);return s.str(); }
void preservation_controls(const fs::path& source) {
    const auto from=source/L"preservation-control";const auto prior=source/L"prior-untrusted-backup";
    make_dir(from);make_dir(prior);write_new(from/L"identity.txt","source");write_new(prior/L"identity.txt","prior backup");
    require(!MoveFileExW(from.c_str(),prior.c_str(),MOVEFILE_WRITE_THROUGH),"existing backup collision refuses");
    require(read(from/L"identity.txt")=="source"&&read(prior/L"identity.txt")=="prior backup","collision preserves both trees");
    auto blocked=open_dir(from);inspect(blocked.get(),true);
    const auto renamed=source/L"control-untrusted-backup";
    require(!MoveFileExW(from.c_str(),renamed.c_str(),MOVEFILE_WRITE_THROUGH)&&GetLastError()==ERROR_SHARING_VIOLATION,"own non-delete-sharing handle refuses relocation");
    require(read(from/L"identity.txt")=="source","refusal leaves original source intact");blocked.reset();
    require(MoveFileExW(from.c_str(),renamed.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"ordinary preservation retry after own handle closes");
    require(read(renamed/L"identity.txt")=="source","retried non-live backup preserved");
    std::cout<<"GROUP PASS: ordinary backup collision, sharing refusal, retry and preservation\n";
}
DWORD request_uac(const std::wstring& id) {
    std::array<wchar_t,32768> image{}; require(GetModuleFileNameW(nullptr,image.data(),static_cast<DWORD>(image.size()))!=0,"own probe path");
    const auto args=L"--elevated-proof "+id; SHELLEXECUTEINFOW request{};request.cbSize=sizeof(request);request.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_NOASYNC;request.lpVerb=L"runas";request.lpFile=image.data();request.lpParameters=args.c_str();request.nShow=SW_HIDE;
    if(!ShellExecuteExW(&request)) return GetLastError()==ERROR_CANCELLED?20:23;
    Handle child(request.hProcess); require(child.get()!=nullptr,"own elevated child handle");
    // Finite wait; no DNS/network/foreign process or arbitrary helper.
    require(WaitForSingleObject(child.get(),60000)==WAIT_OBJECT_0,"fixture child must complete within 60 seconds; leave evidence on failure");
    DWORD code=99;require(GetExitCodeProcess(child.get(),&code)!=FALSE,"child disposition");return code;
}
int normal(bool cancel) {
    auto t=token();require(!elevated(t.get()),"start ordinary, never Run as administrator");auto ordinary=limited(t.get());Anchor anchor(ordinary.get());
    auto id=make_id(); const auto root=anchor.root(id);const auto backup=fs::path(root.native()+L"-untrusted-backup");
    std::wcout<<L"Disposable canonical stand-in: C:\\xtest-sfx-o2-"<<id<<L"\n";
    make_dir(root);for(auto leaf:mutable_dirs)make_dir(root/leaf);make_dir(root/L"Bin");
    for(auto name:program_names)write_new(root/L"Bin"/name,source_program);
    write_new(root/L"owner-unknown-material.txt","preserve unknown owner material\r\n");
    for(auto leaf:mutable_dirs)write_new(root/leaf/L"source.txt","root-relative fixture data\r\n");
    write_new(root/L"Config"/L"ssh-identity.fixture","fixed dummy host identity, not a private key\r\n");
    {Db source(root/L"Data"/L"source.db");sql(source.value,"CREATE TABLE proof(value INTEGER); INSERT INTO proof VALUES(7);");source.close();}
    preservation_controls(root);
    auto original=open_dir(root,GENERIC_READ|READ_CONTROL,share_all);auto original_id=inspect(original.get(),true);
    // Rename is ordinary and no-replace. No elevated function accepts backup.
    require(MoveFileExW(root.c_str(),backup.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"ordinary no-overwrite untrusted-backup labeling before UAC");
    auto saved=open_dir(backup);require(same(original_id,inspect(saved.get(),true)),"preservation retains exact source directory identity");original.reset();
    std::wcout<<L"PRESERVED UNTRUSTED BACKUP: C:\\xtest-sfx-o2-"<<id<<L"-untrusted-backup\nDO NOT LAUNCH ITS EXECUTABLES.\n";
    std::cout<<(cancel?"Cancel the upcoming UAC prompt.\n":"Approve the upcoming UAC prompt for this defensive fixture only.\n")<<std::flush;
    const auto code=request_uac(id);
    std::cout<<"UAC launch/child result: "<<code<<"\n"<<std::flush;
    if(code>=10000) { try {std::cout<<read(root/L"probe-diagnostic.txt")<<std::flush;}catch(const std::exception&){std::cout<<"No readable protected diagnostic (failure may precede creation).\n";} }
    for(auto name:program_names)require(read(backup/L"Bin"/name)==source_program,"adopted Bin remains different and unchanged");
    require(read(backup/L"owner-unknown-material.txt")=="preserve unknown owner material\r\n","unknown owner material preserved");
    if(cancel) {require(code==20,"UAC cancellation must be observed, not inferred"); require(GetFileAttributesW(root.c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,"cancel leaves new canonical root absent");std::cout<<"CANCELLATION CONTROL PASS; source remains labeled untrusted backup. Checks="<<checks<<"\n";return 0;}
    require(code==0,"elevated fresh proof succeeded; refusal/cancel is not a pass");
    auto structural=descriptor(sid(ordinary.get()),false);auto content=descriptor(sid(ordinary.get()),true);
    auto new_root=open_dir(root); auto bin=open_dir(root/L"Bin");require(!same(original_id,inspect(new_root.get(),true)),"new canonical root has a different identity");require(exact(new_root.get(),structural.get())&&exact(bin.get(),structural.get()),"root/Bin settled exact policy");
    for(const auto& p:{root,root/L"Bin"})for(DWORD access:{DWORD(WRITE_DAC),DWORD(WRITE_OWNER),DWORD(DELETE),DWORD(FILE_DELETE_CHILD),DWORD(FILE_ADD_FILE),DWORD(FILE_ADD_SUBDIRECTORY)}) {
        auto denied=open_dir(p,access,share_all);require(denied.get()==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_ACCESS_DENIED,"ordinary direct and parent-mediated structural authority denied"); }
    for(auto name:program_names) {
        const auto path=root/L"Bin"/name;Handle h(CreateFileW(path.c_str(),GENERIC_READ|READ_CONTROL,share_all,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        inspect(h.get(),false);require(exact(h.get(),structural.get()),"settled executable exact owner and ACL");
        for(DWORD access:{DWORD(WRITE_DAC),DWORD(WRITE_OWNER),DWORD(DELETE),DWORD(FILE_WRITE_DATA)}) {
            Handle denied(CreateFileW(path.c_str(),access,share_all,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            require(denied.get()==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_ACCESS_DENIED,"ordinary program modification and replacement rights denied");
        }
    }
    require(matched(root/L"Bin",1),"new Bin exclusively matches independently trusted package bytes, never source");
    const auto proof=read(root/L"probe-proof.txt");
    require(proof.rfind("SFX O2 PROOF 1\r\nelevated_checks_before_marker=",0)==0&&
        proof.find("\r\nsix_durable_cuts=PASS\r\nconflict_retry_rollback=PASS\r\nbroader_gate=CONDITIONAL\r\n")!=std::string::npos,
        "protected elevated completion marker verified after child exit");
    std::cout<<proof<<"GROUP PASS: fresh identities, final ACLs, structural denials and trusted program bytes\n";
    for(auto leaf:mutable_dirs) { auto h=open_dir(root/leaf);require(exact(h.get(),content.get()),"settled mutable boundary"); if(wcscmp(leaf,L"Work")) require(CopyFileW((backup/leaf/L"source.txt").c_str(),(root/leaf/L"imported.txt").c_str(),TRUE)!=FALSE,"unelevated data import to new destination"); }
    require(CopyFileW((backup/L"Data"/L"source.db").c_str(),(root/L"Data"/L"imported.db").c_str(),TRUE)!=FALSE,"ordinary closed database copy-first import");
    require(CopyFileW((backup/L"Config"/L"ssh-identity.fixture").c_str(),(root/L"Config"/L"ssh-identity.fixture").c_str(),TRUE)!=FALSE,"ordinary fixture host-identity import");
    require(read(root/L"Config"/L"ssh-identity.fixture")==read(backup/L"Config"/L"ssh-identity.fixture"),"host identity bytes retained");
    normal_compatibility(root);require(matched(root/L"Bin",1)&&exact(new_root.get(),structural.get())&&exact(bin.get(),structural.get()),"ordinary work does not weaken protected program set");
    std::cout<<"GROUP PASS: ordinary import, eight atomic saves, SQLite WAL/SHM/rollback/backup/reopen\n"
        <<"NATIVE PROBE PASS: "<<checks<<" ordinary checks; protected elevated completion marker verified.\n"
        <<"FEASIBILITY REMAINS CONDITIONAL: no production, real SFX import, independent-process update provenance, adversarial namespace/race matrix or power-loss proof.\n"
        <<"All fixture roots and untrusted backups remain; no automatic deletion.\n";
    return 0;
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc==3&&std::wstring(argv[1])==L"--elevated-proof"&&valid_id(argv[2]))return privileged(argv[2]);
        require(argc==1||(argc==2&&std::wstring(argv[1])==L"--cancel"),"no arbitrary paths or operations accepted");return normal(argc==2);
    }catch(const std::exception& e){trace("FAIL after "+std::to_string(checks)+" checks: "+e.what()+"\r\n");
        std::cerr<<"PROBE REFUSAL/FAILURE after "<<checks<<" checks: "<<e.what()<<"\nSource and fixture evidence retained.\n";
        return argc==3?static_cast<int>(10000+checks):30;}
}
