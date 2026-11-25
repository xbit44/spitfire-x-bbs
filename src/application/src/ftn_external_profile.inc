// Supported stock BinkD profile grammar. This deliberately has no include,
// wildcard, alternate password, arbitrary command or executable expansion.
namespace {
std::optional<std::vector<std::string>> profile_words(std::string_view line){
    std::vector<std::string> result;std::size_t at{};
    while(at<line.size()){
        while(at<line.size()&&(line[at]==' '||line[at]=='\t'||line[at]=='\r'))++at;
        if(at==line.size()||line[at]=='#')break;
        std::string word;const bool quoted=line[at]=='"';if(quoted)++at;bool closed=!quoted;
        while(at<line.size()){
            const char c=line[at++];if(quoted&&c=='"'){closed=true;break;}
            if(!quoted&&(c==' '||c=='\t'||c=='\r'))break;
            if(static_cast<unsigned char>(c)<32||c=='\\'||c=='"'||c=='#')return {};
            word+=c;if(word.size()>512)return {};
        }
        if(!closed||word.empty()||(quoted&&at<line.size()&&line[at]!=' '&&line[at]!='\t'&&line[at]!='\r'&&line[at]!='#'))return {};
        result.push_back(std::move(word));if(result.size()>12)return {};
    }return result;
}
}
std::optional<FtnExternalProfile> parse_ftn_external_profile(std::string_view text,FtnExternalPathStyle style){
    if(style!=FtnExternalPathStyle::canonical&&style!=FtnExternalPathStyle::linux_lowercase_v1)return {};
    const bool native=style==FtnExternalPathStyle::linux_lowercase_v1;
    const std::string base=native?"ftn/external":"FTN/External";if(text.empty()||text.size()>49152||text.find('\0')!=text.npos)return {};
    FtnExternalProfile result;std::set<std::string> singleton,nodes,domains,codes;unsigned local_count{};
    const auto append=[&](const std::vector<std::string>& words){for(std::size_t i=0;i<words.size();++i){if(i)result.configuration+=' ';result.configuration+='"';result.configuration+=words[i];result.configuration+='"';}result.configuration+='\n';};
    while(!text.empty()){
        const auto end=text.find('\n');const auto line=text.substr(0,end);text=end==text.npos?std::string_view{}:text.substr(end+1);
        const auto parsed=profile_words(line);if(!parsed)return {};const auto& w=*parsed;if(w.empty())continue;
        const auto& key=w[0];
        if(key=="domain"){
            if(w.size()!=4||!valid_ftn_code(w[1])||!domains.insert(lower(w[1])).second||domains.size()>64)return {};
            const auto& path=w[2];const auto prefix=base+"/";const std::string suffix=native?"/outbound":"/Outbound";
            if(path.rfind(prefix,0)!=0||path.size()<=prefix.size()+suffix.size()||path.substr(path.size()-suffix.size())!=suffix)return {};
            const auto code=path.substr(prefix.size(),path.size()-prefix.size()-suffix.size());
            if(!valid_ftn_code(code)||!codes.insert(lower(code)).second||path!=ftn_external_root(code,style)+suffix)return {};
            std::uint64_t zone{};if(!number(w[3],zone)||!zone||zone>65535)return {};
            result.networks.push_back({code,lower(w[1]),static_cast<std::uint16_t>(zone)});
        }else if(key=="address"){
            if(w.size()!=2||++local_count>64)return {};
            const auto address=parse_ftn_address(w[1]);if(!address||address->domain.empty())return {};result.local.push_back(*address);
        }else if(key=="node"){
            // Explicit address, no shared defaults, no password lists or !.
            const std::size_t offset=w.size()==8&&w[2]=="-nr"?1:0;
            if(w.size()!=7+offset||w[5+offset]!="-"||w[6+offset]!="-"||(w[4+offset]!="i"&&w[4+offset]!="c"&&w[4+offset]!="h"&&w[4+offset]!="-"))return {};
            const auto address=parse_ftn_address(w[1]);if(!address||address->domain.empty()||!nodes.insert(lower(format_ftn_address(*address))).second||nodes.size()>512)return {};
            result.remote.push_back(*address);
            if(w[2+offset].empty()||w[2+offset][0]=='-'||w[2+offset].find_first_of(" \t#;*?,$`")!=w[2+offset].npos)return {};
            if(w[3+offset].empty()||w[3+offset].size()>128||w[3+offset].find_first_of("! ,\t#;*$`?")!=w[3+offset].npos)return {};
            if(w[3+offset]!="-")result.protected_remote.push_back(*address);
        }else if(key=="inbound"||key=="inbound-nonsecure"||key=="temp-inbound"){
            const auto expected=base+(native?(key=="inbound"?"/inbound/secure":key=="inbound-nonsecure"?"/inbound/nonsecure":"/temp"):(key=="inbound"?"/Inbound/Secure":key=="inbound-nonsecure"?"/Inbound/Nonsecure":"/Temp"));
            if(w.size()!=2||w[1]!=expected||!singleton.insert(key).second)return {};
        }else if(key=="listen"||key=="sysname"||key=="location"||key=="sysop"||key=="nodeinfo"||key=="timeout"||key=="connect-timeout"||key=="maxservers"||key=="maxclients"){
            if(w.size()!=2||!singleton.insert(key).second)return {};
            if(key=="maxservers"||key=="maxclients"){std::uint64_t value{};if(!number(w[1],value)||!value||value>64)return {};}
            if(key=="timeout"||key=="connect-timeout"){auto value=w[1];if(!value.empty()&&value.back()=='s')value.pop_back();std::uint64_t seconds{};if(!number(value,seconds)||!seconds||seconds>3600)return {};}
        }else return {};
        append(w);
    }
    if(domains.empty()||result.local.empty()||!singleton.count("listen")||!singleton.count("inbound")||!singleton.count("inbound-nonsecure")||!singleton.count("temp-inbound"))return {};
    for(const auto& address:result.local)if(!domains.count(lower(address.domain)))return {};
    for(const auto& address:result.remote)if(!domains.count(lower(address.domain)))return {};
    for(const auto& network:result.networks)if(std::none_of(result.local.begin(),result.local.end(),[&](const auto& address){return lower(address.domain)==network.domain;}))return {};
    return result;
}
