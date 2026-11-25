#include "sfx/application/door.hpp"
#include "sfx/application/door_storage.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>

namespace sfx::application {
std::vector<DoorDefinition> fresh_install_starter_doors(StarterDoorPlatform platform) {
    if(platform!=StarterDoorPlatform::windows&&platform!=StarterDoorPlatform::linux_native)return {};
    const bool native_linux=platform==StarterDoorPlatform::linux_native;
    const auto local=[native_linux](DoorId id, std::string name, std::string code,
        std::string directory, std::string executable) {
        DoorDefinition d; d.id=id; d.name=std::move(name); d.code=std::move(code);
        d.enabled=true; d.kind=DoorExecutionKind::local;
        d.security_level=10; d.execution.level=10;
        d.program.startup_directory=std::move(directory);
        d.program.command_line=std::move(executable)+(native_linux?" --drop \"%D/DOORDROP.INI\"":" --drop \"%D\\DOORDROP.INI\"");
        d.program.type=DoorProgramType::native; d.program.io=DoorIo::socket;
        d.program.drop_file=DoorDropFile::doordrop_ini;
        return d;
    };
    auto game=local(1,"SFX Guess a Number","SFXGUESS32",native_linux?"doors/sfxguess32":"Doors/SFXGuess32",native_linux?"sfxguess32":"SFXGuess32.exe");
    DoorDefinition gateway; gateway.id=2; gateway.name="X-Gate Game Server";
    gateway.code="XGATE"; gateway.enabled=true; gateway.security_level=10;
    gateway.execution.level=10; gateway.rlogin.host="x-bit.org";
    gateway.rlogin.port=5130; gateway.rlogin.terminal_type="xtrn_sec=xtrn_sec";
    gateway.rlogin.prompt_credential=true; gateway.rlogin.quiet=true;
    gateway.rlogin.pause=false;
    auto news=local(3,"NewsWire-X RSS","NEWSWIRE",native_linux?"doors/newswire-x-rss":"Doors/NewsWire-X-RSS",native_linux?"newswire-x-rss":"NewsWire-X-RSS.exe");
    return {std::move(game),std::move(gateway),std::move(news)};
}
std::optional<DoorCatalog> without_door_category(DoorCatalog c,DoorMenuId id) {
    if(!id||!find_door_menu(c,id)||!validate_door_catalog(c).valid())return {};
    std::set<DoorMenuId> menus{id};std::set<DoorId> doors;
    std::vector<DoorMenuId> pending{id};
    for(std::size_t i=0;i<pending.size();++i)for(const auto& e:c.entries)if(e.menu==pending[i]){
        if(e.kind==DoorEntryKind::door)doors.insert(e.target);
        else if(menus.insert(e.target).second)pending.push_back(e.target);
    }
    c.entries.erase(std::remove_if(c.entries.begin(),c.entries.end(),[&](const auto& e){
        return menus.count(e.menu)||(e.kind==DoorEntryKind::door?doors.count(e.target):menus.count(e.target));
    }),c.entries.end());
    c.doors.erase(std::remove_if(c.doors.begin(),c.doors.end(),[&](const auto& d){return doors.count(d.id);}),c.doors.end());
    c.menus.erase(std::remove_if(c.menus.begin(),c.menus.end(),[&](const auto& m){return menus.count(m.id);}),c.menus.end());
    if(!validate_door_catalog(c).valid())return {};
    return c;
}
std::optional<DoorCatalog> changed_door_catalog(DoorCatalog c,const DoorConfigurationChange& r) {
    if(!valid_door_change(r)||c.revision!=r.expected_revision)return {};
    if(r.action==DoorConfigurationAction::remove_menu)return without_door_category(std::move(c),r.menu.id);
    const auto apply=[&]()->bool {
    const auto save=[](auto& list,const auto& item,auto predicate){const auto i=std::find_if(list.begin(),list.end(),predicate);if(i==list.end())list.push_back(item);else *i=item;};
    const auto remove=[](auto& list,auto predicate){const auto i=std::find_if(list.begin(),list.end(),predicate);if(i==list.end())return false;list.erase(i);return true;};
    switch(r.action){
    case DoorConfigurationAction::save_door:
        if(r.create_only==(find_door(c,r.door.id)!=nullptr))return false;
        save(c.doors,r.door,[&](const auto& d){return d.id==r.door.id;});break;
    case DoorConfigurationAction::save_menu:
        if(r.create_only==(find_door_menu(c,r.menu.id)!=nullptr))return false;
        save(c.menus,r.menu,[&](const auto& m){return m.id==r.menu.id;});break;
    case DoorConfigurationAction::save_entry:
        if(r.move_entry){
            if(r.source_menu==r.entry.menu)return false;
            const auto source=std::find_if(c.entries.begin(),c.entries.end(),[&](const auto& e){return e.menu==r.source_menu&&e.number==r.previous_number;});
            if(source==c.entries.end()||source->kind!=r.entry.kind||source->target!=r.entry.target)return false;
            c.entries.erase(source);
        }
        {const auto identity=r.previous_number&&!r.move_entry?r.previous_number:r.entry.number;
        const bool exists=std::any_of(c.entries.begin(),c.entries.end(),[&](const auto& e){return e.menu==r.entry.menu&&e.number==identity;});
        if(r.create_only==exists)return false;}
        if(r.previous_number && !r.move_entry && r.previous_number!=r.entry.number){
            if(std::any_of(c.entries.begin(),c.entries.end(),[&](const auto& e){return e.menu==r.entry.menu && e.number==r.entry.number;}) ||
                !remove(c.entries,[&](const auto& e){return e.menu==r.entry.menu && e.number==r.previous_number;}))return false;
        }
        save(c.entries,r.entry,[&](const auto& e){return e.menu==r.entry.menu && e.number==r.entry.number;});
        if(!r.order.empty()){
            std::vector<DoorMenuEntry> entries;
            for(const auto& e:c.entries)if(e.menu==r.entry.menu)entries.push_back(e);
            if(entries.size()!=r.order.size())return false;
            std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.number<b.number;});
            std::vector<DoorMenuEntry> ordered;
            for(const auto n:r.order){const auto i=std::find_if(entries.begin(),entries.end(),[&](const auto& e){return e.number==n;});if(i==entries.end())return false;ordered.push_back(*i);}
            for(std::size_t i=0;i<ordered.size();++i)ordered[i].number=entries[i].number;
            c.entries.erase(std::remove_if(c.entries.begin(),c.entries.end(),[&](const auto& e){return e.menu==r.entry.menu;}),c.entries.end());
            c.entries.insert(c.entries.end(),ordered.begin(),ordered.end());
        }
        break;
    case DoorConfigurationAction::remove_door:if(!remove(c.doors,[&](const auto& d){return d.id==r.door.id;}))return false;break;
    case DoorConfigurationAction::remove_menu:if(!remove(c.menus,[&](const auto& m){return m.id==r.menu.id;}))return false;break;
    case DoorConfigurationAction::remove_entry:if(!remove(c.entries,[&](const auto& e){return e.menu==r.entry.menu && e.number==r.entry.number;}))return false;break;
    case DoorConfigurationAction::duplicate_menu:return duplicate_door_menu(c,r.source_menu,r.menu.id,r.menu.name);
    }
    return validate_door_catalog(c).valid();

    };
    if(!apply())return {};
    return c;
}
bool valid_door_change(const DoorConfigurationChange& c) noexcept {
    if(!c.operation.valid() || c.expected_revision<1 || c.expected_revision==INT64_MAX || c.previous_number>99 ||
        (c.action!=DoorConfigurationAction::save_entry && c.previous_number) ||
        (c.action!=DoorConfigurationAction::duplicate_menu && !c.move_entry && c.source_menu) ||
        (c.create_only && c.action!=DoorConfigurationAction::save_door && c.action!=DoorConfigurationAction::save_menu && c.action!=DoorConfigurationAction::save_entry) ||
        (c.create_only && c.previous_number && !c.move_entry) ||
        (c.move_entry&&(c.action!=DoorConfigurationAction::save_entry||!c.create_only||!c.previous_number||c.source_menu>999)) ||
        (!c.order.empty()&&c.action!=DoorConfigurationAction::save_entry) || c.order.size()>99)return false;
    std::array<bool,100> numbers{};
    for(const auto n:c.order){if(!n||n>99||numbers[n])return false;numbers[n]=true;}
    switch(c.action){
    case DoorConfigurationAction::save_door:return valid_door_definition(c.door);
    case DoorConfigurationAction::save_menu:return c.menu.id<=999 && valid_door_text(c.menu.name,64)&&valid_door_requirements(c.menu.access)&&valid_door_requirements(c.menu.execution);
    case DoorConfigurationAction::save_entry:return c.entry.menu<=999 && c.entry.number>=1 && c.entry.number<=99 &&
        static_cast<unsigned>(c.entry.kind)<=1 && c.entry.target>=1 && c.entry.target<=999 &&
        valid_door_text(c.entry.title,64) && valid_door_text(c.entry.description,256,true);
    case DoorConfigurationAction::remove_door:return c.door.id>=1 && c.door.id<=999;
    case DoorConfigurationAction::remove_menu:return c.menu.id>=1 && c.menu.id<=999;
    case DoorConfigurationAction::remove_entry:return c.entry.menu<=999 && c.entry.number>=1 && c.entry.number<=99;
    case DoorConfigurationAction::duplicate_menu:return c.menu.id>=1 && c.menu.id<=999 && c.source_menu<=999 && c.menu.id!=c.source_menu && valid_door_text(c.menu.name,64);
    }
    return false;
}
bool valid_door_text(std::string_view s, std::size_t maximum, bool empty) noexcept {
    if ((!empty && s.empty()) || s.size()>maximum) return false;
    for (unsigned char c:s) if (c<32 || c==127) return false;
    return true;
}
namespace {
bool valid_rlogin_settings(const RloginTarget& t,bool slot) noexcept {
    if (!valid_door_text(t.host,253,slot) || !t.port ||
        static_cast<unsigned>(t.authentication)>2 || static_cast<unsigned>(t.server_identity)>1 ||
        !valid_door_text(t.server_name,127,t.server_identity==RloginIdentity::caller_alias) ||
        !valid_door_text(t.client_name,127,true) || !valid_door_text(t.terminal_type,127) ||
        !valid_door_text(t.salt,128,true) || t.protected_credential.size()>4096 ||
        t.protected_pepper.size()>4096 || t.connection_timeout_seconds<1 ||
        t.connection_timeout_seconds>120 || (t.prompt_credential && !t.protected_credential.empty())) return false;
    // No URL, path, userinfo, service/port suffix or whitespace ambiguity.
    for (unsigned char c:t.host) if (c<=32 || c>=127 || c=='/' || c=='\\' || c=='@' || c=='[' || c==']') return false;
    if (t.authentication==RloginAuthentication::synchronet_caller_hash)
        return t.prompt_credential && t.protected_credential.empty();
    if (t.authentication==RloginAuthentication::synchronet_predefined_hash)
        return t.protected_pepper.empty() && (t.prompt_credential || !t.protected_credential.empty());
    return t.protected_pepper.empty();
}
}
bool valid_rlogin_target(const RloginTarget& t) noexcept {return valid_rlogin_settings(t,false);}
bool valid_telnet_target(const TelnetTarget& t,bool slot) noexcept {
    if(!valid_door_text(t.host,253,slot)||!t.port||!valid_door_text(t.terminal_type,127)||t.connection_timeout_seconds<1||t.connection_timeout_seconds>120)return false;
    for(unsigned char c:t.host)if(c<=32||c>=127||c=='/'||c=='\\'||c=='@'||c=='['||c==']')return false;
    return true;
}
bool valid_door_code(std::string_view code) noexcept {
    if(code.empty()||code.size()>16)return false;
    for(const unsigned char c:code)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return false;
    return true;
}
std::string effective_door_code(const DoorDefinition& d) {
    auto code=d.code.empty()?"DOOR"+std::to_string(d.id):d.code;
    for(auto& c:code)if(c>='a'&&c<='z')c=static_cast<char>(c-'a'+'A');
    return code;
}
std::string suggest_door_code(const DoorCatalog& catalog,std::string_view name,DoorId id) {
    std::string stem;
    for(const unsigned char c:name){
        if(stem.size()==16)break;
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-')stem+=static_cast<char>(c>='a'&&c<='z'?c-'a'+'A':c);
        else if(c==' '&&!stem.empty()&&stem.back()!='_')stem+='_' ;
    }
    if(stem.empty())stem="DOOR"+std::to_string(id);
    const auto unused=[&](const std::string& code){return std::none_of(catalog.doors.begin(),catalog.doors.end(),[&](const auto& d){return d.id!=id&&effective_door_code(d)==code;});};
    if(unused(stem))return stem;
    for(unsigned n=1;n<=1000;++n){const auto suffix="_"+std::to_string(n);const auto code=stem.substr(0,16-suffix.size())+suffix;if(unused(code))return code;}
    return {};
}
bool door_has_command(const DoorDefinition& d) noexcept {return d.kind==DoorExecutionKind::telnet?!d.telnet.host.empty():d.kind==DoorExecutionKind::rlogin?!d.rlogin.host.empty():!d.program.command_line.empty();}
bool valid_door_definition(const DoorDefinition& d) noexcept {
    const auto& p=d.program;
    return d.id>=1 && d.id<=999 && valid_door_text(d.name,64) &&
        static_cast<unsigned>(d.security_mode)<=2 && d.security_level>=1 &&
        d.security_level<=999 && valid_door_requirements(d.execution) && valid_rlogin_settings(d.rlogin,true) &&
        (d.code.empty()||valid_door_code(d.code)) && static_cast<unsigned>(d.kind)<=2 && valid_telnet_target(d.telnet,true) &&
        static_cast<unsigned>(p.type)<=1 && static_cast<unsigned>(p.io)<=4 &&
        (p.type==DoorProgramType::dos?p.io!=DoorIo::socket:(p.io==DoorIo::standard||p.io==DoorIo::socket)) &&
        valid_door_text(p.startup_directory,1024,true) && valid_door_text(p.command_line,4096,true) &&
        valid_door_text(p.cleanup_command,4096,true) && static_cast<unsigned>(p.drop_file)<=14 &&
        (p.drop_file!=DoorDropFile::doordrop_ini || p.drop_location!=DoorDropLocation::startup || !d.allow_multiple) &&
        static_cast<unsigned>(p.drop_location)<=2 && static_cast<unsigned>(d.automatic_event)<=2;
}
const DoorDefinition* find_door(const DoorCatalog& c, DoorId id) noexcept {
    const auto i=std::find_if(c.doors.begin(),c.doors.end(),[id](const auto& d){return d.id==id;});
    return i==c.doors.end()?nullptr:&*i;
}
const DoorMenuDefinition* find_door_menu(const DoorCatalog& c, DoorMenuId id) noexcept {
    const auto i=std::find_if(c.menus.begin(),c.menus.end(),[id](const auto& m){return m.id==id;});
    return i==c.menus.end()?nullptr:&*i;
}
DoorValidation validate_door_catalog(const DoorCatalog& c) {
    DoorValidation v;
    const auto error=[&](const char* code,std::uint16_t id=0,std::uint8_t n=0){v.errors.push_back({code,id,n});};
    if (c.revision<1 || c.doors.size()>999 || c.menus.empty() ||
        c.menus.size()>1000 || c.entries.size()>99000) {error("catalog bounds");return v;}
    std::set<DoorId> doors;std::set<DoorMenuId> menus;std::set<std::string> codes;
    for(const auto& d:c.doors) if(!valid_door_definition(d) || !doors.insert(d.id).second) error("invalid or duplicate Door ID",d.id);
    for(const auto& d:c.doors)if(!codes.insert(effective_door_code(d)).second)error("duplicate Door code",d.id);
    for(const auto& m:c.menus) if(m.id>999 || !valid_door_text(m.name,64) || !valid_door_requirements(m.access) || !valid_door_requirements(m.execution) || !menus.insert(m.id).second) error("invalid or duplicate Menu ID",m.id);
    if(!menus.count(0))error("root menu missing");
    std::map<DoorMenuId,std::vector<DoorMenuId>> edges;
    std::set<std::pair<DoorMenuId,std::uint8_t>> placements;
    for(const auto& e:c.entries) {
        if(!menus.count(e.menu) || !e.number || e.number>99 || static_cast<unsigned>(e.kind)>1 ||
            !valid_door_text(e.title,64) || !valid_door_text(e.description,256,true) ||
            !placements.insert({e.menu,e.number}).second) {error("invalid or duplicate placement",e.menu,e.number);continue;}
        if(e.kind==DoorEntryKind::door) {if(!doors.count(e.target))error("missing Door target",e.menu,e.number);}
        else if(!e.target || !menus.count(e.target))error("missing submenu target",e.menu,e.number);
        else edges[e.menu].push_back(e.target);
    }
    // Check every component, including unattached menus. Longest-path memoization
    // handles shared children without rejecting a valid second parent.
    std::array<std::uint8_t,1000> state{};
    std::array<unsigned,1000> height{};
    const auto visit=[&](auto&& self,DoorMenuId id)->unsigned {
        if(state[id]==1){error("submenu cycle",id);return 4;}
        if(state[id]==2)return height[id];
        state[id]=1;unsigned h=1;
        for(const auto child:edges[id])h=std::max(h,1+self(self,child));
        state[id]=2;height[id]=h;
        if(h>3)error("submenu depth exceeds three displays",id);
        return h;
    };
    for(const auto id:menus)visit(visit,id);
    return v;
}
bool door_accessible(const DoorDefinition& d,DoorAuthority a) noexcept {
    // An empty saved slot is visible to the Sysop, never a runnable caller item.
    return d.enabled&&door_has_command(d)&&meets_door_requirements({d.security_mode,d.security_level,d.sysop_only},a);
}
bool valid_door_requirements(const DoorRequirements& r) noexcept {
    return static_cast<unsigned>(r.mode)<=2&&r.level>=1&&r.level<=999;
}
bool meets_door_requirements(const DoorRequirements& r,DoorAuthority a) noexcept {
    if(!valid_door_requirements(r)||a.security_level<1||a.security_level>999||a.sysop_threshold<1||a.sysop_threshold>999)return false;
    if(r.sysop_only)return a.security_level>=a.sysop_threshold;
    switch(r.mode){
    case DoorSecurityMode::minimum:return a.security_level>=r.level;
    case DoorSecurityMode::maximum:return a.security_level<=r.level;
    case DoorSecurityMode::exact:return a.security_level==r.level;
    }
    return false;
}
bool door_executable(const DoorDefinition& d,DoorAuthority a) noexcept {return door_accessible(d,a)&&meets_door_requirements(d.execution,a);}
std::vector<DoorMenuEntry> visible_door_entries(const DoorCatalog& c,DoorMenuId menu,DoorAuthority a) {
    std::vector<DoorMenuEntry> result;
    for(const auto& e:c.entries)if(e.menu==menu) {
        if(e.kind==DoorEntryKind::door){const auto* d=find_door(c,e.target);if(!d || d->event_only || !door_accessible(*d,a))continue;}
        else {const auto* m=find_door_menu(c,e.target);if(!m||!meets_door_requirements(m->access,a))continue;}
        result.push_back(e);
    }
    std::sort(result.begin(),result.end(),[](const auto& x,const auto& y){return x.number<y.number;});return result;
}
std::optional<DoorMenuEntry> select_door_entry(const DoorCatalog& displayed,const DoorCatalog& current,
    DoorMenuId menu,std::uint8_t number,DoorAuthority a) {
    if(displayed.revision!=current.revision)return std::nullopt;
    const auto entries=visible_door_entries(current,menu,a);
    const auto i=std::find_if(entries.begin(),entries.end(),[number](const auto& e){return e.number==number;});
    return i==entries.end()?std::nullopt:std::optional<DoorMenuEntry>(*i);
}
std::optional<DoorId> next_unused_door_id(const DoorCatalog& c) noexcept {
    for(DoorId id=1;id<=999;++id)if(!find_door(c,id))return id;
    return std::nullopt;
}
std::optional<DoorMenuId> next_unused_door_menu_id(const DoorCatalog& c) noexcept {
    for(DoorMenuId id=1;id<=999;++id)if(!find_door_menu(c,id))return id;
    return std::nullopt;
}
bool copy_door_entry(DoorCatalog& c,DoorMenuId source,std::uint8_t n,DoorMenuId destination,std::uint8_t number) {
    const auto i=std::find_if(c.entries.begin(),c.entries.end(),[&](const auto& e){return e.menu==source&&e.number==n;});
    if(i==c.entries.end())return false;
    auto candidate=c;auto e=*i;e.menu=destination;e.number=number;candidate.entries.push_back(std::move(e));
    if(!validate_door_catalog(candidate).valid())return false;
    c=std::move(candidate);return true;
}
bool duplicate_door_definition(DoorCatalog& c,DoorId source,DoorId destination) {
    const auto* d=find_door(c,source);if(!d || find_door(c,destination))return false;
    auto candidate=c;auto copy=*d;copy.id=destination;copy.enabled=false;copy.code=suggest_door_code(c,copy.name,destination);candidate.doors.push_back(std::move(copy));
    if(!validate_door_catalog(candidate).valid())return false;
    c=std::move(candidate);return true;
}
bool duplicate_door_menu(DoorCatalog& c,DoorMenuId source,DoorMenuId destination,std::string name) {
    if(!find_door_menu(c,source) || !destination || find_door_menu(c,destination))return false;
    auto candidate=c;auto menu=*find_door_menu(c,source);menu.id=destination;menu.name=std::move(name);candidate.menus.push_back(std::move(menu));
    for(const auto& e:c.entries)if(e.menu==source){auto copy=e;copy.menu=destination;candidate.entries.push_back(std::move(copy));}
    if(!validate_door_catalog(candidate).valid())return false;
    c=std::move(candidate);return true;
}
namespace {
struct Writer {
    std::vector<std::uint8_t> b;
    void number(std::uint64_t n){for(int i=0;i<8;++i)b.push_back(static_cast<std::uint8_t>(n>>(i*8)));}
    void bytes(const std::vector<std::uint8_t>& v){number(v.size());b.insert(b.end(),v.begin(),v.end());}
    void text(std::string_view s){number(s.size());b.insert(b.end(),s.begin(),s.end());}
};
struct Reader {
    const std::vector<std::uint8_t>& b;std::size_t p{};bool ok{true};
    std::uint64_t number(){if(b.size()-p<8){ok=false;return 0;}std::uint64_t n=0;for(int i=0;i<8;++i)n|=std::uint64_t(b[p++])<<(i*8);return n;}
    std::uint16_t small(std::uint16_t maximum=999){auto n=number();if(n>maximum)ok=false;return static_cast<std::uint16_t>(n);}
    bool flag(){return small(1)!=0;}
    std::vector<std::uint8_t> bytes(std::size_t maximum){auto n=number();if(!ok || n>maximum || n>b.size()-p){ok=false;return {};}
        std::vector<std::uint8_t> out(b.begin()+static_cast<std::ptrdiff_t>(p),b.begin()+static_cast<std::ptrdiff_t>(p+n));p+=static_cast<std::size_t>(n);return out;}
    std::string text(std::size_t maximum){auto v=bytes(maximum);return {v.begin(),v.end()};}
};
void put_requirements(Writer& w,const DoorRequirements& p){w.number(static_cast<unsigned>(p.mode));w.number(p.level);w.number(p.sysop_only);}
DoorRequirements get_requirements(Reader& r){return {static_cast<DoorSecurityMode>(r.small(2)),r.small(),r.flag()};}
bool needs_doordrop_version(const DoorDefinition& d) {
    return d.program.drop_file == DoorDropFile::doordrop_ini || d.program.doordrop_utf8;
}
void put(Writer& w,const DoorDefinition& d,std::uint64_t version){
    w.number(d.id);w.text(d.name);w.number(d.enabled);w.number(static_cast<unsigned>(d.security_mode));w.number(d.security_level);w.number(d.sysop_only);
    const auto& t=d.rlogin;w.text(t.host);w.number(t.port);w.number(static_cast<unsigned>(t.authentication));w.number(static_cast<unsigned>(t.server_identity));
    w.text(t.server_name);w.text(t.client_name);w.text(t.terminal_type);w.text(t.salt);w.bytes(t.protected_credential);w.bytes(t.protected_pepper);
    w.number(t.prompt_credential);w.number(t.connection_timeout_seconds);w.number(t.quiet);w.number(t.banner);w.number(t.pause);put_requirements(w,d.execution);
    w.text(effective_door_code(d));w.number(static_cast<unsigned>(d.kind));const auto& p=d.program;
    w.text(p.startup_directory);w.text(p.command_line);w.text(p.cleanup_command);w.number(static_cast<unsigned>(p.type));w.number(static_cast<unsigned>(p.io));
    w.number(p.command_shell);w.number(static_cast<unsigned>(p.drop_file));w.number(static_cast<unsigned>(p.drop_location));
    w.number(d.allow_multiple);w.number(static_cast<unsigned>(d.automatic_event));w.number(d.event_only);
    if (version >= 4) w.number(p.doordrop_utf8);
    if(version>=5){w.text(d.telnet.host);w.number(d.telnet.port);w.text(d.telnet.terminal_type);w.number(d.telnet.connection_timeout_seconds);}
}
DoorDefinition get(Reader& r,std::uint64_t version){
    DoorDefinition d;d.id=r.small();d.name=r.text(64);d.enabled=r.flag();d.security_mode=static_cast<DoorSecurityMode>(r.small(2));d.security_level=r.small();d.sysop_only=r.flag();
    auto& t=d.rlogin;t.host=r.text(253);t.port=r.small(65535);t.authentication=static_cast<RloginAuthentication>(r.small(2));t.server_identity=static_cast<RloginIdentity>(r.small(1));
    t.server_name=r.text(127);t.client_name=r.text(127);t.terminal_type=r.text(127);t.salt=r.text(128);t.protected_credential=r.bytes(4096);t.protected_pepper=r.bytes(4096);
    t.prompt_credential=r.flag();t.connection_timeout_seconds=r.small(120);t.quiet=r.flag();t.banner=r.flag();t.pause=r.flag();
    // Immutable v1 records retain their original visibility AND launch rule.
    d.execution=version==1?DoorRequirements{d.security_mode,d.security_level,d.sysop_only}:get_requirements(r);
    if(version>=3){d.code=r.text(16);if(!valid_door_code(d.code))r.ok=false;d.kind=static_cast<DoorExecutionKind>(r.small(version>=5?2:1));auto& p=d.program;
        p.startup_directory=r.text(1024);p.command_line=r.text(4096);p.cleanup_command=r.text(4096);p.type=static_cast<DoorProgramType>(r.small(1));p.io=static_cast<DoorIo>(r.small(4));
        p.command_shell=r.flag();p.drop_file=static_cast<DoorDropFile>(r.small(version>=4?14:13));p.drop_location=static_cast<DoorDropLocation>(r.small(2));
        d.allow_multiple=r.flag();d.automatic_event=static_cast<DoorAutomaticEvent>(r.small(2));d.event_only=r.flag();
        if (version >= 4) p.doordrop_utf8 = r.flag();
        if(version>=5){d.telnet.host=r.text(253);d.telnet.port=r.small(65535);d.telnet.terminal_type=r.text(127);d.telnet.connection_timeout_seconds=r.small(120);}
    }else d.code=effective_door_code(d);
    return d;
}
}
std::vector<std::uint8_t> encode_door_definition(const DoorDefinition& d){
    if(!valid_door_definition(d))return {};
    const auto version = d.kind==DoorExecutionKind::telnet||!d.telnet.host.empty()?5u:needs_doordrop_version(d)?4u:3u;
    Writer w;w.number(version);put(w,d,version);return w.b;
}
std::optional<DoorDefinition> decode_door_definition(const std::vector<std::uint8_t>& b){
    if(b.size()>32768)return std::nullopt;
    Reader r{b};const auto version=r.number();if(version<1||version>5)return std::nullopt;auto d=get(r,version);
    return r.ok&&r.p==b.size()&&valid_door_definition(d)?std::optional<DoorDefinition>(std::move(d)):std::nullopt;
}
std::vector<std::uint8_t> encode_door_catalog(const DoorCatalog& c){
    if(!validate_door_catalog(c).valid())return {};
    const auto version = std::any_of(c.doors.begin(),c.doors.end(),[](const auto& d){return d.kind==DoorExecutionKind::telnet||!d.telnet.host.empty();})?5u:std::any_of(c.doors.begin(),c.doors.end(),needs_doordrop_version)?4u:3u;
    Writer w;w.number(version);w.number(static_cast<std::uint64_t>(c.revision));w.number(c.doors.size());for(const auto& d:c.doors)put(w,d,version);
    w.number(c.menus.size());for(const auto& m:c.menus){w.number(m.id);w.text(m.name);put_requirements(w,m.access);put_requirements(w,m.execution);}
    w.number(c.entries.size());for(const auto& e:c.entries){w.number(e.menu);w.number(e.number);w.number(static_cast<unsigned>(e.kind));w.number(e.target);w.text(e.title);w.text(e.description);}return w.b;
}
std::optional<DoorCatalog> decode_door_catalog(const std::vector<std::uint8_t>& b){
    if(b.size()>48U*1024U*1024U)return std::nullopt;
    Reader r{b};const auto version=r.number();if(version<1||version>5)return std::nullopt;DoorCatalog c;c.menus.clear();auto rev=r.number();
    if(rev>static_cast<std::uint64_t>(INT64_MAX))return std::nullopt;
    c.revision=static_cast<std::int64_t>(rev);
    auto n=r.number();if(n>999)return std::nullopt;for(std::uint64_t i=0;i<n&&r.ok;++i)c.doors.push_back(get(r,version));
    n=r.number();if(n>1000)return std::nullopt;for(std::uint64_t i=0;i<n&&r.ok;++i){DoorMenuDefinition m;m.id=r.small();m.name=r.text(64);if(version>=2){m.access=get_requirements(r);m.execution=get_requirements(r);}c.menus.push_back(std::move(m));}
    n=r.number();if(n>99000)return std::nullopt;for(std::uint64_t i=0;i<n&&r.ok;++i){DoorMenuEntry e;e.menu=r.small();e.number=static_cast<std::uint8_t>(r.small(99));e.kind=static_cast<DoorEntryKind>(r.small(1));e.target=r.small();e.title=r.text(64);e.description=r.text(256);c.entries.push_back(std::move(e));}
    if(!r.ok||r.p!=b.size()||!validate_door_catalog(c).valid())return std::nullopt;
    return c;
}
} // namespace sfx::application
