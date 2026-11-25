#include "sfx/application/ftn_external.hpp"
#include <algorithm>
#include <charconv>
#include <set>

namespace sfx::application {
namespace {
bool alnum(char c) noexcept {return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9');}
std::string lower(std::string s){for(auto& c:s)if(c>='A'&&c<='Z')c=static_cast<char>(c+32);return s;}
bool same(std::string_view a,std::string_view b){return lower(std::string(a))==lower(std::string(b));}
std::string hex(std::uint64_t value,unsigned width){std::string out(width,'0');do{if(!width)return {};out[--width]="0123456789abcdef"[value&15];value>>=4;}while(value);return out;}
template<class T>std::string bytes_hex(const T& bytes){std::string out;for(auto b:bytes)out+=hex(b,2);return out;}
template<class T>bool read_hex(std::string_view text,T& bytes){
    if(text.size()!=bytes.size()*2)return false;
    for(std::size_t i=0;i<bytes.size();++i){unsigned value{};const auto at=text.data()+2*i;const auto r=std::from_chars(at,at+2,value,16);if(r.ec!=std::errc{}||r.ptr!=at+2)return false;bytes[i]=static_cast<std::uint8_t>(value);}return true;
}
bool number(std::string_view text,std::uint64_t& value){if(text.empty()||text.size()>20)return false;const auto r=std::from_chars(text.data(),text.data()+text.size(),value);return r.ec==std::errc{}&&r.ptr==text.data()+text.size();}
bool valid(const FtnExternalReceiveRecord& r){
    if(!r.id.valid()||(r.run.empty()?!valid_ftn_code(r.network):!r.network.empty())||r.remote.empty()||r.remote.size()>64||r.size>kFtnTransferMaximumBytes||r.timestamp>UINT32_MAX||!valid_ftn_external_path(r.relative_file))return false;
    const auto prefix=r.run.empty()?ftn_external_root(r.network)+"/Inbound/":std::string("FTN/External/Inbound/");
    if(r.relative_file.rfind(prefix,0)!=0)return false;
    auto tail=std::string_view(r.relative_file).substr(prefix.size());
    if(tail.rfind("Secure/",0)==0)tail.remove_prefix(7);
    else if(tail.rfind("Nonsecure/",0)==0)tail.remove_prefix(10);
    else if(tail.rfind("Links/",0)==0){tail.remove_prefix(6);const auto end=tail.find('/');if(end==tail.npos||!valid_ftn_code(tail.substr(0,end)))return false;tail.remove_prefix(end+1);}
    else return false;
    const auto name=tail;
    if(!valid_ftn_leaf_name(name)||name.find('/')!=name.npos)return false;
    std::set<std::string> addresses;
    for(const auto& a:r.remote){const auto text=format_ftn_address(a);if(!parse_ftn_address(text)||!addresses.insert(lower(text)).second)return false;}
    if(r.run.empty())return r.authenticated_remote.empty();
    Identifier128 run;if(!read_hex(r.run,run.bytes)||!run.valid()||r.authenticated_remote.size()>r.remote.size()||(!r.protected_session&&!r.authenticated_remote.empty()))return false;
    std::set<std::string> trusted;
    for(const auto& a:r.authenticated_remote){const auto text=lower(format_ftn_address(a));if(!addresses.count(text)||!trusted.insert(text).second)return false;}
    return true;
}
}
bool valid_ftn_external_path(std::string_view path) noexcept {
    if(path.empty()||path.size()>512||path.front()=='/'||path.back()=='/')return false;
    std::size_t start{};
    for(std::size_t i=0;i<=path.size();++i){
        if(i==path.size()||path[i]=='/'){
            const auto part=path.substr(start,i-start);
            if(part.empty()||part=="."||part==".."||part.back()=='.')return false;
            start=i+1;
        }else if(!alnum(path[i])&&path[i]!='.'&&path[i]!='_'&&path[i]!='-')return false;
    }return true;
}
namespace {
std::string external_path(std::string_view input,FtnExternalPathStyle style,bool to_native){
    if(style==FtnExternalPathStyle::canonical)return std::string(input);
    if(style!=FtnExternalPathStyle::linux_lowercase_v1||!valid_ftn_external_path(input))return {};
    const std::string from=to_native?"FTN/External/":"ftn/external/";
    if(input.substr(0,from.size())!=from)return {};
    std::vector<std::string> parts;std::size_t at=from.size();
    while(at<input.size()){const auto end=input.find('/',at);parts.emplace_back(input.substr(at,end==input.npos?input.size()-at:end-at));if(end==input.npos)break;at=end+1;}
    const auto replace=[&](std::size_t index,std::string_view canonical){
        if(index>=parts.size())return false;
        const auto native=lower(std::string(canonical));const auto& expected=to_native?std::string(canonical):native;
        if(parts[index]!=expected)return false;
        parts[index]=to_native?native:std::string(canonical);return true;
    };
    std::size_t slot{};
    // On disk a valid network code may also be a global directory's name.
    // Match a known per-network role first, as the archive mapper does, so a
    // network named "runs" does not turn its profile into a global run file.
    bool network_role=false;
    if(!to_native&&parts.size()>1){
        for(const auto name:{"inbound","temp","records","consumed","copies","outbound","journal","profile","profile.tmp"})
            if(parts[1]==name){network_role=true;break;}
        const auto& role=parts[1];
        const auto zone=role.substr(std::min(std::size_t{9},role.size()));
        if(role.substr(0,9)=="outbound."&&(zone.size()==3||zone.size()==4)&&zone.find_first_not_of("0123456789abcdef")==zone.npos)
            network_role=true;
    }
    bool global=false;
    if(!network_role)for(const auto name:{"Inbound","Temp","Records","Consumed","Runs","Tics"})
        if(replace(0,name)){global=true;break;}
    if(!global){
        if(parts.empty())return {};
        if(parts.size()==1)return std::string(to_native?"ftn/external/":"FTN/External/")+parts[0];
        slot=1;
        for(const auto name:{"Inbound","Temp","Records","Consumed","Copies","Outbound","Journal","Profile","Profile.tmp"})
            if(replace(slot,name))break;
        const auto prefix=to_native?std::string("Outbound."):std::string("outbound.");
        const auto zone=parts[slot].substr(std::min(prefix.size(),parts[slot].size()));
        if(parts[slot].substr(0,prefix.size())==prefix&&(zone.size()==3||zone.size()==4)&&zone.find_first_not_of("0123456789abcdef")==zone.npos)
            parts[slot]=(to_native?"outbound.":"Outbound.")+zone;
    }
    const auto family=lower(parts[slot]);
    if(family=="inbound")for(const auto name:{"Secure","Nonsecure","Links"})if(replace(slot+1,name))break;
    if(family=="runs"&&parts.size()==slot+3){if(!replace(slot+2,"Manifest"))replace(slot+2,"Manifest.tmp");}
    std::string result=to_native?"ftn/external":"FTN/External";
    for(const auto& part:parts)result+='/'+part;
    return result;
}
}
std::string ftn_external_physical_path(std::string_view value,FtnExternalPathStyle style){return external_path(value,style,true);}
std::string ftn_external_canonical_path(std::string_view value,FtnExternalPathStyle style){return external_path(value,style,false);}
std::string ftn_external_root(std::string_view network,FtnExternalPathStyle style){
    if(!valid_ftn_code(network))return {};
    const auto result="FTN/External/"+lower(std::string(network));
    return valid_ftn_external_path(result)?ftn_external_physical_path(result,style):std::string{};
}
std::optional<std::string> encode_ftn_external_receive(const FtnExternalReceiveRecord& r){
    if(!valid(r))return {};
    std::string out=(r.run.empty()?"SFX-BINKD-RECEIVE-1\n":"SFX-BINKD-RECEIVE-2\n")+bytes_hex(r.id.bytes)+"\n"+r.network+"\n"+r.relative_file+"\n"+(r.protected_session?"1\n":"0\n")+std::to_string(r.size)+"\n"+std::to_string(r.timestamp)+"\n"+bytes_hex(r.digest)+"\n";
    for(const auto& a:r.remote)out+=format_ftn_address(a)+"\n";
    if(!r.run.empty()){out+="RUN\n"+r.run+"\n"+bytes_hex(r.run_digest)+"\n";for(const auto& a:r.authenticated_remote)out+=format_ftn_address(a)+"\n";}
    if(out.size()>8192)return {};
    return out;
}
std::optional<FtnExternalReceiveRecord> decode_ftn_external_receive(std::string_view text){
    if(text.empty()||text.size()>8192||text.back()!='\n'||text.find('\r')!=text.npos||text.find('\0')!=text.npos)return {};
    std::vector<std::string_view> rows;
    while(!text.empty()){const auto end=text.find('\n');if(end==text.npos||rows.size()==140)return {};rows.push_back(text.substr(0,end));text.remove_prefix(end+1);}
    if(rows.size()<9||(rows[0]!="SFX-BINKD-RECEIVE-1"&&rows[0]!="SFX-BINKD-RECEIVE-2"))return {};
    FtnExternalReceiveRecord r;
    if(!read_hex(rows[1],r.id.bytes)||!read_hex(rows[7],r.digest)||!number(rows[5],r.size)||!number(rows[6],r.timestamp)||(rows[4]!="0"&&rows[4]!="1"))return {};
    r.network=rows[2];r.relative_file=rows[3];r.protected_session=rows[4]=="1";
    std::size_t i=8;for(;i<rows.size()&&rows[i]!="RUN";++i){const auto a=parse_ftn_address(rows[i]);if(!a)return {};r.remote.push_back(*a);}
    if(rows[0]=="SFX-BINKD-RECEIVE-2"){
        if(i+3>rows.size()||rows[i]!="RUN")return {};
        r.run=rows[i+1];if(!read_hex(rows[i+2],r.run_digest))return {};
        for(i+=3;i<rows.size();++i){const auto a=parse_ftn_address(rows[i]);if(!a)return {};r.authenticated_remote.push_back(*a);}
    }else if(i!=rows.size())return {};
    if(!valid(r))return {};
    return r;
}
std::optional<FtnPeerMatch> match_ftn_external_receive(const FtnConfiguration& config,const FtnExternalReceiveRecord& r,std::string_view network){
    if(config.settings.mode!=FtnMailerMode::external_binkd||!valid(r))return {};
    if(network.empty())network=r.network;
    FtnConfiguration scoped;
    for(const auto& n:config.networks)if(n.enabled&&same(n.code,network))scoped.networks.push_back(n);
    if(scoped.networks.size()!=1)return {};
    for(const auto& l:config.links)if(same(l.network,network))scoped.links.push_back(l);
    const auto peer=resolve_ftn_peer(scoped,r.protected_session&&!r.run.empty()?r.authenticated_remote:r.remote);
    if(!peer)return {};
    const auto link=std::find_if(scoped.links.begin(),scoped.links.end(),[&](const auto& l){return same(l.code,peer->link);});
    // The hook carries neither session direction nor CRAM/legacy method. Any
    // enabled configured link may receive mail during its permitted session.
    if(link==scoped.links.end())return {};
    const auto link_prefix=ftn_external_root(r.network)+"/Inbound/Links/";
    if(r.relative_file.rfind(link_prefix,0)==0&&r.relative_file.rfind(link_prefix+lower(peer->link)+"/",0)!=0)return {};
    return peer;
}
bool ftn_external_nonsecure_receive(const FtnConfiguration& config,const FtnExternalReceiveRecord& r){
    if(config.settings.mode!=FtnMailerMode::external_binkd||!config.settings.allow_direct_local_netmail||!valid(r)||r.protected_session)return false;
    const auto profiles=std::count_if(config.networks.begin(),config.networks.end(),[&](const auto& n){return n.enabled&&(r.network.empty()||same(n.code,r.network));});
    if(!profiles||(!r.network.empty()&&profiles!=1))return false;
    if(r.relative_file.rfind(ftn_external_root(r.network)+"/Inbound/Links/",0)==0)return false;
    // Include disabled and other-network peers: failed/ambiguous matching must
    // never turn a known peer into an unconfigured caller with weaker rules.
    for(const auto& link:config.links){
        auto addresses=link.additional_addresses;addresses.push_back(link.address);
        for(const auto& text:addresses)if(const auto address=parse_ftn_address(text))
            for(const auto& remote:r.remote)if(same_ftn_address(*address,remote,true))return false;
    }
    return true;
}
std::optional<FtnExternalOutboundPaths> ftn_external_outbound_paths(const FtnNetwork& network,const FtnLink& link,const FtnOutboundJob& job,FtnExternalPathStyle style){
    const auto own=parse_ftn_address(network.address),peer=parse_ftn_address(link.address);
    const auto root=ftn_external_root(network.code);
    if(root.empty()||!own||!peer||!network.enabled||!link.enabled||!ftn_outbound(link)||!same(link.network,network.code)||!same(job.network,network.code)||!same(job.link,link.code)||!job.id.valid()||!valid_ftn_leaf_name(job.file.name)||!valid_ftn_external_path(job.file.name)||job.file.name.find('/')!=std::string::npos||static_cast<unsigned>(job.flavor)>2)return {};
    if(!own->domain.empty()&&!peer->domain.empty()&&!same(own->domain,peer->domain))return {};
    auto base=root+"/Outbound";
    if(peer->zone!=own->zone)base+="."+hex(peer->zone,peer->zone>4095?4:3);
    base+="/"+hex(peer->net,4)+hex(peer->node,4);
    if(peer->point)base+=".pnt/"+hex(peer->point,8);
    const auto extension=job.flavor==FtnMailFlavor::hold?".hlo":job.flavor==FtnMailFlavor::crash?".clo":".flo";
    FtnExternalOutboundPaths paths{ftn_external_physical_path(base+".bsy",style),ftn_external_physical_path(base+extension,style),ftn_external_physical_path(root+"/Copies/"+bytes_hex(job.id.bytes)+"/"+job.file.name,style)};
    return paths.busy.empty()||paths.flow.empty()||paths.copy.empty()?std::optional<FtnExternalOutboundPaths>{}:std::optional<FtnExternalOutboundPaths>{std::move(paths)};
}
std::string ftn_external_flow_line(const FtnExternalOutboundPaths& paths,FtnExternalPathStyle style){
    const auto canonical=ftn_external_canonical_path(paths.copy,style);
    return valid_ftn_external_path(canonical)&&canonical.rfind("FTN/External/",0)==0?"^"+paths.copy+"\r\n":std::string{};
}
#include "ftn_external_profile.inc"
}
