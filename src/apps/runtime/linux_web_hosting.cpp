#include "linux_web_hosting.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <sodium.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <array>
#include <charconv>
#include <map>
#include <set>

namespace sfx::apps::runtime::web {
namespace {
namespace native=sfx::platform::linux_host;
using Json=nlohmann::json;
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
void keys(const Json& value,std::initializer_list<const char*> names){
    require(value.is_object()&&value.size()==names.size(),"hosting_fields_invalid");
    for(const auto* name:names)require(value.contains(name),"hosting_fields_invalid");
}
std::string unit_path(const std::filesystem::path& path){
    const auto text=path.string();require(path.is_absolute()&&path.lexically_normal()==path&&text.size()<=4096,"hosting_path_invalid");
    std::string out="\"";
    for(const unsigned char c:text){
        require(c>=32&&c!=127,"hosting_path_control_character");
        if(c=='%'||c=='\\'||c=='\"')out+=static_cast<char>(c=='%'?'%':'\\');
        out+=static_cast<char>(c);
    }
    return out+'"';
}
std::string working_directory(const std::filesystem::path& path){
    // WorkingDirectory is a single raw path, unlike the quoted ExecStart argv.
    // A trailing space is trimmed and a trailing backslash continues a unit
    // line, so refuse those roots rather than silently binding another path.
    (void)unit_path(path);const auto text=path.string();
    require(text.back()!=' '&&text.back()!='\\',"hosting_working_directory_invalid");
    std::string out;for(const char c:text){if(c=='%')out+='%';out+=c;}return out;
}
}
HostingSettings parse_hosting(const Json& value){
    keys(value,{"revision","start_bbs_at_boot"});
    require(value.at("revision").is_string()&&value.at("start_bbs_at_boot").is_boolean(),"hosting_value_invalid");
    const auto text=value.at("revision").get<std::string>();HostingSettings result;
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),result.revision);
    require(parsed.ec==std::errc{}&&parsed.ptr==text.data()+text.size()&&result.revision&&result.revision<UINT64_MAX&&std::to_string(result.revision)==text,"hosting_revision_invalid");
    result.start_bbs_at_boot=value.at("start_bbs_at_boot").get<bool>();return result;
}
Json hosting_json(const HostingSettings& value){return {{"revision",std::to_string(value.revision)},{"start_bbs_at_boot",value.start_bbs_at_boot}};}
HostingSettings read_hosting(const std::filesystem::path& root){
    const auto directory=native::open_owned_directory(root/"config");struct stat info{};
    if(fstatat(directory.get(),"hosting-linux.v1",&info,AT_SYMLINK_NOFOLLOW)!=0){require(errno==ENOENT,"hosting_unreadable");return {};}
    const auto bytes=native::read_private_file(directory.get(),"hosting-linux.v1",1024);std::map<int,std::set<std::string>> fields;
    return parse_hosting(Json::parse(bytes.begin(),bytes.end(),[&](int depth,Json::parse_event_t event,Json& value){
        require(depth<=2,"hosting_depth_invalid");if(event==Json::parse_event_t::object_start)fields[depth+1].clear();
        if(event==Json::parse_event_t::key)require(fields[depth].insert(value.get<std::string>()).second,"hosting_duplicate_field");
        return true;
    }));
}
void write_hosting(const std::filesystem::path& root,const HostingSettings& value,std::uint64_t expected){
    native::InstallationLock publication;require(publication.acquire(root/"management","hosting-linux.lock"),"hosting_busy");
    const auto checked=parse_hosting(hosting_json(value));
    require(expected<UINT64_MAX-1&&checked.revision==expected+1,"hosting_revision_invalid");
    require(read_hosting(root).revision==expected,"hosting_changed_elsewhere");
    const auto directory=native::open_owned_directory(root/"config");const auto bytes=hosting_json(checked).dump()+"\n";
    native::publish_private_file(directory.get(),"hosting-linux.v1",{bytes.begin(),bytes.end()},true);
}
HostingSettings hosting_update(const Json& request){
    keys(request,{"revision","values"});keys(request.at("values"),{"start_bbs_at_boot"});
    auto value=parse_hosting({{"revision",request.at("revision")},{"start_bbs_at_boot",request.at("values").at("start_bbs_at_boot")}});
    require(value.revision<UINT64_MAX-1,"hosting_revision_exhausted");++value.revision;return value;
}
Json hosting_page(const HostingSettings& value,bool service,const std::string& startup_detail){
    const auto fields=Json::array({{{"key","start_bbs_at_boot"},{"label","Start BBS when Linux starts"},{"type","checkbox"},{"value",value.start_bbs_at_boot},
        {"help","Used when the Linux service starts. Saving does not start or stop the BBS. Opening the desktop launcher never starts the BBS."}}});
    return {{"route","system.runtime_behavior"},{"title","Runtime Behavior"},{"category","System"},{"revision",std::to_string(value.revision)},
        {"fields",fields},{"active_fields",fields},{"can_save",true},{"service_invocation",service},
        {"activation",std::string(service?"Management was started in service mode. ":"Management was opened outside the Linux service. ")+startup_detail+
            " Management boot and logout survival require the installed Linux service. Changing this choice does not register or restart that service."}};
}
Json hosting_service_plan(const std::filesystem::path& executable,const std::filesystem::path& root,const std::filesystem::path& tzdata,
    std::uint32_t uid,std::uint32_t gid,bool low_ports){
    require(uid!=0&&gid!=0&&uid!=UINT32_MAX&&gid!=UINT32_MAX&&sodium_init()>=0,"hosting_identity_invalid");
    const auto program=unit_path(executable),board=unit_path(root),zones=unit_path(tzdata);
    const auto identity=std::to_string(uid)+":"+root.string();std::array<unsigned char,32> digest{};std::array<char,65> hex{};
    require(crypto_hash_sha256(digest.data(),reinterpret_cast<const unsigned char*>(identity.data()),identity.size())==0,"hosting_identity_hash_failed");
    sodium_bin2hex(hex.data(),hex.size(),digest.data(),digest.size());const auto name="sfx-"+std::string(hex.data(),24)+".service";
    // ':' disables environment expansion; '%' is doubled by unit_path. The
    // systemd executable-name parser rejects quote/backslash characters even
    // after unquoting. env -- passes the absolute SFX path as literal argv and
    // exec-replaces itself with no shell or privilege change. WorkingDirectory
    // expands specifiers only, so a literal '$' also remains unchanged there.
    const std::string capability=low_ports?"CAP_NET_BIND_SERVICE":"";
    const auto unit=std::string("[Unit]\nDescription=Spitfire-X BBS management\nAfter=network.target\n\n[Service]\nType=exec\nUser=")+std::to_string(uid)+
        "\nGroup="+std::to_string(gid)+"\nWorkingDirectory="+working_directory(root)+"\nExecStart=:/usr/bin/env -- "+program+" --root "+board+" --tzdata "+zones+" --port 0 --service\n"+
        "UMask=0077\nNoNewPrivileges=yes\nCapabilityBoundingSet="+capability+"\nAmbientCapabilities="+capability+
        "\nKillMode=mixed\nTimeoutStopSec=120\nSendSIGKILL=no\nRestart=no\n\n[Install]\nWantedBy=multi-user.target\n";
    return {{"version",1},{"unit_name",name},{"unit",unit},{"uid",uid},{"gid",gid},{"root",root.string()},{"executable",executable.string()},
        {"tzdata",tzdata.string()},{"low_ports",low_ports},{"installed",false},
        {"detail","Prepared only. Install this exact unit as administrator, then enable it. The BBS boot choice is separate and starts off. After moving the SFX folder or changing accounts, prepare and register the service again."}};
}
} // namespace sfx::apps::runtime::web
