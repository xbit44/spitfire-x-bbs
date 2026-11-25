#include "linux_installed_components.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <sodium.h>
#include <array>
#include <exception>
#include <string_view>
#include <unistd.h>
namespace sfx::apps::runtime {
bool verified_linux_starter_doors(const std::filesystem::path& root){
    namespace host=platform::linux_host;
    const auto installation=host::open_owned_directory(root);
    const auto doors=host::open_owned_resource_directory(installation.get(),"doors");
    struct Input {const char* folder;const char* program;std::size_t bytes;const char* sha256;};
    // Approved standalone Linux runtime inputs. Source archives are never
    // installed; only a fresh shared bootstrap can publish factory Door rows.
    constexpr Input inputs[]={{"sfxguess32","sfxguess32",559256,"587a201293285a4c9c2bc70ac841cd3371768ffb8f26e8a7efbb751849edceef"},
        {"newswire-x-rss","newswire-x-rss",3047528,"2cf617c33d421ccfcc650000fbe6995a2242d8ad3868cbf55aa2fd9d053b428d"}};
    for(const auto& input:inputs){
        try{const auto folder=host::open_owned_resource_directory(doors.get(),input.folder);const auto data=host::read_owned_resource(folder.get(),input.program,input.bytes);
            if(data.size()!=input.bytes||faccessat(folder.get(),input.program,X_OK,0)!=0)return false;
            std::array<unsigned char,32> digest{};std::array<char,65> hex{};
            if(sodium_init()<0||crypto_hash_sha256(digest.data(),data.data(),data.size())!=0)return false;
            sodium_bin2hex(hex.data(),hex.size(),digest.data(),digest.size());if(std::string_view(hex.data())!=input.sha256)return false;
        }catch(const std::exception&){return false;}
    }
    return true;
}
}
