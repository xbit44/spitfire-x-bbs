#include "sfx/application/ftn_retention.hpp"
#include <algorithm>
#include <set>
namespace sfx::application {
namespace {
bool text(std::string_view s,std::size_t maximum){return s.size()<=maximum&&std::none_of(s.begin(),s.end(),[](unsigned char c){return c<32||c==127;});}
void put(std::vector<std::uint8_t>& out,std::uint64_t n,unsigned bytes){for(unsigned i=0;i<bytes;++i)out.push_back(static_cast<std::uint8_t>(n>>(8*i)));}
void string(std::vector<std::uint8_t>& out,std::string_view s){put(out,s.size(),2);out.insert(out.end(),s.begin(),s.end());}
struct Read {
    const std::vector<std::uint8_t>& in;std::size_t at{};bool ok{true};
    std::uint64_t number(unsigned n){if(n>in.size()-at){ok=false;return 0;}std::uint64_t v{};for(unsigned i=0;i<n;++i)v|=std::uint64_t{in[at++]}<<(i*8);return v;}
    std::string string(std::size_t maximum){const auto n=number(2);if(!ok||n>maximum||n>in.size()-at){ok=false;return {};}std::string out(in.begin()+at,in.begin()+at+static_cast<std::size_t>(n));at+=static_cast<std::size_t>(n);return out;}
    std::int64_t time(){const auto n=number(8);if(n>INT64_MAX)ok=false;return static_cast<std::int64_t>(n);}
};
}
std::vector<std::uint8_t> encode_ftn_retention_preview(const FtnRetentionPreview& p){
    if(p.configuration_revision<1||p.reviewed_utc<1||p.changed>kFtnRetentionReviewMaximum||p.shortened>p.changed||p.networks.size()>128||p.draft==decltype(p.draft){}||p.token==FtnTransferToken{})return {};
    std::vector<std::uint8_t> out{1};out.insert(out.end(),p.token.begin(),p.token.end());out.insert(out.end(),p.draft.begin(),p.draft.end());put(out,p.configuration_revision,8);put(out,p.reviewed_utc,8);put(out,p.changed,4);put(out,p.shortened,4);put(out,p.networks.size(),2);
    std::uint64_t total{};std::set<std::string> networks;
    for(const auto& n:p.networks){if(!valid_ftn_code(n.network)||n.name.empty()||!text(n.name,96)||!networks.insert(n.network).second||n.earliest<1||n.latest<n.earliest||(!n.due_now&&!n.expire_later)||n.from_forever>std::uint64_t{n.due_now}+n.expire_later)return {};total+=std::uint64_t{n.due_now}+n.expire_later;string(out,n.network);string(out,n.name);put(out,n.due_now,4);put(out,n.expire_later,4);put(out,n.from_forever,4);put(out,n.earliest,8);put(out,n.latest,8);}
    return total==p.shortened&&out.size()<=kFtnConfigurationPageBytes?out:std::vector<std::uint8_t>{};
}
std::optional<FtnRetentionPreview> decode_ftn_retention_preview(const std::vector<std::uint8_t>& bytes){
    if(bytes.size()<75||bytes.size()>kFtnConfigurationPageBytes)return {};
    Read r{bytes};if(r.number(1)!=1)return {};FtnRetentionPreview p;for(auto& v:p.token)v=static_cast<std::uint8_t>(r.number(1));for(auto& v:p.draft)v=static_cast<std::uint8_t>(r.number(1));p.configuration_revision=r.time();p.reviewed_utc=r.time();p.changed=static_cast<std::uint32_t>(r.number(4));p.shortened=static_cast<std::uint32_t>(r.number(4));const auto count=r.number(2);if(count>128)return {};
    for(std::size_t i=0;i<count&&r.ok;++i){FtnRetentionNetworkReview n;n.network=r.string(48);n.name=r.string(96);n.due_now=static_cast<std::uint32_t>(r.number(4));n.expire_later=static_cast<std::uint32_t>(r.number(4));n.from_forever=static_cast<std::uint32_t>(r.number(4));n.earliest=r.time();n.latest=r.time();p.networks.push_back(std::move(n));}if(!r.ok||r.at!=bytes.size()||encode_ftn_retention_preview(p).empty())return {};return p;
}
}
