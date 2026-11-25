#include "sfx/platform/linux/portable_key_store.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <sodium.h>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace sfx::platform::linux_host {
namespace {
constexpr std::string_view magic="SFXKEY1\n",marker_magic="SFX-PORTABLE-CREDENTIALS-1\n";
constexpr std::size_t material=8+16+32,total=material+32;
struct RawKey {
    std::vector<std::uint8_t> bytes;
    ~RawKey(){application::erase_rlogin_secret(bytes.data(),bytes.size());}
};
PortableKeyResult fail(PortableKeyStatus status,const char* code){return {status,{},code};}
bool missing(int directory,const char* leaf) {
    struct stat info{};
    if(fstatat(directory,leaf,&info,AT_SYMLINK_NOFOLLOW)==0)return false;
    if(errno==ENOENT)return true;
    throw std::runtime_error("portable_key_metadata_unavailable");
}
bool valid(const RawKey& raw) {
    if(raw.bytes.size()!=total||!std::equal(magic.begin(),magic.end(),raw.bytes.begin()))return false;
    std::array<unsigned char,32> checksum{};
    crypto_hash_sha256(checksum.data(),raw.bytes.data(),material);
    return sodium_memcmp(checksum.data(),raw.bytes.data()+material,checksum.size())==0;
}
std::string hex(const unsigned char* bytes,std::size_t count) {
    constexpr char digits[]="0123456789ABCDEF";std::string text;text.reserve(count*2);
    for(std::size_t n=0;n<count;++n){text+=digits[bytes[n]>>4];text+=digits[bytes[n]&15];}return text;
}
std::string marker(const RawKey& raw) {
    std::array<unsigned char,32> digest{};crypto_hash_sha256(digest.data(),raw.bytes.data(),raw.bytes.size());
    return std::string(marker_magic)+hex(raw.bytes.data()+8,16)+'\n'+hex(digest.data(),digest.size())+'\n';
}
bool marker_matches(int directory,const RawKey& raw) {
    const auto bytes=read_private_file(directory,"portable-credentials.v1",256);
    return std::string(bytes.begin(),bytes.end())==marker(raw);
}
bool boundary(int directory,const std::filesystem::path& config) {
    const auto current=open_owned_directory(config);struct stat a{},b{};
    return fstat(directory,&a)==0&&fstat(current.get(),&b)==0&&a.st_dev==b.st_dev&&a.st_ino==b.st_ino;
}
PortableKeyResult decoded(const RawKey& raw) {
    PortableBoardKey key;std::copy_n(raw.bytes.begin()+8,key.board.size(),key.board.begin());
    std::copy_n(raw.bytes.begin()+24,key.key.bytes.size(),key.key.bytes.begin());
    return {PortableKeyStatus::ready,std::move(key),"portable_key_ready"};
}
}
PortableKeyResult load_portable_board_key(const std::filesystem::path& root) {
    try {
        if(geteuid()==0||sodium_init()<0)return {};
        const auto config=root/"config";const auto directory=open_owned_directory(config);
        if(missing(directory.get(),"board.key"))return fail(PortableKeyStatus::missing,"portable_key_missing_restore_matching_copy");
        RawKey raw;raw.bytes=read_private_file(directory.get(),"board.key",total);
        if(!valid(raw))return fail(PortableKeyStatus::invalid,"portable_key_invalid_original_preserved");
        if(missing(directory.get(),"portable-credentials.v1")||!marker_matches(directory.get(),raw))
            return fail(PortableKeyStatus::invalid,"portable_key_metadata_missing_or_changed");
        if(!boundary(directory.get(),config))return fail(PortableKeyStatus::invalid,"portable_key_directory_changed");
        return decoded(raw);
    }catch(...){return {};}
}
PortableKeyResult initialize_fresh_portable_board_key(const std::filesystem::path& root) {
    try {
        if(geteuid()==0||sodium_init()<0)return {};
        const auto config=root/"config";const auto directory=open_owned_directory(config);
        const auto intent=read_private_file(directory.get(),"first-run.v1",64);
        if(std::string(intent.begin(),intent.end())!="SFX_LINUX_FIRST_RUN=1\n")
            return fail(PortableKeyStatus::invalid,"portable_key_fresh_intent_required");
        InstallationLock creation;
        if(!creation.acquire(config,".board-key.initialize.lock"))return {};
        const bool no_key=missing(directory.get(),"board.key");
        const bool no_marker=missing(directory.get(),"portable-credentials.v1");
        if(no_key&&!no_marker)return fail(PortableKeyStatus::invalid,"portable_key_missing_established_board_originals_preserved");
        RawKey raw;
        if(no_key) {
            raw.bytes.resize(total);std::copy(magic.begin(),magic.end(),raw.bytes.begin());
            randombytes_buf(raw.bytes.data()+8,16);crypto_aead_xchacha20poly1305_ietf_keygen(raw.bytes.data()+24);
            crypto_hash_sha256(raw.bytes.data()+material,raw.bytes.data(),material);
            publish_private_file(directory.get(),"board.key",raw.bytes,false);
        }else raw.bytes=read_private_file(directory.get(),"board.key",total);
        if(!valid(raw))return fail(PortableKeyStatus::invalid,"portable_key_invalid_original_preserved");
        if(no_marker) {
            const auto text=marker(raw);publish_private_file(directory.get(),"portable-credentials.v1",{text.begin(),text.end()},false);
        }else if(!marker_matches(directory.get(),raw))
            return fail(PortableKeyStatus::invalid,"portable_key_metadata_missing_or_changed");
        if(!boundary(directory.get(),config))return fail(PortableKeyStatus::invalid,"portable_key_directory_changed");
        return decoded(raw);
    }catch(...){return {};}
}
} // namespace sfx::platform::linux_host
