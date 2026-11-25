#include "sfx/platform/win32/storage_access.hpp"
#include <windows.h>
#include <aclapi.h>
#include <authz.h>
#include <memory>
#include <vector>
namespace sfx::platform::win32 {
namespace {
StorageDaclAssessment assess_access(const std::filesystem::path& path,StorageDaclObjectKind kind,bool mutable_content){
    const auto fail=[](const char* why,bool private_access=false,bool effective_access=false){return StorageDaclAssessment{{application::StorageErrorCode::permission_denied,why},private_access,effective_access};};
    if(!path.is_absolute())return fail("current storage path must be absolute");
    const auto handle=CreateFileW(path.c_str(),READ_CONTROL|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|(kind==StorageDaclObjectKind::directory?FILE_FLAG_BACKUP_SEMANTICS:0),nullptr);
    if(handle==INVALID_HANDLE_VALUE)return fail("read current storage object and access rules");
    struct Close {HANDLE value;~Close(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}} object{handle};BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(handle,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0)!=(kind==StorageDaclObjectKind::directory))return fail("storage object type or redirection invalid");
    PACL acl{};PSECURITY_DESCRIPTOR descriptor{};if(GetSecurityInfo(handle,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,nullptr,nullptr,&acl,nullptr,&descriptor)!=ERROR_SUCCESS||!descriptor||!acl)return fail("storage has no usable private access rules");
    struct Free {void* value;~Free(){if(value)LocalFree(value);}} free{descriptor};
    HANDLE primary{},impersonation{};if(!OpenThreadToken(GetCurrentThread(),TOKEN_QUERY|TOKEN_DUPLICATE,TRUE,&primary)){
        if(GetLastError()!=ERROR_NO_TOKEN||!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_DUPLICATE,&primary))return fail("read current Windows access token");
    }Close primary_token{primary};
    if(!DuplicateToken(primary,SecurityImpersonation,&impersonation))return fail("evaluate current Windows access token");Close token{impersonation};
    GENERIC_MAPPING mapping{FILE_GENERIC_READ,FILE_GENERIC_WRITE,FILE_GENERIC_EXECUTE,FILE_ALL_ACCESS};DWORD wanted=mutable_content?(FILE_GENERIC_READ|FILE_GENERIC_WRITE|DELETE):(FILE_GENERIC_READ|FILE_GENERIC_EXECUTE);MapGenericMask(&wanted,&mapping);
    std::vector<unsigned char> privileges(2048);DWORD privilege_bytes=static_cast<DWORD>(privileges.size()),granted{};BOOL allowed{};
    if(!AccessCheck(descriptor,impersonation,wanted,&mapping,reinterpret_cast<PRIVILEGE_SET*>(privileges.data()),&privilege_bytes,&granted,&allowed)||!allowed)return fail(mutable_content?"current Windows account lacks needed storage read/write/delete access":"current Windows account lacks needed program read/execute access");
    // Authz's local SID-only contexts avoid account/group enumeration and
    // support inherited deny ACEs. GetEffectiveRightsFromAcl cannot do that.
    // Check explicit broad-principal grants independently of the current token.
    std::vector<unsigned char> mapped_acl(acl->AclSize);CopyMemory(mapped_acl.data(),acl,acl->AclSize);auto* checked_acl=reinterpret_cast<PACL>(mapped_acl.data());
    for(DWORD index=0;index<checked_acl->AceCount;++index){void* raw{};if(!GetAce(checked_acl,index,&raw))return fail("read current storage access entry",false,true);auto* header=static_cast<ACE_HEADER*>(raw);
        if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE&&header->AceType!=ACCESS_DENIED_ACE_TYPE)return fail("storage uses an access entry requiring separate review",false,true);
        auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);MapGenericMask(&ace->Mask,&mapping);
    }
    SECURITY_DESCRIPTOR checked{};PSID original_owner{};BOOL defaulted{};
    if(!InitializeSecurityDescriptor(&checked,SECURITY_DESCRIPTOR_REVISION)||!GetSecurityDescriptorOwner(descriptor,&original_owner,&defaulted)||!SetSecurityDescriptorOwner(&checked,original_owner,FALSE)||!SetSecurityDescriptorDacl(&checked,TRUE,checked_acl,FALSE))return fail("evaluate private storage access",false,true);
    AUTHZ_RESOURCE_MANAGER_HANDLE manager{};if(!AuthzInitializeResourceManager(AUTHZ_RM_FLAG_NO_AUDIT,nullptr,nullptr,nullptr,L"SFX private storage access",&manager))return fail("evaluate private storage access",false,true);
    struct Resource {AUTHZ_RESOURCE_MANAGER_HANDLE value;~Resource(){AuthzFreeResourceManager(value);}} resource{manager};bool private_access=true;
    for(const auto type:{WinWorldSid,WinAuthenticatedUserSid,WinBuiltinUsersSid,WinBuiltinGuestsSid,WinAnonymousSid}){
        DWORD size=SECURITY_MAX_SID_SIZE;std::vector<unsigned char> sid(size);if(!CreateWellKnownSid(type,nullptr,sid.data(),&size))return fail("evaluate shared Windows storage access",false,true);
        AUTHZ_CLIENT_CONTEXT_HANDLE context{};LUID id{};if(!AuthzInitializeContextFromSid(AUTHZ_SKIP_TOKEN_GROUPS,sid.data(),manager,nullptr,id,nullptr,&context))return fail("evaluate shared Windows storage access",false,true);
        struct Context {AUTHZ_CLIENT_CONTEXT_HANDLE value;~Context(){AuthzFreeContext(value);}} holder{context};
        AUTHZ_ACCESS_REQUEST request{};request.DesiredAccess=MAXIMUM_ALLOWED;ACCESS_MASK access{};DWORD error{},sacl{};AUTHZ_ACCESS_REPLY reply{1,&access,&sacl,&error};
        if(!AuthzAccessCheck(0,context,&request,nullptr,&checked,nullptr,0,&reply,nullptr)||(error!=ERROR_SUCCESS&&error!=ERROR_ACCESS_DENIED))return fail("evaluate shared Windows storage access",false,true);
        const DWORD forbidden=FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_DELETE_CHILD|DELETE|WRITE_DAC|WRITE_OWNER|(mutable_content?FILE_READ_DATA:0);
        if(access&forbidden)private_access=false;
    }
    if(!private_access)return fail(mutable_content?"shared Windows accounts can read or change private SFX storage":"shared Windows accounts can change SFX programs or directories",false,true);
    return {application::StorageStatus::success(),true,true};
}
}
StorageDaclAssessment assess_current_storage_access(const std::filesystem::path& path,StorageDaclObjectKind kind){return assess_access(path,kind,true);}
StorageDaclAssessment assess_current_program_access(const std::filesystem::path& path,StorageDaclObjectKind kind){return assess_access(path,kind,false);}
}
