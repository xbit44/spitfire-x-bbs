#pragma once
#include "sfx/platform/win32/portable_key_store.hpp"
#include "sfx/application/door.hpp"
#include "sfx/application/ftn.hpp"
#include <map>
namespace sfx::platform::win32 {
using PortableCredentialAliases=std::map<application::PortableCredentialGeneration,std::vector<std::uint8_t>>;
bool portable_credentials_established(const std::filesystem::path&);
PortableCredentialAliases read_portable_credential_aliases(const std::filesystem::path&);
void publish_portable_credential_aliases(const std::filesystem::path&,const std::wstring&,const PortableCredentialAliases&);
std::optional<std::vector<std::uint8_t>> portable_credential_alias(const std::filesystem::path&,const std::vector<std::uint8_t>&);
application::PortableCredentialGeneration legacy_credential_generation(const std::vector<std::uint8_t>&);
class PortableCredentialReader final {
public:
    explicit PortableCredentialReader(const std::filesystem::path&);
    bool legacy_allowed()const noexcept{return !established_;}
    std::optional<application::RloginSecret> open(PortableCredentialFamily,std::string_view purpose,const std::vector<std::uint8_t>&)const;
private:
    PortableKeyResult key_;PortableCredentialAliases aliases_;bool established_{},valid_{true};
};
struct PortableCredentialIssue {std::string connection,diagnostic;};
struct PortableCredentialPreparation {std::size_t converted{};std::vector<PortableCredentialIssue> issues;};
// Explicit stopped bootstrap/update/preparation. Each readable legacy value is
// verified before its alias is published; unreadable originals remain exact.
// Existing portable values/aliases never permit regeneration of a missing key.
// Catalog revisions and pending FTN credential generations are unchanged.
PortableCredentialPreparation prepare_portable_credentials(const std::filesystem::path&,const std::wstring&,
    const application::FtnConfiguration&,const application::DoorCatalog&);
}
