#include <winsock2.h>
#include "alpha_ssh_stream.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/backup_read_access.hpp"
#include <Aclapi.h>
#include <sddl.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
namespace fs=std::filesystem;namespace win=sfx::platform::win32;namespace runtime=sfx::apps::runtime;
unsigned checks{};void check(bool yes,const char* why){++checks;if(!yes)throw std::runtime_error(why);}
int wmain(int argc,wchar_t** argv)try{
    check(argc==2,"fresh isolated key fixture required");const auto root=fs::absolute(argv[1]);check(!fs::exists(root),"preserve earlier key fixtures");
    const auto owner=win::current_process_user_sid_string();fs::create_directories(root/L"Config");
    const win::WindowsStorageDaclPolicy policy(owner);
    check(policy.apply(root,win::StorageDaclObjectKind::directory).succeeded()&&
        policy.apply(root/L"Config",win::StorageDaclObjectKind::directory).succeeded(),"protect fresh isolated key fixture objects");
    const std::wstring reader=L"S-1-5-21-314159-265358-979323-1901";
    const auto sddl=L"O:"+owner+L"D:P(A;OICI;0x1301ff;;;"+owner+L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;"+reader+L")";
    void* raw{};check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&raw,nullptr),"encode trusted Config reader");
    PACL acl{};BOOL present{},defaulted{};check(GetSecurityDescriptorDacl(raw,&present,&acl,&defaulted)&&present&&acl,"read trusted Config DACL");
    auto config=(root/L"Config").native();const auto status=SetNamedSecurityInfoW(config.data(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,acl,nullptr);LocalFree(raw);
    check(status==ERROR_SUCCESS,"set only synthetic Config reader");
    std::string diagnostic;auto backend=runtime::AlphaSshBackend::acquire(root,owner,diagnostic);
    if(!backend)std::cerr<<"key_diagnostic="<<diagnostic<<'\n';check(bool(backend),"actual native host-key generation/protection");
    const auto key=root/L"Config"/L"ssh_host_ed25519.pem";
    auto readers=win::capture_backup_read_grants(key,owner,false);bool found{};
    PSID selected{};check(ConvertStringSidToSidW(reader.c_str(),&selected),"parse independent reader SID");
    for(const auto& entry:readers){const auto* ace=reinterpret_cast<const ACCESS_ALLOWED_ACE*>(entry.data());
        if(ace->Header.AceType==ACCESS_ALLOWED_ACE_TYPE&&EqualSid(const_cast<DWORD*>(&ace->SidStart),selected)){
            found=(ace->Mask&FILE_GENERIC_READ)==FILE_GENERIC_READ&&!(ace->Mask&(FILE_WRITE_DATA|FILE_APPEND_DATA|DELETE|WRITE_DAC|WRITE_OWNER));}}
    LocalFree(selected);check(found,"trusted reader survives actual native key-writer ACL replacement");
    check(runtime::AlphaSshBackend::statistics().key_generation_attempts==1,"single actual key generation");
    backend.reset();auto loaded=runtime::AlphaSshBackend::acquire(root,owner,diagnostic);check(bool(loaded),"existing protected key reloads");
    check(win::capture_backup_read_grants(key,owner,false)==readers,"existing-key load preserves reader permissions");
    check(runtime::AlphaSshBackend::statistics().key_generation_attempts==1,"reload does not replace key");
    std::cout<<"PASS "<<checks<<" actual native SSH-key generation/reload reader persistence; no listener, Runtime or service started\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}
