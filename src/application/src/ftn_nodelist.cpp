#include "sfx/application/ftn_nodelist.hpp"
#include <algorithm>
#include <vector>
namespace sfx::application {
namespace {
std::string fold(std::string_view s){std::string out(s);for(auto& c:out)if(c>='a'&&c<='z')c=static_cast<char>(c-'a'+'A');return out;}
bool number(std::string_view s,std::uint16_t& out){if(s.empty()||s.size()>5)return false;unsigned n{};for(const auto c:s){if(c<'0'||c>'9')return false;n=n*10+static_cast<unsigned>(c-'0');if(n>65535)return false;}out=static_cast<std::uint16_t>(n);return true;}
bool host(std::string_view s){if(s.empty()||s.size()>253||s.front()=='.'||s.back()=='.')return false;for(const auto c:s)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-'))return false;return true;}
std::vector<std::string_view> split(std::string_view s,char separator){std::vector<std::string_view> fields;while(true){const auto end=s.find(separator);fields.push_back(s.substr(0,end));if(end==s.npos)break;s.remove_prefix(end+1);}return fields;}
}
std::optional<FtnNodelistEndpoint> ftn_nodelist_endpoint(std::string_view text,const FtnAddress& target){
    if(text.size()>32*1024*1024||!target.zone||!target.net)return {};
    std::uint16_t zone{},net{};FtnAddress boss;std::optional<FtnNodelistEndpoint> found;unsigned matches{};std::size_t lines{};
    while(!text.empty()){
        if(++lines>500000)return {};
        const auto end=text.find('\n');auto line=text.substr(0,end);text=end==text.npos?std::string_view{}:text.substr(end+1);if(!line.empty()&&line.back()=='\r')line.remove_suffix(1);if(line.empty()||line.front()==';')continue;if(line.size()>4096)return {};
        const auto f=split(line,',');if(f.size()<2)continue;const auto kind=fold(f[0]);
        if(kind=="BOSS"){const auto a=parse_ftn_address(f[1]);boss=a.value_or(FtnAddress{});continue;}
        std::uint16_t n{};if(!number(f[1],n)){if(kind=="ZONE"){zone=net=0;boss={};}else if(kind=="HOST"||kind=="REGION"){net=0;boss={};}continue;}FtnAddress address;
        if(kind=="ZONE"){zone=n;net=n;boss={};address={zone,net,0,0,{}};}
        else if(kind=="REGION"||kind=="HOST"){net=n;boss={};address={zone,net,0,0,{}};}
        else if(kind=="POINT"){address=boss;address.point=n;}
        else if(kind.empty()||kind=="HUB"||kind=="PVT"||kind=="HOLD"||kind=="DOWN"){boss={};address={zone,net,n,0,{}};}
        else continue;
        if(address.zone!=target.zone||address.net!=target.net||address.node!=target.node||address.point!=target.point)continue;
        if(++matches>1)return {};
        if(kind=="DOWN"||kind=="HOLD"||f.size()<8)return {};
        FtnNodelistEndpoint value;bool bink{},invalid{};
        for(std::size_t i=7;i<f.size();++i){const auto flag=fold(f[i]);if(flag.rfind("INA:",0)==0){const auto h=std::string(f[i].substr(4));if(!value.host.empty()&&fold(value.host)!=fold(h))invalid=true;value.host=h;}
            else if(flag=="IBN"||flag.rfind("IBN:",0)==0){if(bink){invalid=true;continue;}bink=true;if(flag.size()>4){const auto parts=split(f[i].substr(4),':');std::uint16_t port{};if(parts.size()==1&&number(parts[0],port)){value.port=port;}else if(parts.size()<=2){if(!parts[0].empty()){if(!value.host.empty()&&fold(value.host)!=fold(parts[0]))invalid=true;value.host=std::string(parts[0]);}if(parts.size()==2&&(!number(parts[1],port)||!port))invalid=true;else if(parts.size()==2)value.port=port;}else invalid=true;}}
        }
        if(!invalid&&bink&&value.port&&host(value.host))found=std::move(value);
    }
    return found;
}
}
