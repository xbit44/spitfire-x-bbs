#include "linux_web_doors.hpp"
#include "../config_ui/door_command_line.hpp"
#include "sfx/application/rlogin.hpp"
#include <algorithm>
#include <charconv>
#include <climits>
namespace sfx::apps::runtime::web {
namespace {
namespace app=application;using Json=nlohmann::json;
void require(bool yes){if(!yes)throw std::runtime_error("door_request_invalid");}
void keys(const Json& value,std::initializer_list<const char*> names){require(value.is_object()&&value.size()==names.size());for(const auto* name:names)require(value.contains(name));}
unsigned number(const Json& value,unsigned low,unsigned high){require(value.is_number_integer()&&value>=Json(low)&&value<=Json(high));return value.get<unsigned>();}
bool flag(const Json& value){require(value.is_boolean());return value.get<bool>();}
std::string text(const Json& value,std::size_t maximum,bool empty=false){require(value.is_string());auto result=value.get<std::string>();require(app::valid_door_text(result,maximum,empty));return result;}
std::int64_t revision(const Json& value){require(value.is_string());const auto s=value.get<std::string>();std::int64_t result{};const auto parsed=std::from_chars(s.data(),s.data()+s.size(),result);require(parsed.ec==std::errc{}&&parsed.ptr==s.data()+s.size()&&result>0&&result<INT64_MAX&&std::to_string(result)==s);return result;}
app::Identifier128 operation(const Json& value){const auto s=text(value,32);require(s.size()==32);app::Identifier128 result;
    const auto hex=[](char c)->unsigned{if(c>='0'&&c<='9')return static_cast<unsigned>(c-'0');if(c>='a'&&c<='f')return static_cast<unsigned>(c-'a'+10);throw std::runtime_error("door_operation_invalid");};
    for(std::size_t i=0;i<result.bytes.size();++i)result.bytes[i]=static_cast<std::uint8_t>(hex(s[i*2])*16+hex(s[i*2+1]));
    require(result.valid());return result;}
Json requirements(const app::DoorRequirements& r){return {{"mode",static_cast<unsigned>(r.mode)},{"level",r.level},{"sysop_only",r.sysop_only}};}
app::DoorRequirements requirements(const Json& value){keys(value,{"mode","level","sysop_only"});return {static_cast<app::DoorSecurityMode>(number(value.at("mode"),0,2)),static_cast<std::uint16_t>(number(value.at("level"),1,999)),flag(value.at("sysop_only"))};}
Json placement(const app::DoorMenuEntry& e){return {{"menu",e.menu},{"number",e.number},{"kind",static_cast<unsigned>(e.kind)},{"target",e.target},{"title",e.title},{"description",e.description}};}
app::DoorMenuEntry placement(const Json& value){keys(value,{"menu","number","kind","target","title","description"});return {static_cast<app::DoorMenuId>(number(value.at("menu"),0,999)),static_cast<std::uint8_t>(number(value.at("number"),1,99)),static_cast<app::DoorEntryKind>(number(value.at("kind"),0,1)),static_cast<std::uint16_t>(number(value.at("target"),1,999)),text(value.at("title"),64),text(value.at("description"),256,true)};}
void wipe_secrets(Json& value)noexcept{try{if(!value.is_object()||!value.contains("entry")||!value["entry"].is_object())return;auto& e=value["entry"];for(const auto* name:{"password","pepper"})if(e.contains(name)&&e[name].is_string()){auto& s=e[name].get_ref<std::string&>();app::erase_rlogin_secret(s.data(),s.size());s.clear();}}catch(...) {}}
const app::DoorMenuEntry& existing_entry(const app::DoorCatalog& c,unsigned menu,unsigned n){const auto found=std::find_if(c.entries.begin(),c.entries.end(),[&](const auto& e){return e.menu==menu&&e.number==n;});require(found!=c.entries.end());return *found;}
}
Json door_catalog_page(const app::DoorCatalog& c,unsigned menu,std::string_view op){
    require(app::find_door_menu(c,static_cast<app::DoorMenuId>(menu))&&menu<=999);Json menus=Json::array(),doors=Json::array(),entries=Json::array();
    for(const auto& m:c.menus)menus.push_back({{"id",m.id},{"name",m.name},{"access",requirements(m.access)},{"execution",requirements(m.execution)}});
    for(const auto& d:c.doors)doors.push_back({{"id",d.id},{"name",d.name},{"code",app::effective_door_code(d)},{"enabled",d.enabled}});
    for(const auto& e:c.entries)if(e.menu==menu)entries.push_back(placement(e));
    std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.at("number")<b.at("number");});
    return {{"revision",std::to_string(c.revision)},{"operation",op},{"menu",menu},{"menus",menus},{"doors",doors},{"entries",entries}};
}
Json door_editor_page(const app::DoorCatalog& c,unsigned id,std::string_view op){
    app::DoorDefinition d;if(id){const auto* saved=app::find_door(c,static_cast<app::DoorId>(id));require(saved&&id<=999);d=*saved;}else{d.kind=app::DoorExecutionKind::local;d.enabled=true;d.security_level=10;d.execution.level=10;}
    const auto& p=d.program;const auto& r=d.rlogin;
    const auto command=d.kind==app::DoorExecutionKind::local?p.command_line:d.kind==app::DoorExecutionKind::telnet?sfx::config_ui::format_door_command(d.telnet):sfx::config_ui::format_door_command(r);
    Json entry={{"id",id},{"name",d.name},{"code",d.code},{"enabled",d.enabled},{"access",requirements(app::DoorRequirements{d.security_mode,d.security_level,d.sysop_only})},{"execution",requirements(d.execution)},
        {"command",command},{"startup_directory",p.startup_directory},{"cleanup_command",p.cleanup_command},{"program_type",static_cast<unsigned>(p.type)},{"io",static_cast<unsigned>(p.io)},{"command_shell",p.command_shell},{"drop_file",static_cast<unsigned>(p.drop_file)},{"drop_location",static_cast<unsigned>(p.drop_location)},{"doordrop_utf8",p.doordrop_utf8},
        {"allow_multiple",d.allow_multiple},{"automatic_event",static_cast<unsigned>(d.automatic_event)},{"event_only",d.event_only},{"authentication",static_cast<unsigned>(r.authentication)},{"caller_password",r.prompt_credential},{"salt",r.salt},{"banner",r.banner},{"password",""},{"pepper",""},{"clear_password",false},{"clear_pepper",false}};
    return {{"revision",std::to_string(c.revision)},{"operation",op},{"entry",entry},{"saved_password",!r.protected_credential.empty()},{"saved_pepper",!r.protected_pepper.empty()}};
}
app::DoorConfigurationChange door_catalog_update(Json& value,const app::DoorCatalog& catalog,const DoorSecretProtector& protect){
    struct Wipe{Json& v;~Wipe(){wipe_secrets(v);}}wipe{value};
    keys(value,{"revision","operation","action","entry"});app::DoorConfigurationChange change;change.expected_revision=revision(value.at("revision"));require(change.expected_revision==catalog.revision);change.operation=operation(value.at("operation"));const auto action=text(value.at("action"),32);const auto& e=value.at("entry");
    if(action=="door"){
        keys(e,{"id","name","code","enabled","access","execution","command","startup_directory","cleanup_command","program_type","io","command_shell","drop_file","drop_location","doordrop_utf8","allow_multiple","automatic_event","event_only","authentication","caller_password","salt","banner","password","pepper","clear_password","clear_pepper"});
        const auto id=number(e.at("id"),0,999);change.action=app::DoorConfigurationAction::save_door;change.create_only=id==0;auto& d=change.door;
        if(id){const auto* saved=app::find_door(catalog,static_cast<app::DoorId>(id));require(saved!=nullptr);d=*saved;}else{const auto next=app::next_unused_door_id(catalog);require(next.has_value());d.id=*next;}
        d.name=text(e.at("name"),64);d.code=text(e.at("code"),16,true);if(d.code.empty())d.code=app::suggest_door_code(catalog,d.name,d.id);d.code=app::effective_door_code(d);d.enabled=flag(e.at("enabled"));
        const auto access=requirements(e.at("access"));d.security_mode=access.mode;d.security_level=access.level;d.sysop_only=access.sysop_only;d.execution=requirements(e.at("execution"));
        auto& p=d.program;p.startup_directory=text(e.at("startup_directory"),4096,true);p.cleanup_command=text(e.at("cleanup_command"),4096,true);p.type=static_cast<app::DoorProgramType>(number(e.at("program_type"),0,1));p.io=static_cast<app::DoorIo>(number(e.at("io"),0,4));p.command_shell=flag(e.at("command_shell"));p.drop_file=static_cast<app::DoorDropFile>(number(e.at("drop_file"),0,14));p.drop_location=static_cast<app::DoorDropLocation>(number(e.at("drop_location"),0,2));p.doordrop_utf8=flag(e.at("doordrop_utf8"));
        d.allow_multiple=flag(e.at("allow_multiple"));d.automatic_event=static_cast<app::DoorAutomaticEvent>(number(e.at("automatic_event"),0,2));d.event_only=flag(e.at("event_only"));
        const auto command=text(e.at("command"),4096,true);const auto first=command.find_first_not_of(" \t");const auto trimmed=first==command.npos?std::string{}:command.substr(first);const auto builtin=[&](std::string_view name){return trimmed==name||(trimmed.size()>name.size()&&trimmed.compare(0,name.size(),name)==0&&(trimmed[name.size()]==' '||trimmed[name.size()]=='\t'));};
        // Remote passwords only use the board-key envelope. No ciphertext comes from the browser.
        require(e.at("password").is_string()&&e.at("pepper").is_string());const auto& password=e.at("password").get_ref<const std::string&>();const auto& pepper=e.at("pepper").get_ref<const std::string&>();require(app::valid_door_text(password,127,true)&&app::valid_door_text(pepper,127,true));
        const bool clear_password=flag(e.at("clear_password")),clear_pepper=flag(e.at("clear_pepper"));const auto auth=number(e.at("authentication"),0,2);const auto caller_password=flag(e.at("caller_password"));const auto salt=text(e.at("salt"),128,true);const bool banner=flag(e.at("banner"));
        if(builtin("telnet")){const auto parsed=sfx::config_ui::parse_telnet_command(trimmed);require(bool(parsed));d.kind=app::DoorExecutionKind::telnet;d.telnet=parsed.target;require(password.empty()&&pepper.empty()&&!clear_password&&!clear_pepper);}
        else if(builtin("rlogin")){
            const auto parsed=sfx::config_ui::parse_door_command(trimmed,d.rlogin);require(bool(parsed));d.kind=app::DoorExecutionKind::rlogin;d.rlogin=parsed.target;auto& r=d.rlogin;r.authentication=static_cast<app::RloginAuthentication>(auth);r.prompt_credential=caller_password;r.salt=salt;r.banner=banner;
            if(parsed.password!=sfx::config_ui::DoorCommandPassword::unchanged){r.authentication=parsed.password==sfx::config_ui::DoorCommandPassword::plain_prompt?app::RloginAuthentication::traditional:app::RloginAuthentication::synchronet_caller_hash;r.prompt_credential=true;}
            require(!r.prompt_credential||password.empty());require(r.authentication==app::RloginAuthentication::synchronet_caller_hash||pepper.empty());require(!clear_password||password.empty());require(!clear_pepper||pepper.empty());
            if(clear_password||r.prompt_credential)r.protected_credential.clear();
            if(clear_pepper||r.authentication!=app::RloginAuthentication::synchronet_caller_hash)r.protected_pepper.clear();
            const auto seal=[&](const auto& secret,auto& destination){if(secret.empty())return;require(bool(protect));const auto sealed=protect(secret);require(sealed.has_value()&&!sealed->empty());destination=*sealed;};seal(password,r.protected_credential);seal(pepper,r.protected_pepper);
        }else{d.kind=app::DoorExecutionKind::local;p.command_line=trimmed.empty()?std::string{}:command;require(password.empty()&&pepper.empty()&&!clear_password&&!clear_pepper);}
    }else if(action=="menu"){
        keys(e,{"id","name","access","execution"});const auto id=number(e.at("id"),0,1000);change.action=app::DoorConfigurationAction::save_menu;change.create_only=id==1000;
        if(change.create_only){const auto next=app::next_unused_door_menu_id(catalog);require(next.has_value());change.menu.id=*next;}else{require(app::find_door_menu(catalog,static_cast<app::DoorMenuId>(id))!=nullptr);change.menu.id=static_cast<app::DoorMenuId>(id);}
        change.menu.name=text(e.at("name"),64);change.menu.access=requirements(e.at("access"));change.menu.execution=requirements(e.at("execution"));
    }else if(action=="entry"){
        keys(e,{"placement","previous_number","source_menu","create","move","order"});change.action=app::DoorConfigurationAction::save_entry;change.entry=placement(e.at("placement"));change.previous_number=static_cast<std::uint8_t>(number(e.at("previous_number"),0,99));change.source_menu=static_cast<app::DoorMenuId>(number(e.at("source_menu"),0,999));change.create_only=flag(e.at("create"));change.move_entry=flag(e.at("move"));require(e.at("order").is_array()&&e.at("order").size()<=99);for(const auto& n:e.at("order"))change.order.push_back(static_cast<std::uint8_t>(number(n,1,99)));
        if(!change.create_only){const auto& prior=existing_entry(catalog,change.entry.menu,change.previous_number?change.previous_number:change.entry.number);require(prior.kind==change.entry.kind&&prior.target==change.entry.target);}
    }else if(action=="duplicate_door"||action=="duplicate_menu"){
        keys(e,{"id","name"});const auto id=number(e.at("id"),action=="duplicate_door"?1U:0U,999);const auto name=text(e.at("name"),64);
        if(action=="duplicate_door"){const auto* saved=app::find_door(catalog,static_cast<app::DoorId>(id));const auto next=app::next_unused_door_id(catalog);require(saved&&next);change.action=app::DoorConfigurationAction::save_door;change.create_only=true;change.door=*saved;change.door.id=*next;change.door.name=name;change.door.code=app::suggest_door_code(catalog,name,*next);change.door.enabled=false;}
        else{const auto next=app::next_unused_door_menu_id(catalog);require(next.has_value());change.action=app::DoorConfigurationAction::duplicate_menu;change.source_menu=static_cast<app::DoorMenuId>(id);change.menu={*next,name};}
    }else{
        keys(e,{"id","number"});const auto id=number(e.at("id"),0,999),n=number(e.at("number"),0,99);require(action=="remove_door"||action=="remove_menu"||action=="remove_entry");
        if(action=="remove_door"){require(n==0&&app::find_door(catalog,static_cast<app::DoorId>(id)));change.action=app::DoorConfigurationAction::remove_door;change.door.id=static_cast<app::DoorId>(id);}
        else if(action=="remove_menu"){require(n==0&&id&&app::find_door_menu(catalog,static_cast<app::DoorMenuId>(id)));change.action=app::DoorConfigurationAction::remove_menu;change.menu.id=static_cast<app::DoorMenuId>(id);}
        else{change.action=app::DoorConfigurationAction::remove_entry;change.entry=existing_entry(catalog,id,n);}
    }
    require(app::valid_door_change(change));return change;
}
#include "linux_web_door_bulletins.inc"
Json door_removal_preview(const app::DoorCatalog& c,std::string_view action,unsigned id,unsigned n){
    require(id<=999&&n<=99);std::size_t doors{},menus{},entries{};
    if(action=="remove_menu"){require(n==0);const auto next=app::without_door_category(c,static_cast<app::DoorMenuId>(id));require(next.has_value());doors=c.doors.size()-next->doors.size();menus=c.menus.size()-next->menus.size();entries=c.entries.size()-next->entries.size();}
    else if(action=="remove_door"){require(n==0&&app::find_door(c,static_cast<app::DoorId>(id)));doors=1;entries=static_cast<std::size_t>(std::count_if(c.entries.begin(),c.entries.end(),[&](const auto& e){return e.kind==app::DoorEntryKind::door&&e.target==id;}));}
    else{require(action=="remove_entry");(void)existing_entry(c,id,n);entries=1;}
    return {{"revision",std::to_string(c.revision)},{"doors",doors},{"categories",menus},{"entries",entries},{"program_files_deleted",false}};
}
}
