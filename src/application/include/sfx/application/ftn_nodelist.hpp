#pragma once
#include "sfx/application/ftn.hpp"
namespace sfx::application {
struct FtnNodelistEndpoint {std::string host;std::uint16_t port{24554};};
// Bounded lookup of one configured peer in one explicitly selected network's
// ordinary nodelist. This grants no routing, identity or authentication rights.
std::optional<FtnNodelistEndpoint> ftn_nodelist_endpoint(std::string_view,const FtnAddress&);
}
