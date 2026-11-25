#include "sfx/application/ftn_hatch_catalog.hpp"
#include "sfx/application/ftn_hatch.hpp"
#include <algorithm>
#include <set>
#include <tuple>
#include <limits>
namespace sfx::application {
namespace {
struct Writer{std::vector<std::uint8_t> bytes;bool good{true};void n(std::uint64_t v,unsigned count){if(count>kFtnConfigurationMaximumBytes-bytes.size()){good=false;return;}for(unsigned i=0;i<count;++i)bytes.push_back(static_cast<std::uint8_t>(v>>(i*8)));}void s(const std::string& v){if(v.size()>512||v.size()+2>kFtnConfigurationMaximumBytes-bytes.size()){good=false;return;}n(v.size(),2);bytes.insert(bytes.end(),v.begin(),v.end());}};
struct Reader{const std::vector<std::uint8_t>& bytes;std::size_t at{};bool good{true};std::uint64_t n(unsigned count){if(count>bytes.size()-at){good=false;return 0;}std::uint64_t value{};for(unsigned i=0;i<count;++i)value|=static_cast<std::uint64_t>(bytes[at++])<<(i*8);return value;}std::string s(std::size_t limit){const auto count=n(2);if(count>limit||count>bytes.size()-at){good=false;return {};}std::string value(bytes.begin()+at,bytes.begin()+at+static_cast<std::size_t>(count));at+=static_cast<std::size_t>(count);return value;}};
std::string key(std::string s){for(auto& c:s)if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');return s;}
bool valid(const FtnHatchCatalog& c){
    if(c.revision<1||c.networks.size()>128||c.echoes.size()>16384||c.destinations.size()>65536)return false;
    std::set<std::string> networks;std::set<std::pair<std::string,std::string>> echoes;std::set<std::tuple<std::string,std::string,std::string>> destinations;
    for(const auto& n:c.networks){if(!valid_ftn_code(n.code)||!valid_ftn_display_text(n.name,96)||!networks.insert(key(n.code)).second||n.addresses.size()>33)return false;for(const auto& a:n.addresses)if(!parse_ftn_address(a))return false;}
    for(const auto& e:c.echoes)if(!networks.count(key(e.network))||!valid_ftn_echo_tag(e.tag)||e.file_area<=0||!echoes.emplace(key(e.network),key(e.tag)).second)return false;
    for(const auto& d:c.destinations)if(!echoes.count({key(d.network),key(d.echo)})||!valid_ftn_code(d.link)||!valid_ftn_display_text(d.name,96)||!destinations.emplace(key(d.network),key(d.echo),key(d.link)).second)return false;
    return true;
}
}
#include "ftn_hatch_request.inc"
FtnHatchCatalog ftn_hatch_catalog(const FtnConfiguration& c){
    FtnHatchCatalog out;out.revision=c.revision;
    for(const auto& n:c.networks){FtnHatchNetwork v{n.code,n.name,{},n.enabled};if(parse_ftn_address(n.address))v.addresses.push_back(n.address);for(const auto& a:n.additional_addresses)if(parse_ftn_address(a))v.addresses.push_back(a);out.networks.push_back(std::move(v));}
    for(const auto& e:c.file_echoes){const auto n=std::find_if(c.networks.begin(),c.networks.end(),[&](const auto& v){return key(v.code)==key(e.network);});if(n!=c.networks.end())out.echoes.push_back({n->code,e.tag,e.file_area});}
    for(const auto& p:c.permissions)if(p.file_echo&&p.send){const auto link=std::find_if(c.links.begin(),c.links.end(),[&](const auto& l){return key(l.network)==key(p.network)&&key(l.code)==key(p.link);});const auto echo=std::find_if(out.echoes.begin(),out.echoes.end(),[&](const auto& e){return key(e.network)==key(p.network)&&key(e.tag)==key(p.tag);});if(link!=c.links.end()&&echo!=out.echoes.end()&&link->enabled&&ftn_outbound(*link))out.destinations.push_back({echo->network,echo->tag,link->code,link->name});}
    return out;
}
std::vector<std::uint8_t> encode_ftn_hatch_catalog(const FtnHatchCatalog& c){if(!valid(c))return {};Writer w;w.n(1,1);w.n(static_cast<std::uint64_t>(c.revision),8);w.n(c.networks.size(),4);for(const auto& n:c.networks){w.s(n.code);w.s(n.name);w.n(n.enabled,1);w.n(n.addresses.size(),1);for(const auto& a:n.addresses)w.s(a);}w.n(c.echoes.size(),4);for(const auto& e:c.echoes){w.s(e.network);w.s(e.tag);w.n(static_cast<std::uint64_t>(e.file_area),8);}w.n(c.destinations.size(),4);for(const auto& d:c.destinations){w.s(d.network);w.s(d.echo);w.s(d.link);w.s(d.name);}return w.good?std::move(w.bytes):std::vector<std::uint8_t>{};}
std::optional<FtnHatchCatalog> decode_ftn_hatch_catalog(const std::vector<std::uint8_t>& bytes){if(bytes.size()>kFtnConfigurationMaximumBytes)return {};Reader r{bytes};if(r.n(1)!=1)return {};FtnHatchCatalog c;const auto revision=r.n(8);if(revision>INT64_MAX)return {};c.revision=static_cast<std::int64_t>(revision);auto count=r.n(4);if(count>128)return {};for(std::uint64_t i=0;i<count&&r.good;++i){FtnHatchNetwork n;n.code=r.s(48);n.name=r.s(96);const auto enabled=r.n(1);if(enabled>1)return {};n.enabled=enabled==1;const auto addresses=r.n(1);if(addresses>33)return {};for(std::uint64_t j=0;j<addresses&&r.good;++j)n.addresses.push_back(r.s(96));c.networks.push_back(std::move(n));}count=r.n(4);if(count>16384)return {};for(std::uint64_t i=0;i<count&&r.good;++i){FtnHatchEcho e;e.network=r.s(48);e.tag=r.s(128);const auto area=r.n(8);if(area>INT64_MAX)return {};e.file_area=static_cast<std::int64_t>(area);c.echoes.push_back(std::move(e));}count=r.n(4);if(count>65536)return {};for(std::uint64_t i=0;i<count&&r.good;++i)c.destinations.push_back({r.s(48),r.s(128),r.s(48),r.s(96)});if(!r.good||r.at!=bytes.size()||!valid(c))return {};return c;}
}
