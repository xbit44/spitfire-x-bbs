#include "sfx/platform/win32/service_file_owner.hpp"
#include <windows.h>
#include <sddl.h>
#include <mutex>
#include <stdexcept>
#include <vector>
namespace sfx::platform::win32 {
namespace {
std::mutex activation_mutex;
std::wstring bound_owner;
bool preparation_worker{};
struct Token {HANDLE value{};~Token(){if(value)CloseHandle(value);}};
bool system_token(HANDLE token) {
    DWORD size{};GetTokenInformation(token,TokenUser,nullptr,0,&size);
    if(!size||size>65536)return false;
    std::vector<unsigned char> data(size);
    if(!GetTokenInformation(token,TokenUser,data.data(),size,&size))return false;
    BYTE sid[SECURITY_MAX_SID_SIZE];DWORD length=sizeof(sid);
    return CreateWellKnownSid(WinLocalSystemSid,nullptr,sid,&length)&&
        EqualSid(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,sid);
}
bool elevated_token(HANDLE token){TOKEN_ELEVATION value{};DWORD size{};return GetTokenInformation(token,TokenElevation,&value,sizeof(value),&size)&&value.TokenIsElevated;}
bool same_user(HANDLE a,HANDLE b){DWORD na{},nb{};GetTokenInformation(a,TokenUser,nullptr,0,&na);GetTokenInformation(b,TokenUser,nullptr,0,&nb);if(!na||!nb||na>65536||nb>65536)return false;std::vector<unsigned char> left(na),right(nb);return GetTokenInformation(a,TokenUser,left.data(),na,&na)&&GetTokenInformation(b,TokenUser,right.data(),nb,&nb)&&EqualSid(reinterpret_cast<TOKEN_USER*>(left.data())->User.Sid,reinterpret_cast<TOKEN_USER*>(right.data())->User.Sid);}
}
void activate_service_file_owner(const std::wstring& owner) {
    Token process;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&process.value)||!system_token(process.value))
        throw std::runtime_error("service_owner_requires_system");
    PSID sid{};
    if(!ConvertStringSidToSidW(owner.c_str(),&sid))throw std::runtime_error("service_owner_sid_invalid");
    const bool valid=IsValidSid(sid)!=FALSE;LocalFree(sid);
    if(!valid)throw std::runtime_error("service_owner_sid_invalid");
    std::lock_guard<std::mutex> lock(activation_mutex);
    if(!bound_owner.empty()&&bound_owner!=owner)throw std::runtime_error("service_owner_already_bound");
    bound_owner=owner;
}
void activate_preparation_file_owner(const std::wstring& owner){
    Token process;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&process.value)||!elevated_token(process.value)||system_token(process.value))throw std::runtime_error("preparation_owner_requires_consented_admin_worker");
    PSID sid{};if(!ConvertStringSidToSidW(owner.c_str(),&sid))throw std::runtime_error("preparation_owner_sid_invalid");const bool valid=IsValidSid(sid)!=FALSE;LocalFree(sid);if(!valid)throw std::runtime_error("preparation_owner_sid_invalid");
    std::lock_guard<std::mutex> lock(activation_mutex);if(!bound_owner.empty())throw std::runtime_error("preparation_owner_already_bound");bound_owner=owner;preparation_worker=true;
}
bool service_file_owner_enabled(const std::wstring& owner) noexcept {
    try{std::lock_guard<std::mutex> lock(activation_mutex);return !bound_owner.empty()&&bound_owner==owner;}
    catch(...){return false;}
}
ServiceFileOwnerScope::ServiceFileOwnerScope(const std::wstring& owner) noexcept
    : ServiceFileOwnerScope(owner,false) {}
ServiceFileOwnerScope::ServiceFileOwnerScope(const std::wstring& owner,const bool read_metadata) noexcept {
    try {
        bool preparation{};{std::lock_guard<std::mutex> lock(activation_mutex);
            if(bound_owner.empty()||bound_owner!=owner){valid_=true;return;}preparation=preparation_worker;}
        Token process,prior,scoped;
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_DUPLICATE,&process.value)||!(preparation?elevated_token(process.value):system_token(process.value)))return;
        if(!OpenThreadToken(GetCurrentThread(),TOKEN_QUERY|TOKEN_IMPERSONATE,TRUE,&prior.value)) {
            if(GetLastError()!=ERROR_NO_TOKEN)return;
        }else if(!(preparation?(elevated_token(prior.value)&&same_user(prior.value,process.value)):system_token(prior.value)))return; // Never elevate a caller's impersonation.
        if(!DuplicateTokenEx(process.value,TOKEN_QUERY|TOKEN_ADJUST_PRIVILEGES|TOKEN_IMPERSONATE,nullptr,
            SecurityImpersonation,TokenImpersonation,&scoped.value))return;
        TOKEN_PRIVILEGES privileges{};privileges.PrivilegeCount=1;
        if(!LookupPrivilegeValueW(nullptr,SE_RESTORE_NAME,&privileges.Privileges[0].Luid))return;
        privileges.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;SetLastError(ERROR_SUCCESS);
        if(!AdjustTokenPrivileges(scoped.value,FALSE,&privileges,0,nullptr,nullptr)||GetLastError()!=ERROR_SUCCESS)return;
        if(preparation||read_metadata){
            // Current-copy metadata/read preparation only; primary and Door
            // tokens never receive either backup/restore privilege. The
            // attested service requests read_metadata only while capturing
            // owner-only writer ACLs for a managed publication.
            if(!LookupPrivilegeValueW(nullptr,SE_BACKUP_NAME,&privileges.Privileges[0].Luid))return;
            SetLastError(ERROR_SUCCESS);
            if(!AdjustTokenPrivileges(scoped.value,FALSE,&privileges,0,nullptr,nullptr)||GetLastError()!=ERROR_SUCCESS)return;
        }
        if(!SetThreadToken(nullptr,scoped.value))return;
        previous_=prior.value;prior.value=nullptr;changed_=true;valid_=true;
    }catch(...){valid_=false;}
}
ServiceFileOwnerScope::~ServiceFileOwnerScope() {
    if(changed_&&!SetThreadToken(nullptr,static_cast<HANDLE>(previous_)))RevertToSelf();
    if(previous_)CloseHandle(static_cast<HANDLE>(previous_));
}
}
