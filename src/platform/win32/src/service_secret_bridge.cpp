#include "sfx/platform/win32/service_secret_bridge.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/backup_read_access.hpp"
#include "sfx/platform/win32/storage_path_policy.hpp"
#include "sfx/platform/win32/ftn_secrets.hpp"
#include "sfx/platform/win32/rlogin_secrets.hpp"
#include "sfx/platform/win32/portable_credentials.hpp"
#include "sfx/platform/win32/secure_identifier_generator.hpp"
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include <sstream>
#include <algorithm>
#include <stdexcept>

namespace sfx::platform::win32 {
namespace {
namespace inst=installation;
constexpr std::size_t maximum=8U*1024U*1024U;
std::string hex(const std::vector<std::uint8_t>& bytes) {
    constexpr char d[]="0123456789ABCDEF";std::string s;s.reserve(bytes.size()*2);
    for(const auto b:bytes){s+=d[b>>4];s+=d[b&15];}return s;
}
std::vector<std::uint8_t> unhex(std::string_view s) {
    inst::require(!s.empty()&&s.size()%2==0&&s.size()<=8192,"service_secret_record_invalid");
    const auto digit=[](char c)->unsigned {if(c>='0'&&c<='9')return c-'0';if(c>='A'&&c<='F')return c-'A'+10;
        throw std::runtime_error("service_secret_record_invalid");};
    std::vector<std::uint8_t> b;for(std::size_t i=0;i<s.size();i+=2)b.push_back(static_cast<std::uint8_t>((digit(s[i])<<4)|digit(s[i+1])));return b;
}
}
ServiceSecretBridge load_service_secret_bridge(const std::filesystem::path& root,const std::wstring& owner) {
    if(load_portable_board_key(root))return {};
    const auto receipt=inst::read_receipt(root,owner);const auto file=root/L"Config"/L"service-secrets.v1";
    WindowsStorageDaclPolicy policy(owner);
    inst::require(policy.assess(file,StorageDaclObjectKind::file).status.succeeded(),"service_secret_bridge_security_invalid");
    const auto bytes=inst::read_file(file,maximum);std::istringstream in(std::string(bytes.begin(),bytes.end()));
    std::string magic,id,line;inst::require(std::getline(in,magic)&&magic=="SFX-SERVICE-SECRETS-1"&&std::getline(in,id)&&
        std::wstring(id.begin(),id.end())==receipt.id,"service_secret_bridge_identity_invalid");
    ServiceSecretBridge bridge;
    while(std::getline(in,line)) {
        const auto split=line.find(':');inst::require(split!=std::string::npos&&bridge.size()<10000,"service_secret_bridge_invalid");
        auto key=unhex(std::string_view(line).substr(0,split)),value=unhex(std::string_view(line).substr(split+1));
        inst::require(key.size()>5&&value.size()>5&&key[4]==1&&value[4]==2&&
            std::equal(key.begin(),key.begin()+4,value.begin()),"service_secret_bridge_version_invalid");
        inst::require(bridge.emplace(std::move(key),std::move(value)).second,"service_secret_bridge_duplicate");
    }
    return bridge;
}
PortableCredentialPreparation prepare_portable_connection_credentials(const std::filesystem::path& root,const std::wstring& owner) {
    PortableCredentialPreparation result;
    const auto data=root/L"Data"/L"SFX.DB";const auto flags=GetFileAttributesW(data.c_str());
    if(flags!=INVALID_FILE_ATTRIBUTES) {
        auto pin=inst::open_object(data,false,FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
        LARGE_INTEGER size{};inst::require(GetFileSizeEx(pin.get(),&size)&&size.QuadPart>0,"service_conversion_database_invalid");
        WindowsLocalStoragePathPolicy paths(owner);adapters::storage::SqliteStorageSettings settings;
        settings.data_directory=root/L"Data";adapters::storage::SqliteStorage store(settings,paths);
        inst::require(store.start().succeeded(),"service_conversion_storage_unavailable");
        const application::MessageConfigurationActor actor{true,{}};
        const auto ftn=store.ftn_configuration(actor);
        const auto doors=store.door_configuration(actor);
        inst::require(ftn&&ftn.value&&doors&&doors.value,"service_conversion_catalog_unavailable");
        result=prepare_portable_credentials(root,owner,*ftn.value,*doors.value);
        inst::require(store.shutdown().succeeded(),"service_conversion_storage_cleanup_failed");
    } else inst::require(GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND,"service_conversion_database_unavailable");
    return result;
}
void prepare_service_secret_bridge(const std::filesystem::path& root,const std::wstring& owner) {
    inst::require(!inst::elevated()&&inst::operator_sid()==owner,"service_conversion_requires_ordinary_owner");
    inst::verify_runtime_installation(root,owner);const auto receipt=inst::read_receipt(root,owner);
    const auto converted=prepare_portable_connection_credentials(root,owner);
    inst::require(converted.issues.empty(),"service_conversion_saved_connection_unavailable_original_preserved_check_Health");
    // A genuinely fresh Service install has no database/key yet. Retain the
    // empty bootstrap record until Runtime initializes its board key after the
    // existing first-run persistent-state classification. No new DPAPI aliases.
    ServiceSecretBridge bridge;
    std::string text="SFX-SERVICE-SECRETS-1\n";
    for(const auto c:receipt.id)text+=static_cast<char>(c);
    text+='\n';
    for(const auto& item:bridge){text+=hex(item.first)+":"+hex(item.second)+"\n";inst::require(text.size()<=maximum,"service_secret_bridge_capacity");}
    const auto target=root/L"Config"/L"service-secrets.v1";
    const auto temp=root/L"Config"/(L"service-secrets-"+inst::unique_id()+L".pending");
    auto sd=inst::mutable_descriptor(owner);inst::create_file(temp,{text.begin(),text.end()},sd.get());
    // The directory descriptor deliberately carries inheritance ACE flags.
    // Seal this regular file with the existing exact private-file policy.
    inst::require(WindowsStorageDaclPolicy(owner).apply_for_publication(temp,StorageDaclObjectKind::file,target).succeeded(),
        "service_secret_bridge_file_protection_failed");
    inst::require(MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH),"service_secret_bridge_publication_failed_pending_preserved");
    inst::require(load_service_secret_bridge(root,owner)==bridge,"service_secret_bridge_verification_failed");
}
}
