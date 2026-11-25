#include "sfx/application/botgate_rules.hpp"
#include <algorithm>
#include <charconv>

namespace sfx::application {
namespace {
std::string_view trim(std::string_view s) {
    const auto a=s.find_first_not_of(" \r\t");
    return a==s.npos ? std::string_view{} : s.substr(a,s.find_last_not_of(" \r\t")-a+1);
}
bool wildcard(std::string_view p,std::string_view s) noexcept {
    std::size_t i=0,j=0,star=p.npos,retry=0;
    while (j<s.size()) {
        const auto lower=[](char c) { return c>='A' && c<='Z' ? static_cast<char>(c+32) : c; };
        if (i<p.size() && (p[i]=='?' || lower(p[i])==lower(s[j]))) { ++i;++j; }
        else if (i<p.size() && p[i]=='*') { star=i++;retry=j; }
        else if (star!=p.npos) { i=star+1;j=++retry; }
        else return false;
    }
    while(i<p.size() && p[i]=='*') ++i;
    return i==p.size();
}
std::optional<std::int64_t> expiry(std::string_view s) {
    // OG UTC timestamp; rejecting malformed/non-UTC offsets avoids stale bans.
    if (s.size()!=20 || s[8]!='T' || s.substr(15)!="+0000") return {};
    const auto number=[&](std::size_t pos,std::size_t n) {
        unsigned value{}; auto r=std::from_chars(s.data()+pos,s.data()+pos+n,value);
        return r.ec==std::errc{} && r.ptr==s.data()+pos+n ? value : UINT32_MAX;
    };
    auto y=number(0,4),m=number(4,2),d=number(6,2),h=number(9,2),min=number(11,2),sec=number(13,2);
    if(y<1970 || y>9999 || m<1 || m>12 || d<1 || d>31 || h>23 || min>59 || sec>59) return {};
    const bool leap=y%4==0 && (y%100!=0 || y%400==0);
    constexpr unsigned days[]{31,28,31,30,31,30,31,31,30,31,30,31};
    if(d>days[m-1]+(m==2 && leap ? 1U : 0U)) return {};
    y-=m<=2; const auto era=y/400,yoe=y-era*400;
    const auto doy=(153*(m>2 ? m-3 : m+9)+2)/5+d-1;
    const auto doe=yoe*365+yoe/4-yoe/100+doy;
    return (static_cast<std::int64_t>(era)*146097+doe-719468)*86400+h*3600+min*60+sec;
}
}
std::optional<BotGatePatternList> parse_botgate_patterns(std::string_view bytes,bool host,bool temporary) {
    if(bytes.size()>1024*1024 || bytes.find('\0')!=bytes.npos) return {};
    BotGatePatternList result;
    while(!bytes.empty()) {
        const auto end=bytes.find('\n'); auto line=trim(bytes.substr(0,end));
        bytes=end==bytes.npos ? std::string_view{} : bytes.substr(end+1);
        if(line.size()>1024) return {};
        if(line.empty() || line.front()==';' || line.front()=='#') continue;
        if(result.size()>=10000) return {};
        BotGatePattern rule;
        if(temporary) {
            const auto marker=line.find("\te=");
            if(marker==line.npos) return {};
            const auto end_time=line.find('\t',marker+3);
            const auto time=expiry(line.substr(marker+3,end_time==line.npos ? line.npos : end_time-marker-3));
            if(!time) return {};
            rule.expires=*time;
        }
        line=trim(line.substr(0,line.find_first_of("\t ;#")));
        if(!line.empty() && line.front()=='!') {rule.negated=true;line.remove_prefix(1);}
        if(line.empty() || line.size()>253) return {};
        rule.network=parse_botgate_network(line);
        if(!rule.network) {
            for(char c:line) if(!((c>='0' && c<='9') || c=='.' || c=='*' || c=='?' ||
                (host && ((c>='a' && c<='z') || (c>='A' && c<='Z') || c=='-')))) return {};
            if(!host && line.find_first_of("*?")==line.npos) return {};
            rule.text=std::string(line);
        }
        result.push_back(std::move(rule));
    }
    return result;
}
bool botgate_patterns_match(const BotGatePatternList& rules,std::string_view source,std::int64_t now) noexcept {
    bool match=false; const auto ip=parse_botgate_ipv4(source);
    for(const auto& r:rules) {
        if(r.expires && now>=r.expires) continue;
        const bool hit=r.network ? ip && ((*ip&r.network->mask)==r.network->address) : wildcard(r.text,source);
        if(hit) { if(r.negated) return false; match=true; }
    }
    return match;
}
std::optional<BotGateGeoList> parse_botgate_geo(std::string_view name,std::string_view bytes) {
    if(name.empty() || name.size()>64 || bytes.size()>16*1024*1024 || bytes.find('\0')!=bytes.npos) return {};
    BotGateGeoList result; result.name=std::string(name);
    while(!bytes.empty()) {
        const auto end=bytes.find('\n'); auto line=trim(bytes.substr(0,end));
        bytes=end==bytes.npos ? std::string_view{} : bytes.substr(end+1);
        if(line.size()>1024) return {};
        if(line.empty() || line.front()=='#' || line.front()==';' || line.front()=='<') continue;
        std::string lower(line); for(auto& c:lower) if(c>='A' && c<='Z') c+=32;
        if(lower=="order allow,deny" || lower=="allow from all") continue;
        if(lower.compare(0,10,"deny from ")) return {};
        line=trim(line.substr(10)); line=trim(line.substr(0,line.find('#')));
        const auto net=parse_botgate_network(line);
        if(!net || result.ranges.size()>=500000) return {};
        result.ranges.emplace_back(net->address,net->address|~net->mask);
    }
    if(result.ranges.empty()) return {};
    std::sort(result.ranges.begin(),result.ranges.end());
    std::vector<std::pair<std::uint32_t,std::uint32_t>> merged;
    for(auto r:result.ranges) {
        if(!merged.empty() && static_cast<std::uint64_t>(r.first)<=static_cast<std::uint64_t>(merged.back().second)+1)
            merged.back().second=std::max(merged.back().second,r.second);
        else merged.push_back(r);
    }
    result.ranges=std::move(merged); return result;
}
bool BotGateGeoList::contains(std::uint32_t ip) const noexcept {
    auto i=std::upper_bound(ranges.begin(),ranges.end(),std::make_pair(ip,UINT32_MAX));
    return i!=ranges.begin() && ip<=(--i)->second;
}
bool BotGateRuleSnapshot::exempt(std::string_view ip,std::int64_t now) const noexcept {
    return botgate_patterns_match(lists[0],ip,now);
}
std::string BotGateRuleSnapshot::refusal(std::string_view ip,std::string_view host,std::int64_t now) const {
    if(exempt(ip,now)) return {};
    for(const auto index:{4U,1U,2U}) if(botgate_patterns_match(lists[index],ip,now)) return kBotGateRuleNames[index];
    if(const auto numeric=parse_botgate_ipv4(ip)) for(const auto& country:geo) if(country.contains(*numeric)) return "geo/"+country.name;
    if(!host.empty() && botgate_patterns_match(lists[3],host,now)) return "host.can";
    return {};
}
bool BotGateArrivalLimits::admit(std::uint32_t ip,std::uint64_t now) {
    for(auto i=entries_.begin();i!=entries_.end();) {
        auto& entry=i->second;
        entry.arrivals.erase(std::remove_if(entry.arrivals.begin(),entry.arrivals.end(),[now](auto t){return now>=t && now-t>=10;}),entry.arrivals.end());
        if(entry.arrivals.empty() && entry.until<=now) i=entries_.erase(i); else ++i;
    }
    if(entries_.find(ip)==entries_.end() && entries_.size()>=4096) return false;
    auto& entry=entries_[ip];if(entry.until>now) return false;
    if(entry.arrivals.size()>=20) {entry.until=now+5400;entry.arrivals.clear();return false;}
    entry.arrivals.push_back(now);return true;
}
std::uint64_t BotGateArrivalLimits::banned_until(std::uint32_t ip) const {
    const auto i=entries_.find(ip);return i==entries_.end() ? 0 : i->second.until;
}
}
