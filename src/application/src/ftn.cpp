#include "sfx/application/ftn.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <tuple>

namespace sfx::application {
namespace {
bool text(std::string_view s, std::size_t max, bool blank=false) noexcept {
    if(s.size()>max||(!blank&&s.empty()))return false;
    for(std::size_t i=0;i<s.size();){
        const auto first=static_cast<unsigned char>(s[i++]);
        if(first<128){if(first<32||first==127)return false;continue;}
        unsigned count{},value{},minimum{};
        if(first>=0xc2&&first<=0xdf){count=1;value=first&31;minimum=0x80;}
        else if(first>=0xe0&&first<=0xef){count=2;value=first&15;minimum=0x800;}
        else if(first>=0xf0&&first<=0xf4){count=3;value=first&7;minimum=0x10000;}
        else return false;
        if(count>s.size()-i)return false;
        for(unsigned n=0;n<count;++n){const auto c=static_cast<unsigned char>(s[i++]);if((c&0xc0)!=0x80)return false;value=(value<<6)|(c&63);}
        if(value<minimum||value>0x10ffff||(value>=0xd800&&value<=0xdfff)||(value>=0x80&&value<=0x9f)||value==0x2028||value==0x2029)return false;
    }
    return true;
}
bool alnum(char c) noexcept {
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9');
}
std::string lower(std::string s) {
    for(auto& c:s)if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');
    return s;
}
bool number(std::string_view s, std::uint16_t& n) {
    if(s.empty()||s.size()>5)return false;
    unsigned value{};
    for(char c:s){if(c<'0'||c>'9')return false;value=value*10+static_cast<unsigned>(c-'0');}
    if(value>65535)return false;
    n=static_cast<std::uint16_t>(value);return true;
}
bool host(std::string_view s) noexcept {
    if(!text(s,253))return false;
    for(char c:s)if(!alnum(c)&&c!='.'&&c!='-'&&c!=':'&&c!='['&&c!=']')return false;
    return true;
}
bool addresses(std::string_view primary,const std::vector<std::string>& extra,bool required) {
    if(extra.size()>32)return false;
    if(!required){if(!text(primary,96,true))return false;for(const auto& s:extra)if(!text(s,96,true))return false;return true;}
    std::set<std::string> used;
    if(!primary.empty()){
        const auto a=parse_ftn_address(primary);if(!a)return false;
        used.insert(format_ftn_address(*a));
    }else if(required)return false;
    for(const auto& s:extra){const auto a=parse_ftn_address(s);if(!a||!used.insert(format_ftn_address(*a)).second)return false;}
    return true;
}
bool envelopes(const FtnProtectedCredentials& c) noexcept {
    return c.session.size()<=4096&&c.packet.size()<=4096&&c.areafix.size()<=4096&&c.tic.size()<=4096;
}
}

bool valid_ftn_display_text(std::string_view s,std::size_t maximum,bool blank) noexcept {return text(s,maximum,blank);}
bool valid_ftn_code(std::string_view s) noexcept {
    if(s.empty()||s.size()>48||!alnum(s.front()))return false;
    for(char c:s)if(!alnum(c)&&c!='-'&&c!='_')return false;
    // These otherwise-valid codes cannot name ordinary directories on Windows.
    const auto equal=[](std::string_view a,std::string_view b){
        if(a.size()!=b.size())return false;
        for(std::size_t i=0;i<a.size();++i){char c=a[i];if(c>='a'&&c<='z')c=static_cast<char>(c-'a'+'A');if(c!=b[i])return false;}return true;
    };
    for(const auto word:{"CON","PRN","AUX","NUL","CLOCK$","CONIN$","CONOUT$"})if(equal(s,word))return false;
    if(s.size()==4&&(equal(s.substr(0,3),"COM")||equal(s.substr(0,3),"LPT"))&&s[3]>='1'&&s[3]<='9')return false;
    return true;
}
bool valid_ftn_echo_tag(std::string_view s) noexcept {
    if(s.empty()||s.size()>128)return false;
    for(unsigned char c:s)if(c<=32||c>=127||c=='/'||c=='\\'||c==':'||c=='@'||c=='*'||c=='?')return false;
    return true;
}
std::optional<FtnAddress> parse_ftn_address(std::string_view s) {
    if(s.size()>96)return {};
    FtnAddress a;
    const auto at=s.find('@');
    if(at!=std::string_view::npos){
        const auto domain=s.substr(at+1);
        if(domain.empty()||domain.size()>48||!alnum(domain.front())||!alnum(domain.back()))return {};
        for(char c:domain)if(!alnum(c)&&c!='-'&&c!='.')return {};
        if(domain.find("..")!=std::string_view::npos)return {};
        a.domain=lower(std::string(domain));s=s.substr(0,at);
    }
    const auto colon=s.find(':'),slash=s.find('/'),dot=s.find('.');
    if(colon==std::string_view::npos||slash==std::string_view::npos||colon>=slash)return {};
    const auto end=dot==std::string_view::npos?s.size():dot;
    if(end<=slash||!number(s.substr(0,colon),a.zone)||!a.zone||
       !number(s.substr(colon+1,slash-colon-1),a.net)||!a.net||
       !number(s.substr(slash+1,end-slash-1),a.node))return {};
    if(dot!=std::string_view::npos&&!number(s.substr(dot+1),a.point))return {};
    return a;
}
std::string format_ftn_address(const FtnAddress& a) {
    auto s=std::to_string(a.zone)+":"+std::to_string(a.net)+"/"+std::to_string(a.node);
    if(a.point)s+="."+std::to_string(a.point);
    if(!a.domain.empty())s+="@"+lower(a.domain);
    return s;
}
bool valid_ftn_echo_list_url(std::string_view s) noexcept {
    if(!text(s,2048))return false;
    std::size_t prefix{};
    if(s.substr(0,7)=="http://")prefix=7;
    else if(s.substr(0,8)=="https://")prefix=8;
    else return false;
    const auto end=s.find_first_of("/?#",prefix);
    const auto authority=s.substr(prefix,end==std::string_view::npos?s.size()-prefix:end-prefix);
    if(authority.empty()||authority.find('@')!=std::string_view::npos)return false;
    for(unsigned char c:s)if(c<=32||c=='\\'||c=='"'||c=='<'||c=='>')return false;
    return s.find('#')==std::string_view::npos;
}
bool ftn_areafix_enabled(const FtnLink& l) noexcept {
    return l.areafix_enabled.value_or(!l.credentials.areafix.empty());
}
bool ftn_outbound(const FtnLink& l) noexcept {return l.use!=FtnConnectionUse::inbound_only;}
bool ftn_inbound(const FtnLink& l) noexcept {return l.use!=FtnConnectionUse::outbound_only;}

std::vector<FtnValidationIssue> validate_ftn_configuration(const FtnConfiguration& c) {
    std::vector<FtnValidationIssue> issues;
    const auto issue=[&](const char* field,const char* reason){issues.push_back({field,reason});};
    if(c.revision<1||c.revision==INT64_MAX)issue("revision","invalid");
    if(c.message_catalog_revision<0||c.file_catalog_revision<0)issue("catalog_revision","invalid");
    const bool native=c.settings.mode==FtnMailerMode::native;
    if(static_cast<unsigned>(c.settings.mode)>2||(native&&(!c.settings.port||!host(c.settings.bind_address)))||!text(c.settings.bind_address,253,true)||
       !text(c.settings.external_executable,1024,true)||!text(c.settings.tls_certificate,1024,true)||!text(c.settings.tls_private_key,1024,true))issue("mailer","invalid");
    if(!text(c.settings.msg_netmail_directory,1024,true))issue("mailer.msg_directory","required");
    if(native&&!c.settings.tls_port)issue("mailer.tls_port","invalid");
    if(native&&c.settings.tls_listener_enabled){
        if(c.settings.tls_certificate.empty()||c.settings.tls_private_key.empty())issue("mailer.tls_listener","certificate_and_key_required");
        if(c.settings.listener_enabled&&c.settings.port==c.settings.tls_port)issue("mailer.tls_port","duplicate_listener_port");
    }
    if(c.networks.size()>128||c.links.size()>4096||c.message_echoes.size()>16384||
       c.file_echoes.size()>16384||c.permissions.size()>65536||c.routes.size()>16384){issue("catalog","limit");return issues;}
    std::set<std::string> networks;
    for(const auto& n:c.networks){
        if(!valid_ftn_code(n.code)||!networks.insert(lower(n.code)).second)issue("network.code","invalid_or_duplicate");
        if(!text(n.name,96)||!text(n.description,256,true)||!text(n.origin_line,128,true)||!text(n.echo_list_url,2048,true)||(n.enabled&&n.echo_list_source==FtnEchoListSource::url&&!n.echo_list_url.empty()&&!valid_ftn_echo_list_url(n.echo_list_url)))issue("network","invalid_text_or_url");
        if(!addresses(n.address,n.additional_addresses,n.enabled))issue("network.address","invalid_or_required");
        if(n.message_group<0||!text(n.new_group_name,64,true))issue("network.message_group","invalid");
        if(static_cast<unsigned>(n.echo_list_source)>1||!text(n.echo_list_file,1024,true)||
           !text(n.nodelist_file,1024,true)||!text(n.msg_netmail_directory,1024,true)||
           (n.echo_list_source==FtnEchoListSource::local_file&&n.enabled&&n.echo_list_file.empty()))issue("network.sources","invalid");
        if(n.msg_netmail_enabled.value_or(false)&&n.msg_netmail_directory.empty())issue("network.msg_directory","required");
    }
    std::set<std::pair<std::string,std::string>> links,echoes,files;
    const auto known=[&](const std::string& n){return networks.count(lower(n))!=0;};
    for(const auto& l:c.links){
        const auto parent=std::find_if(c.networks.begin(),c.networks.end(),[&](const auto& n){return lower(n.code)==lower(l.network);});
        const bool active=l.enabled&&parent!=c.networks.end()&&parent->enabled;
        if(!known(l.network)||!valid_ftn_code(l.code)||!links.emplace(lower(l.network),lower(l.code)).second)issue("link","invalid_or_duplicate");
        if(!text(l.name,96)||!text(l.host,253,true)||!text(l.tls_server_name,253,true)||!text(l.tls_ca_file,1024,true)||!envelopes(l.credentials)||static_cast<unsigned>(l.use)>2)issue("link","invalid");
        if(!addresses(l.address,l.additional_addresses,active))issue("link.address","invalid_or_required");
        if(native&&active&&ftn_outbound(l)&&((!host(l.host)&&!(l.host.empty()&&parent!=c.networks.end()&&!parent->nodelist_file.empty()))||(!l.port&&!(parent!=c.networks.end()&&!parent->nodelist_file.empty()))))issue("link.endpoint","outbound_host_required");
        if(active&&ftn_areafix_enabled(l)&&l.credentials.areafix.empty())issue("link.areafix","separate_password_required");
    }
    for(const auto& l:c.links)if(l.forward_missing_echoes){
        const auto via=std::find_if(c.links.begin(),c.links.end(),[&](const auto& v){return lower(v.network)==lower(l.network)&&lower(v.code)==lower(l.forward_via_link)&&v.code!=l.code;});
        if(via==c.links.end()||!ftn_outbound(*via)||via->credentials.areafix.empty())issue("link.forwarding","configured_upstream_required");
    }
    for(const auto& n:c.networks)if(!n.default_link.empty()){
        const auto via=std::find_if(c.links.begin(),c.links.end(),[&](const auto& l){return lower(l.network)==lower(n.code)&&lower(l.code)==lower(n.default_link);});
        if(via==c.links.end()||!ftn_outbound(*via)||(n.enabled&&!via->enabled))issue("network.default_hub","outbound_connection_required");
    }
    for(const auto& e:c.message_echoes){
        if(!known(e.network)||!valid_ftn_echo_tag(e.tag)||!echoes.emplace(lower(e.network),lower(e.tag)).second)issue("echo","invalid_or_duplicate");
        if(!text(e.description,512,true)||e.local_echo<0||e.access_level<1||e.access_level>999||
           e.read_level<1||e.read_level>999||e.post_level<1||e.post_level>999)issue("echo","invalid_settings");
        if(!e.store_locally&&e.local_echo)issue("echo","pass_through_local_target");
        if(!text(e.local_name,64,true)||(e.store_locally&&!e.local_echo&&e.local_name.empty()&&e.tag.size()>64))issue("echo.local_name","name_required");
        if(e.update_local_retention>3||(e.maximum_messages&&(!*e.maximum_messages||*e.maximum_messages>999999999))||
           (e.maximum_age_days&&(!*e.maximum_age_days||*e.maximum_age_days>65535)))issue("echo.retention","invalid");
    }
    for(const auto& e:c.file_echoes){
        if(!known(e.network)||!valid_ftn_echo_tag(e.tag)||!files.emplace(lower(e.network),lower(e.tag)).second||
           !text(e.description,512,true)||e.file_area<=0)issue("file_echo","invalid_or_duplicate");
    }
    std::set<std::tuple<std::string,std::string,std::string,bool>> permissions;
    for(const auto& p:c.permissions){
        if(!links.count({lower(p.network),lower(p.link)})||!(p.file_echo?files:echoes).count({lower(p.network),lower(p.tag)})||
           !permissions.emplace(lower(p.network),lower(p.link),lower(p.tag),p.file_echo).second||
           (p.file_echo&&p.manage))issue("echo_permission","invalid_or_duplicate");
    }
    std::set<std::pair<std::string,std::string>> routes;
    for(const auto& r:c.routes){
        const auto destination=parse_ftn_address(r.destination);
        if(!destination||!links.count({lower(r.network),lower(r.via_link)})||static_cast<unsigned>(r.flavor)>2||
           !routes.emplace(lower(r.network),destination?format_ftn_address(*destination):"").second)issue("route","invalid_or_duplicate");
    }
    if(c.setup_subscriptions){
        const auto& request=*c.setup_subscriptions;
        const auto net=std::find_if(c.networks.begin(),c.networks.end(),[&](const auto& n){return n.code==request.network;});
        const auto link=std::find_if(c.links.begin(),c.links.end(),[&](const auto& l){return l.network==request.network&&l.code==request.link;});
        if(net==c.networks.end()||!net->enabled||link==c.links.end()||!link->enabled||!ftn_outbound(*link)||!ftn_inbound(*link)||link->credentials.areafix.empty()||request.tags.empty()||request.tags.size()>4096)issue("setup.areafix","enabled_hub_and_password_required");
        std::set<std::string> targets;
        for(const auto& tag:request.tags){
            if(!valid_ftn_echo_tag(tag)||!targets.insert(lower(tag)).second||std::none_of(c.message_echoes.begin(),c.message_echoes.end(),[&](const auto& e){return lower(e.network)==lower(request.network)&&lower(e.tag)==lower(tag);})||std::none_of(c.permissions.begin(),c.permissions.end(),[&](const auto& p){return lower(p.network)==lower(request.network)&&lower(p.link)==lower(request.link)&&lower(p.tag)==lower(tag)&&!p.file_echo&&p.receive;}))issue("setup.areafix","explicit_receive_echo_required");
        }
    }
    return issues;
}

FtnEchoList parse_ftn_echo_list(std::string_view input) {
    FtnEchoList result;
    if(input.size()>4*1024*1024){result.complete=false;result.issues.push_back({0,"list_too_large"});return result;}
    std::set<std::string> tags;std::size_t line{};
    while(!input.empty()){
        ++line;const auto end=input.find('\n');auto s=input.substr(0,end);
        if(end==std::string_view::npos)input={};else input.remove_prefix(end+1);
        if(!s.empty()&&s.back()=='\r')s.remove_suffix(1);
        while(!s.empty()&&(s.front()==' '||s.front()=='\t'))s.remove_prefix(1);
        if(s.empty()||s.front()==';'||s.front()=='#')continue;
        if(line>50000||s.size()>4096){result.complete=false;result.issues.push_back({line,"list_limit"});return result;}
        const auto split=s.find_first_of(" \t");auto tag=s.substr(0,split);
        std::string_view description;
        if(split!=std::string_view::npos){description=s.substr(split+1);while(!description.empty()&&(description.front()==' '||description.front()=='\t'))description.remove_prefix(1);}
        if(!valid_ftn_echo_tag(tag)||!text(description,512,true)){result.issues.push_back({line,"invalid_entry"});continue;}
        std::string normalized(tag);for(auto& ch:normalized)if(ch>='a'&&ch<='z')ch=static_cast<char>(ch-'a'+'A');
        if(!tags.insert(normalized).second){result.issues.push_back({line,"duplicate_tag"});continue;}
        result.entries.push_back({std::move(normalized),std::string(description),line,true});
    }
    return result;
}
} // namespace sfx::application
