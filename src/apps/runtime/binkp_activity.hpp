#pragma once
#include "sfx/application/ftn.hpp"
#include <string>
#include <string_view>
#include <utility>

namespace sfx::apps::runtime {

inline constexpr std::string_view binkp_startup = "BinkP startup";

template<class Session>
std::string_view binkp_session_network(const Session& state) {
    if(!state.network.empty()||!state.link.empty())return state.network;
    if(state.outbound)return "Outgoing BinkP";
    return state.handshake_complete?"Direct NetMail":"Incoming BinkP";
}

inline std::string_view binkp_import_network(std::string_view network,std::string_view link) {
    return network.empty()&&link.empty()?"Inbound mail":network;
}

inline std::pair<std::string,std::string> binkp_display_names(
    const application::FtnConfiguration* catalog,std::string_view network,std::string_view link) {
    std::pair<std::string,std::string> names{network.empty()?"BinkP":network,link};
    if(catalog) {
        for(const auto& entry:catalog->networks)if(entry.code==network){names.first=entry.name;break;}
        for(const auto& entry:catalog->links)if(entry.network==network&&entry.code==link){names.second=entry.name;break;}
    }
    return names;
}

template<class Session, class Log>
void report_binkp_peer_claims(const Session& state,const Session& previous,const Log& log) {
    if(!state.advertised_system.empty()&&previous.advertised_system.empty())
        log("Peer advertised BBS name: "+state.advertised_system);
    if(state.advertised_addresses.empty()||!previous.advertised_addresses.empty())return;

    const std::string prefix="Peer advertised AKA(s):";
    std::string line=prefix;
    for(const auto& address:state.advertised_addresses) {
        const auto text=application::format_ftn_address(address);
        if(line.size()+1+text.size()>192) {
            log(line);
            line=prefix;
        }
        line+=' ';
        line+=text;
    }
    log(line);
}

}
