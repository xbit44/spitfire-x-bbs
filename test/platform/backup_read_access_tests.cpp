#include "sfx/platform/win32/backup_read_access.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/service_file_owner.hpp"
#include <Aclapi.h>
#include <authz.h>
#include <sddl.h>
#include <filesystem>
#include <fstream>
#include <array>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace win = sfx::platform::win32;
unsigned checks{};
void check(bool yes, const char* label) { ++checks; if (!yes) throw std::runtime_error(label); }
const std::wstring reader=L"S-1-5-21-314159-265358-979323-1901";
const std::wstring denied=L"S-1-5-21-314159-265358-979323-1902";
const std::wstring session=L"S-1-5-5-0-271478";
struct Close { HANDLE value; ~Close() { if(value!=INVALID_HANDLE_VALUE)CloseHandle(value); } };
HANDLE open(const fs::path& path, bool directory=false) {
    const auto handle=CreateFileW(path.c_str(),MAXIMUM_ALLOWED,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_OVERLAPPED|(directory?FILE_FLAG_BACKUP_SEMANTICS:0),nullptr);
    if(handle==INVALID_HANDLE_VALUE)std::wcerr<<L"metadata_open="<<path.native()<<L" error="<<GetLastError()<<L'\n';
    check(handle!=INVALID_HANDLE_VALUE,"open exact fixture metadata");return handle;
}
std::wstring rules(const std::wstring& owner, bool directory=false) {
    const auto flags=directory?L"OICI":L"";
    return L"(A;"+std::wstring(flags)+L";0x1301ff;;;"+owner+L")(A;"+flags+L";FA;;;SY)(A;"+flags+L";FA;;;BA)";
}
void set_acl_handle(HANDLE handle,const std::wstring& sddl,bool protect=true) {
    void* descriptor{};const auto encoded=ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr);
    if(!encoded)std::wcerr<<L"fixture_sddl="<<sddl<<L" error="<<GetLastError()<<L'\n';
    check(encoded,"encode fixture ACL");
    PACL acl{};BOOL present{},defaulted{};check(GetSecurityDescriptorDacl(descriptor,&present,&acl,&defaulted)&&present&&acl,"get fixture DACL");
    const auto status=SetSecurityInfo(handle,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|
        (protect?PROTECTED_DACL_SECURITY_INFORMATION:UNPROTECTED_DACL_SECURITY_INFORMATION),nullptr,nullptr,acl,nullptr);
    LocalFree(descriptor);check(status==ERROR_SUCCESS,"set only fixture DACL");
}
void set_acl(const fs::path& path,const std::wstring& sddl,bool protect=true) {
    Close held{open(path,fs::is_directory(path))};set_acl_handle(held.value,sddl,protect);
}
std::wstring acl_text(const fs::path& path) { Close held{open(path,fs::is_directory(path))};return win::BackupDaclSnapshot(held.value).sddl(); }
bool access(const fs::path& path,const std::wstring& sid,DWORD wanted) {
    void* descriptor{};PACL acl{};check(GetNamedSecurityInfoW(path.c_str(),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
        nullptr,nullptr,&acl,nullptr,&descriptor)==ERROR_SUCCESS,"inspect actual access descriptor");
    GENERIC_MAPPING mapping{FILE_GENERIC_READ,FILE_GENERIC_WRITE,FILE_GENERIC_EXECUTE,FILE_ALL_ACCESS};
    for(DWORD n=0;n<acl->AceCount;++n){void* raw{};check(GetAce(acl,n,&raw),"read actual access entry");auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);MapGenericMask(&ace->Mask,&mapping);}
    PSID identity{};check(ConvertStringSidToSidW(sid.c_str(),&identity),"parse synthetic reader SID");
    AUTHZ_RESOURCE_MANAGER_HANDLE manager{};check(AuthzInitializeResourceManager(AUTHZ_RM_FLAG_NO_AUDIT,nullptr,nullptr,nullptr,L"SFX reader test",&manager),"create SID-only access evaluator");
    AUTHZ_CLIENT_CONTEXT_HANDLE context{};LUID id{};check(AuthzInitializeContextFromSid(AUTHZ_SKIP_TOKEN_GROUPS,identity,manager,nullptr,id,nullptr,&context),"create SID-only reader context");
    AUTHZ_ACCESS_REQUEST request{};request.DesiredAccess=wanted;DWORD granted{},error{};AUTHZ_ACCESS_REPLY reply{1,&granted,nullptr,&error};
    const auto checked=AuthzAccessCheck(0,context,&request,nullptr,descriptor,nullptr,0,&reply,nullptr);
    AuthzFreeContext(context);AuthzFreeResourceManager(manager);LocalFree(identity);LocalFree(descriptor);
    check(checked,"evaluate actual ACL");return error==ERROR_SUCCESS&&(granted&wanted)==wanted;
}
void write(const fs::path& path) { std::ofstream out(path,std::ios::binary);out<<"SFX synthetic offline backup fixture\r\n";check(out.good(),"write fresh fixture"); }
std::vector<unsigned char> process_privileges(){
    HANDLE token{};check(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token),"query primary process privileges");Close held{token};DWORD bytes{};
    GetTokenInformation(token,TokenPrivileges,nullptr,0,&bytes);std::vector<unsigned char> result(bytes);
    check(bytes&&GetTokenInformation(token,TokenPrivileges,result.data(),bytes,&bytes),"read primary privilege snapshot");return result;
}
bool enabled(HANDLE token,const wchar_t* name){LUID selected{};check(LookupPrivilegeValueW(nullptr,name,&selected),"resolve privilege for assertion");DWORD bytes{};
    GetTokenInformation(token,TokenPrivileges,nullptr,0,&bytes);std::vector<unsigned char> data(bytes);
    check(bytes&&GetTokenInformation(token,TokenPrivileges,data.data(),bytes,&bytes),"read scoped privilege flags");const auto* list=reinterpret_cast<const TOKEN_PRIVILEGES*>(data.data());
    for(DWORD n=0;n<list->PrivilegeCount;++n)if(list->Privileges[n].Luid.LowPart==selected.LowPart&&list->Privileges[n].Luid.HighPart==selected.HighPart)return (list->Privileges[n].Attributes&SE_PRIVILEGE_ENABLED)!=0;return false;
}
int wmain(int argc,wchar_t** argv)try {
    if(argc==3&&std::wstring(argv[2])==L"--reparse-check"){
        const auto path=fs::absolute(argv[1]);const auto owner=win::current_process_user_sid_string();
        const auto attributes=GetFileAttributesW(path.c_str());
        check(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"real fixture reparse point required");
        bool reader_refused{},publication_refused{};
        try{(void)win::capture_backup_read_grants(path,owner,true);}catch(const std::exception&){reader_refused=true;}
        try{(void)win::capture_backup_publication_access(path,owner,true);}catch(const std::exception&){publication_refused=true;}
        check(reader_refused&&publication_refused,"reader/publication authority refuses actual reparse source");
        std::cout<<"PASS "<<checks<<" actual junction authority-boundary refusal checks\n";return 0;
    }
    check(argc==2||(argc==3&&std::wstring(argv[2])==L"--preparation"),"fresh output root/closed mode required");const auto root=fs::absolute(argv[1]);check(!fs::exists(root),"preserve earlier fixtures");fs::create_directories(root);
    const auto owner=win::current_process_user_sid_string();const win::WindowsStorageDaclPolicy policy(owner);
    const auto primary_before=process_privileges();
    if(argc==3){
        win::activate_preparation_file_owner(owner);const auto key=root/L"owner-only-metadata.key";
        {win::ServiceFileOwnerScope scope(owner,true);check(scope.valid()&&scope.active(),"attested preparation metadata thread scope active");
            HANDLE token{};check(OpenThreadToken(GetCurrentThread(),TOKEN_QUERY,TRUE,&token),"query scoped impersonation token");Close held{token};
            check(enabled(token,SE_BACKUP_NAME)&&enabled(token,SE_RESTORE_NAME),"metadata privileges enabled only on scoped thread");
            check(process_privileges()==primary_before,"primary token unchanged while metadata scope active");
            void* descriptor{};check(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:SYD:P(A;;FA;;;SY)",SDDL_REVISION_1,&descriptor,nullptr),"encode isolated System-only metadata edge");
            SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};Close created{CreateFileW(key.c_str(),GENERIC_WRITE|READ_CONTROL,FILE_SHARE_READ,&security,CREATE_NEW,FILE_FLAG_BACKUP_SEMANTICS,nullptr)};LocalFree(descriptor);
            check(created.value!=INVALID_HANDLE_VALUE,"scoped restore authority creates exact private edge fixture");
            check(win::capture_backup_read_grants(key,owner,false).empty(),"scoped metadata read succeeds but foreign owner cannot nominate readers");
        }
        check(process_privileges()==primary_before,"primary privileges unchanged after scope");HANDLE thread{};
        const auto impersonating=OpenThreadToken(GetCurrentThread(),TOKEN_QUERY,TRUE,&thread);if(impersonating)CloseHandle(thread);
        check(!impersonating&&GetLastError()==ERROR_NO_TOKEN,"prior no-impersonation state restored");
        Close denied_open{CreateFileW(key.c_str(),READ_CONTROL,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr)};
        check(denied_open.value==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_ACCESS_DENIED,"metadata authority does not survive the scope");
        std::ofstream result(root/L"metadata-scope-result.txt");result<<"PASS "<<checks<<" actual elevated preparation metadata scope; primary token unchanged; authority removed; actual LocalSystem execution not claimed\n";
        return 0;
    }
    bool service_refused{};try{win::activate_service_file_owner(owner);}catch(const std::exception&){service_refused=true;}
    check(service_refused&&!win::service_file_owner_enabled(owner),"ordinary caller cannot activate attested System service authority");
    {win::ServiceFileOwnerScope scope(owner,true);check(scope.valid()&&!scope.active(),"ordinary metadata scope grants no impersonation privilege");}
    check(process_privileges()==primary_before,"ordinary metadata scope leaves process privileges unchanged");
    check(policy.apply(root,win::StorageDaclObjectKind::directory).succeeded(),"establish fixture owner policy");
    const auto file=root/L"existing.txt";write(file);
    set_acl(file,L"D:P(D;;FR;;;"+denied+L")"+rules(owner)+L"(A;;FR;;;"+reader+L")(A;;FR;;;"+session+L")(A;;FR;;;BU)");
    check(policy.apply(file,win::StorageDaclObjectKind::file).succeeded(),"preserve trusted stable read grant");
    check(access(file,reader,FILE_GENERIC_READ),"delegated reader still reads");
    for(const DWORD forbidden:std::array<DWORD,5>{FILE_WRITE_DATA,FILE_APPEND_DATA,WRITE_DAC,WRITE_OWNER,DELETE})
        check(!access(file,reader,forbidden),"reader receives no write/control right");
    check(!access(file,denied,FILE_GENERIC_READ),"custom reader denial retained");
    check(acl_text(file).find(session)==std::wstring::npos&&acl_text(file).find(L";;;BU)")==std::wstring::npos,"transient and broad grants not carried");
    set_acl(root,L"D:P"+rules(owner,true)+L"(A;OICI;0x1200a9;;;"+reader+L")");
    const auto excluded=root/L"protected.txt";write(excluded);set_acl(excluded,L"D:P"+rules(owner));const auto original=acl_text(excluded);
    const auto pending=root/L"protected.pending";write(pending);
    check(policy.apply_for_publication(pending,win::StorageDaclObjectKind::file,excluded).succeeded(),"prepare protected logical replacement");
    check(!access(pending,reader,FILE_GENERIC_READ),"protected old target excludes parent reader on replacement");
    check(acl_text(excluded)==original,"preparation leaves prior target permissions unchanged");
    const auto new_file=root/L"new.pending";write(new_file);
    check(policy.apply_for_publication(new_file,win::StorageDaclObjectKind::file,root/L"absent-final").succeeded(),"new logical publication follows trusted parent policy");
    check(access(new_file,reader,FILE_GENERIC_READ),"new publication receives intended parent reader");
    const auto delegated=root/L"delegated.pending";write(delegated);
    check(policy.apply_for_publication(delegated,win::StorageDaclObjectKind::file,file).succeeded(),"prepare delegated-reader replacement");
    check(access(delegated,reader,FILE_GENERIC_READ)&&!access(delegated,denied,FILE_GENERIC_READ),"prior target read and denial survive replacement");
    const auto stranded=root/L"session-only.txt";write(stranded);
    // A legitimate already-authorized metadata handle models Setup's repair
    // authority; this test does not grant a reader permission to repair ACLs.
    Close stranded_metadata{open(stranded)};
    set_acl_handle(stranded_metadata.value,L"D:P(D;;FR;;;"+denied+L")(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;"+session+L")");
    const auto stranded_before=win::BackupDaclSnapshot(stranded_metadata.value).sddl();
    {win::BackupDaclSnapshot before(stranded_metadata.value);win::ensure_backup_owner_read(stranded_metadata.value,owner,false);
        check(access(stranded,owner,FILE_GENERIC_READ),"owner reads using SID-only context without logon or Admin group");
        check(!access(stranded,denied,FILE_GENERIC_READ),"repair preserves custom denial");
        check(acl_text(stranded).find(session)!=std::wstring::npos,"repair preserves original session ACE");
        before.restore(stranded_metadata.value);}
    check(win::BackupDaclSnapshot(stranded_metadata.value).sddl()==stranded_before,"DACL rollback restores exact original descriptor text");
    for(const auto& denied_identity:{owner,std::wstring(L"S-1-1-0"),std::wstring(L"S-1-5-32-545")}) {
        set_acl_handle(stranded_metadata.value,L"D:P(D;;FR;;;"+denied_identity+L")"+rules(owner));const auto before=win::BackupDaclSnapshot(stranded_metadata.value).sddl();bool refused{};
        try {win::ensure_backup_owner_read(stranded_metadata.value,owner,false);}catch(const std::exception&){refused=true;}
        check(refused&&win::BackupDaclSnapshot(stranded_metadata.value).sddl()==before,"direct/broad/group owner-read deny refused without mutation");
    }
    const auto noncanonical=root/L"noncanonical.txt";write(noncanonical);
    set_acl(noncanonical,L"D:P"+rules(owner)+L"(D;;FR;;;"+denied+L")");const auto noncanonical_before=acl_text(noncanonical);
    check(!policy.apply(noncanonical,win::StorageDaclObjectKind::file).succeeded(),"noncanonical custom source fails closed");
    check(acl_text(noncanonical)==noncanonical_before,"noncanonical original order preserved");
    const auto linked=root/L"linked.txt";check(CreateHardLinkW(linked.c_str(),file.c_str(),nullptr),"create isolated hardlink negative");bool refused_link{};
    try {(void)win::capture_backup_read_grants(file,owner,false);}catch(const std::exception&){refused_link=true;}
    check(refused_link,"hardlinked authorization source rejected");
    const auto foreign=win::capture_backup_read_grants(excluded,reader,false);check(foreign.empty(),"foreign-owner object cannot nominate trusted readers");
    void* expected{};check(ConvertStringSecurityDescriptorToSecurityDescriptorW((L"O:"+owner+L"D:P"+rules(owner,true)).c_str(),SDDL_REVISION_1,&expected,nullptr),"encode expected parent boundary");
    {Close held{open(root,true)};check(win::backup_boundary_with_readers(held.value,expected,owner),"custom reader accepted with every required parent ACE");}
    set_acl(root,L"D:P(A;OICI;0x1301ff;;;"+owner+L")(A;OICI;0x1301ff;;;"+owner+L")(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;"+reader+L")");
    const auto malformed_before=acl_text(root);
    {Close held{open(root,true)};check(!win::backup_boundary_with_readers(held.value,expected,owner),"duplicate owner cannot stand in for missing SYSTEM ACE");}
    check(acl_text(root)==malformed_before,"malformed boundary refusal changes no permissions");LocalFree(expected);
    std::cout<<"PASS "<<checks<<" backup reader, protected publication, stable-owner, deny, rollback and scope checks; synthetic SID contexts are not distinct SMB logins\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';
    if(argc==3){std::ofstream result(fs::path(argv[1])/L"metadata-scope-result.txt");result<<"FAIL "<<checks<<": "<<error.what()<<'\n';}return 1;}
