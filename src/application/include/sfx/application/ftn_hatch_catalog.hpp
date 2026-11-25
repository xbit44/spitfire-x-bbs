#pragma once
#include "sfx/application/ftn.hpp"
namespace sfx::application {
// Purpose-limited FileMan projection. No passwords, protected envelopes,
// endpoint paths, TLS keys or FTN configuration mutation interface.
struct FtnHatchNetwork {std::string code,name;std::vector<std::string> addresses;bool enabled{};};
struct FtnHatchDestination {std::string network,echo,link,name;};
struct FtnHatchEcho {std::string network,tag;std::int64_t file_area{};};
struct FtnHatchCatalog {std::int64_t revision{};std::vector<FtnHatchNetwork> networks;std::vector<FtnHatchEcho> echoes;std::vector<FtnHatchDestination> destinations;};
FtnHatchCatalog ftn_hatch_catalog(const FtnConfiguration&);
std::vector<std::uint8_t> encode_ftn_hatch_catalog(const FtnHatchCatalog&);
std::optional<FtnHatchCatalog> decode_ftn_hatch_catalog(const std::vector<std::uint8_t>&);
}
