#include "sfx/platform/win32/portable_key_store.hpp"
#include "sfx/platform/win32/portable_credentials.hpp"
#include "sfx/platform/win32/service_file_owner.hpp"
#include <windows.h>
#include <sddl.h>
#include <sodium.h>
#include <algorithm>
#include <cstddef>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace sfx::platform::win32 {
namespace {
constexpr std::string_view magic="SFXKEY1\n",marker_magic="SFX-PORTABLE-CREDENTIALS-1\n";
constexpr std::size_t material=8+16+32,total=material+32;
constexpr auto key_leaf=L"board.key",marker_leaf=L"portable-credentials.v1";
std::mutex creation_mutex;
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    Handle()=default;explicit Handle(HANDLE h):value(h){};Handle(const Handle&)=delete;
    Handle(Handle&& h)noexcept:value(h.value){h.value=INVALID_HANDLE_VALUE;}
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    explicit operator bool()const{return value&&value!=INVALID_HANDLE_VALUE;}
};
struct RawKey {std::array<std::uint8_t,total> bytes{};~RawKey(){application::erase_rlogin_secret(bytes.data(),bytes.size());}};
struct Pins {
    std::filesystem::path config;std::vector<Handle> directories;bool valid{};
    explicit Pins(const std::filesystem::path& root){
        auto text=root.native();std::replace(text.begin(),text.end(),L'/',L'\\');
        if(text.size()<4||text.size()>2048||text[1]!=L':'||text[2]!=L'\\'||!iswalpha(text[0]))return;
        std::array<wchar_t,4096> full{};const auto n=GetFullPathNameW(text.c_str(),static_cast<DWORD>(full.size()),full.data(),nullptr);
        if(!n||n>=full.size()||_wcsicmp(text.c_str(),full.data()))return;
        config=std::filesystem::path(text)/L"Config";const auto whole=config.native();
        for(std::size_t end=3;;){
            const auto path=whole.substr(0,end);Handle h(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));BY_HANDLE_FILE_INFORMATION info{};
            if(!h||!GetFileInformationByHandle(h.value,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))return;
            directories.push_back(std::move(h));if(end==whole.size())break;
            const auto next=whole.find(L'\\',end==3?3:end+1);end=next==whole.npos?whole.size():next;
        }
        valid=true;
    }
};
PortableKeyStatus read(const std::filesystem::path& path,std::vector<std::uint8_t>& out,std::size_t maximum){
    Handle h(CreateFileW(path.c_str(),GENERIC_READ|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!h){const auto error=GetLastError();return error==ERROR_FILE_NOT_FOUND?PortableKeyStatus::missing:PortableKeyStatus::unavailable;}
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
    if(!GetFileInformationByHandle(h.value,&info)||!GetFileSizeEx(h.value,&size))return PortableKeyStatus::unavailable;
    if((info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||info.nNumberOfLinks!=1||size.QuadPart<0||size.QuadPart>static_cast<LONGLONG>(maximum))return PortableKeyStatus::invalid;
    out.resize(static_cast<std::size_t>(size.QuadPart));DWORD count{};
    if(!ReadFile(h.value,out.data(),static_cast<DWORD>(out.size()),&count,nullptr)||count!=out.size())return PortableKeyStatus::unavailable;
    return PortableKeyStatus::ready;
}
std::string hex(const std::uint8_t* bytes,std::size_t size){constexpr char digits[]="0123456789ABCDEF";std::string s;for(std::size_t n=0;n<size;++n){s+=digits[bytes[n]>>4];s+=digits[bytes[n]&15];}return s;}
std::string marker(const RawKey& raw){std::array<unsigned char,32> digest{};crypto_hash_sha256(digest.data(),raw.bytes.data(),raw.bytes.size());return std::string(marker_magic)+hex(raw.bytes.data()+8,16)+'\n'+hex(digest.data(),digest.size())+'\n';}
PortableKeyResult load(const Pins& pins,bool marker_required,RawKey* retained=nullptr){
    if(!pins.valid||sodium_init()<0)return {};
    std::vector<std::uint8_t> bytes;struct Wipe {std::vector<std::uint8_t>& b;~Wipe(){application::erase_rlogin_secret(b.data(),b.size());}} wipe{bytes};
    const auto status=read(pins.config/key_leaf,bytes,total);
    if(status!=PortableKeyStatus::ready)return {status,{},status==PortableKeyStatus::missing?"portable_key_missing_restore_matching_copy":"portable_key_unavailable_or_invalid"};
    if(bytes.size()!=total||!std::equal(magic.begin(),magic.end(),bytes.begin()))return {PortableKeyStatus::invalid,{},"portable_key_invalid_original_preserved"};
    std::array<unsigned char,32> checksum{};crypto_hash_sha256(checksum.data(),bytes.data(),material);
    if(sodium_memcmp(checksum.data(),bytes.data()+material,checksum.size()))return {PortableKeyStatus::invalid,{},"portable_key_invalid_original_preserved"};
    RawKey raw;std::copy(bytes.begin(),bytes.end(),raw.bytes.begin());std::vector<std::uint8_t> meta;
    const auto meta_status=read(pins.config/marker_leaf,meta,256);const auto expected=marker(raw);
    if(meta_status==PortableKeyStatus::missing&&!marker_required){}
    else if(meta_status!=PortableKeyStatus::ready||std::string(meta.begin(),meta.end())!=expected)return {meta_status==PortableKeyStatus::unavailable?meta_status:PortableKeyStatus::invalid,{},"portable_key_metadata_missing_or_changed"};
    PortableBoardKey key;std::copy_n(raw.bytes.begin()+8,key.board.size(),key.board.begin());std::copy_n(raw.bytes.begin()+24,key.key.bytes.size(),key.key.bytes.begin());
    if(retained)retained->bytes=raw.bytes;
    return {PortableKeyStatus::ready,std::move(key),"portable_key_ready"};
}
bool publish_new(const Pins& pins,const wchar_t* leaf,const std::uint8_t* data,std::size_t count,void* security,const std::wstring& owner,bool replace=false){
    std::array<std::uint8_t,16> nonce{};randombytes_buf(nonce.data(),nonce.size());const auto name="sfx-key-"+hex(nonce.data(),nonce.size())+".new";const auto temporary=pins.config/std::filesystem::path(name);
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),security,FALSE};HANDLE raw{};
    {ServiceFileOwnerScope scope(owner);if(!scope.valid()){SetLastError(ERROR_ACCESS_DENIED);return false;}raw=CreateFileW(temporary.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ,&attributes,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);}
    Handle h(raw);
    if(!h)return false;DWORD written{};
    // Failed/uncertain private staging files remain for reviewed retry. No
    // other file is removed or replaced and the final key is never partial.
    if(!WriteFile(h.value,data,static_cast<DWORD>(count),&written,nullptr)||written!=count||!FlushFileBuffers(h.value))return false;
    const auto destination=L"\\\\?\\"+(pins.config/leaf).native();std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO)+(destination.size()+1)*sizeof(wchar_t));
    auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());rename->ReplaceIfExists=replace?TRUE:FALSE;rename->RootDirectory=nullptr;rename->FileNameLength=static_cast<DWORD>(destination.size()*sizeof(wchar_t));std::copy(destination.begin(),destination.end(),rename->FileName);
    return SetFileInformationByHandle(h.value,FileRenameInfo,rename,static_cast<DWORD>(buffer.size()))!=FALSE&&FlushFileBuffers(h.value)!=FALSE;
}
}
PortableKeyResult load_portable_board_key(const std::filesystem::path& root){Pins pins(root);return load(pins,true);}
PortableKeyResult create_portable_board_key(const std::filesystem::path& root,const std::wstring& owner){
    std::lock_guard<std::mutex> lock(creation_mutex);Pins pins(root);if(!pins.valid||sodium_init()<0)return {};
    RawKey raw;auto existing=load(pins,false,&raw);
    if(existing.status!=PortableKeyStatus::ready&&existing.status!=PortableKeyStatus::missing)return existing;
    std::vector<std::uint8_t> previous_marker;const auto marker_status=read(pins.config/marker_leaf,previous_marker,256);
    if(existing.status==PortableKeyStatus::missing&&marker_status!=PortableKeyStatus::missing)return {PortableKeyStatus::invalid,{},"portable_key_missing_established_board_originals_preserved"};
    PSID sid{};if(!ConvertStringSidToSidW(owner.c_str(),&sid))return {};LocalFree(sid);
    const auto sddl=L"O:"+owner+L"D:P(A;;FA;;;"+owner+L")(A;;FA;;;SY)(A;;FA;;;BA)";PSECURITY_DESCRIPTOR descriptor{};
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))return {};
    struct Free {void* p;~Free(){LocalFree(p);}} free{descriptor};
    if(existing.status==PortableKeyStatus::missing){
        // Retain explicit original-host initialization authority BEFORE key
        // publication. A crash after key creation may retry the first file
        // namespace without regenerating this key; a copied root cannot use it.
        Handle root_pin(CreateFileW(root.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));BY_HANDLE_FILE_INFORMATION root_info{};
        if(!root_pin||!GetFileInformationByHandle(root_pin.value,&root_info)||(root_info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return {};
        const auto root_id=(std::uint64_t(root_info.nFileIndexHigh)<<32)|root_info.nFileIndexLow;
        const auto proof=std::string("SFX-FILE-INITIALIZATION-1\n")+root.u8string()+'\n'+std::to_string(root_info.dwVolumeSerialNumber)+'\n'+std::to_string(root_id)+'\n';
        std::vector<std::uint8_t> prior;const auto proof_status=read(pins.config/L"file-identities.initialize",prior,4096);
        if(proof_status==PortableKeyStatus::missing){if(!publish_new(pins,L"file-identities.initialize",reinterpret_cast<const std::uint8_t*>(proof.data()),proof.size(),descriptor,owner))return {PortableKeyStatus::unavailable,{},"portable_initialization_proof_incomplete_retry",GetLastError()};}
        else if(proof_status!=PortableKeyStatus::ready||std::string(prior.begin(),prior.end())!=proof)return {PortableKeyStatus::invalid,{},"portable_initialization_proof_changed_preserved"};
        std::copy(magic.begin(),magic.end(),raw.bytes.begin());randombytes_buf(raw.bytes.data()+8,16);crypto_aead_xchacha20poly1305_ietf_keygen(raw.bytes.data()+24);crypto_hash_sha256(raw.bytes.data()+material,raw.bytes.data(),material);
        if(!publish_new(pins,key_leaf,raw.bytes.data(),raw.bytes.size(),descriptor,owner))return {PortableKeyStatus::unavailable,{},"portable_key_creation_incomplete_originals_preserved",GetLastError()};
    }
    if(marker_status==PortableKeyStatus::missing){const auto text=marker(raw);if(!publish_new(pins,marker_leaf,reinterpret_cast<const std::uint8_t*>(text.data()),text.size(),descriptor,owner))return {PortableKeyStatus::unavailable,{},"portable_key_metadata_incomplete_retry",GetLastError()};}
    return load(pins,true);
}
bool portable_credentials_established(const std::filesystem::path& root){
    for(const auto* leaf:{key_leaf,marker_leaf,L"portable-secrets.v1"}){
        if(GetFileAttributesW((root/L"Config"/leaf).c_str())!=INVALID_FILE_ATTRIBUTES)return true;
        const auto error=GetLastError();if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)return true;
    }return false;
}
application::PortableCredentialGeneration legacy_credential_generation(const std::vector<std::uint8_t>& bytes){
    application::PortableCredentialGeneration hash{};if(sodium_init()<0)throw std::runtime_error("portable_crypto_unavailable");crypto_hash_sha256(hash.data(),bytes.data(),bytes.size());return hash;
}
PortableCredentialAliases read_portable_credential_aliases(const std::filesystem::path& root){
    Pins pins(root);if(!pins.valid)throw std::runtime_error("portable_alias_directory_unavailable");
    std::vector<std::uint8_t> bytes;const auto status=read(pins.config/L"portable-secrets.v1",bytes,8U*1024U*1024U);
    if(status==PortableKeyStatus::missing)return {};
    if(status!=PortableKeyStatus::ready)throw std::runtime_error("portable_alias_file_unavailable_original_preserved");
    std::istringstream input(std::string(bytes.begin(),bytes.end()));std::string line;
    if(!std::getline(input,line)||line!="SFX-PORTABLE-SECRETS-1")throw std::runtime_error("portable_alias_file_invalid_original_preserved");
    const auto decode=[](std::string_view value){
        if(value.empty()||value.size()%2||value.size()>application::kPortableCredentialMaximumBytes*2)throw std::runtime_error("portable_alias_record_invalid");
        const auto digit=[](char c)->unsigned{if(c>='0'&&c<='9')return c-'0';if(c>='A'&&c<='F')return c-'A'+10;throw std::runtime_error("portable_alias_record_invalid");};
        std::vector<std::uint8_t> out;for(std::size_t n=0;n<value.size();n+=2)out.push_back(static_cast<std::uint8_t>((digit(value[n])<<4)|digit(value[n+1])));return out;
    };
    PortableCredentialAliases aliases;
    while(std::getline(input,line)){
        if(line.size()<65||line[64]!=':'||aliases.size()>=10000)throw std::runtime_error("portable_alias_record_invalid");
        const auto id=decode(std::string_view(line).substr(0,64));auto value=decode(std::string_view(line).substr(65));
        application::PortableCredentialGeneration generation{};std::copy(id.begin(),id.end(),generation.begin());
        if(value.size()<application::kPortableCredentialMinimumBytes||value.size()>application::kPortableCredentialMaximumBytes||value[0]!='S'||value[1]!='F'||value[2]!='X'||(value[3]!='F'&&value[3]!='R')||value[4]!=3||!std::equal(generation.begin(),generation.end(),value.begin()+application::kPortableCredentialGenerationOffset)||!aliases.emplace(generation,std::move(value)).second)throw std::runtime_error("portable_alias_record_invalid");
    }return aliases;
}
void publish_portable_credential_aliases(const std::filesystem::path& root,const std::wstring& owner,const PortableCredentialAliases& aliases){
    // Caller holds installation exclusion. Read/validate existing records and
    // retain all of them; publication cannot discard an earlier saved alias.
    const auto previous=read_portable_credential_aliases(root);
    for(const auto& item:previous)if(aliases.find(item.first)==aliases.end()||aliases.at(item.first)!=item.second)throw std::runtime_error("portable_alias_history_changed");
    if(previous==aliases)return;
    if(!load_portable_board_key(root))throw std::runtime_error("portable_key_required_aliases_preserved");
    std::string text="SFX-PORTABLE-SECRETS-1\n";for(const auto& item:aliases){text+=hex(item.first.data(),item.first.size())+":"+hex(item.second.data(),item.second.size())+'\n';if(text.size()>8U*1024U*1024U)throw std::runtime_error("portable_alias_capacity");}
    Pins pins(root);if(!pins.valid)throw std::runtime_error("portable_alias_directory_unavailable");
    const auto sddl=L"O:"+owner+L"D:P(A;;FA;;;"+owner+L")(A;;FA;;;SY)(A;;FA;;;BA)";PSECURITY_DESCRIPTOR descriptor{};
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))throw std::runtime_error("portable_alias_security_unavailable");
    struct Free {void* p;~Free(){LocalFree(p);}} free{descriptor};
    if(!publish_new(pins,L"portable-secrets.v1",reinterpret_cast<const std::uint8_t*>(text.data()),text.size(),descriptor,owner,true))throw std::runtime_error("portable_alias_publication_incomplete_originals_preserved");
    if(read_portable_credential_aliases(root)!=aliases)throw std::runtime_error("portable_alias_publication_verification_failed");
}
std::optional<std::vector<std::uint8_t>> portable_credential_alias(const std::filesystem::path& root,const std::vector<std::uint8_t>& original){
    try{const auto aliases=read_portable_credential_aliases(root);const auto found=aliases.find(legacy_credential_generation(original));if(found!=aliases.end())return found->second;}catch(...){}return {};
}
PortableCredentialReader::PortableCredentialReader(const std::filesystem::path& root):key_(load_portable_board_key(root)),established_(portable_credentials_established(root)){
    if(established_)try{aliases_=read_portable_credential_aliases(root);}catch(...){valid_=false;}
}
std::optional<application::RloginSecret> PortableCredentialReader::open(PortableCredentialFamily family,std::string_view purpose,const std::vector<std::uint8_t>& original)const{
    if(!valid_||!key_)return {};const auto* encrypted=&original;
    if(original.size()>4&&original[4]!=3){const auto alias=aliases_.find(legacy_credential_generation(original));if(alias==aliases_.end())return {};encrypted=&alias->second;}
    return open_portable_credential(key_.value->key,key_.value->board,family,purpose,*encrypted);
}
}
