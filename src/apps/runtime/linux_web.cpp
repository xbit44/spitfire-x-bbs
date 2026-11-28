#include "linux_runtime_host.hpp"
#include "ansi_playback_file.hpp"
#include "sfx/application/botgate_rules.hpp"
#include "sfx/platform/linux/installation_layout.hpp"
#include "linux_startup_message.hpp"
#include "linux_installed_components.hpp"
#include "linux_web_operational.hpp"
#include "linux_web_messages.hpp"
#include "linux_web_file_catalog.hpp"
#include "linux_web_file_manager.hpp"
#include "linux_web_doors.hpp"
#include "linux_web_dos_setup.hpp"
#include "sfx/platform/linux/portable_key_store.hpp"
#include "linux_web_sessions.hpp"
#include "linux_web_access.hpp"
#include "linux_web_hosting.hpp"
#include "linux_browser_terminal.hpp"
#include "linux_web_caller_actions.hpp"
#include "linux_web_binkp.hpp"
#include "linux_web_help.hpp"
#include "linux_web_sounds.hpp"
#include "linux_system_monitor.hpp"
#include "../product_version.h"
#include "sfx/application/qwk.hpp"
#include "linux_external_client.hpp"
#include "linux_web_ftn_view.hpp"
#include "linux_web_ftn_source.hpp"
#include "linux_web_ftn_held.hpp"
#include "sfx/platform/linux/ftn_secrets.hpp"
#include "alpha_trusted_displays.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include "sfx/application/external_event_files.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <sodium.h>
#include <charconv>
#include <csignal>
#include <iostream>
#include <map>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

namespace {
namespace app=sfx::application;
namespace rt=sfx::apps::runtime;
namespace host=sfx::platform::linux_host;
using Json=nlohmann::json;
using Code=app::LocalControlResponseCode;
static_assert(std::atomic<bool>::is_always_lock_free,"signal shutdown requires lock-free atomic flag");
std::atomic<bool> interrupted{false};
void interrupt(int){interrupted.store(true,std::memory_order_relaxed);}
class Clock final:public app::IClock{
public:
    std::int64_t unix_seconds()const noexcept override{return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
    std::uint64_t monotonic_millis()const noexcept override{return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
};
std::string random_token(){std::array<unsigned char,32> bytes{};randombytes_buf(bytes.data(),bytes.size());std::array<char,65> hex{};sodium_bin2hex(hex.data(),hex.size(),bytes.data(),bytes.size());return hex.data();}
std::uint64_t number(std::string_view value){std::uint64_t result{};const auto read=std::from_chars(value.data(),value.data()+value.size(),result);
    if(value.empty()||read.ec!=std::errc{}||read.ptr!=value.data()+value.size())throw std::runtime_error("invalid_number");
    return result;}
Json parse(const httplib::Request& request,std::size_t maximum=16384){
    if(request.get_header_value("Content-Type")!="application/json"||request.body.size()>maximum)throw std::runtime_error("invalid_json_request");
    std::map<int,std::set<std::string>> keys;
    return Json::parse(request.body,[&](int depth,Json::parse_event_t event,Json& value){
        if(depth>16)throw std::runtime_error("json_depth_exceeded");
        if(event==Json::parse_event_t::object_start)keys[depth+1].clear();
        if(event==Json::parse_event_t::key&&!keys[depth].insert(value.get<std::string>()).second)throw std::runtime_error("duplicate_json_key");
        return true;
    });
}
void json_response(httplib::Response& response,const Json& value,int status=200){response.status=status;response.set_content(value.dump(),"application/json; charset=utf-8");}
void require_keys(const Json& value,std::initializer_list<const char*> expected){
    if(!value.is_object()||value.size()!=expected.size())throw std::runtime_error("request_fields_invalid");
    for(const auto* key:expected)if(!value.contains(key))throw std::runtime_error("request_fields_invalid");
}
std::string cookie(const httplib::Request& request,const std::string& name){
    std::string result;
    const auto raw=request.get_header_value("Cookie");std::size_t offset{};
    while(offset<raw.size()){auto end=raw.find(';',offset);if(end==raw.npos)end=raw.size();auto item=std::string_view(raw).substr(offset,end-offset);
        while(!item.empty()&&item.front()==' ')item.remove_prefix(1);
        if(item.substr(0,name.size()+1)==name+'='){if(!result.empty())return {};result=std::string(item.substr(name.size()+1));}
        offset=end+1;}
    return result;
}
Json status_json(rt::LinuxRuntimeHost& runtime){
    static std::mutex indicators_mutex;
    static app::AlphaModemIndicators indicators;
    const std::lock_guard<std::mutex> guard(indicators_mutex);
    const auto value=runtime.status();const auto controls=runtime.lifecycle_controls();Json nodes=Json::array();
    const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    indicators.observe(value,now);
    std::array<char,33> epoch{};sodium_bin2hex(epoch.data(),epoch.size(),value.runtime_epoch.bytes.data(),value.runtime_epoch.bytes.size());
    for(const auto& node:value.nodes){
        const auto lights=indicators.lights(node,now);
        nodes.push_back({{"number",node.number},{"caller",node.caller_handle},{"activity",node.activity},{"connected",node.caller_connected},{"enabled",node.enabled},{"available",node.available},{"ready",!controls.draining&&rt::web::browser_node_ready(node)},{"state",static_cast<unsigned>(node.state)},{"seconds",node.connected_seconds},{"transport",node.connection},
            {"generation",std::to_string(node.assignment_sequence)},{"door_launch",std::to_string(node.door_launch_sequence)},{"door",std::to_string(node.door_id)},{"account",std::to_string(node.account_id)},
            {"modem",{{"AA",lights.aa},{"CD",lights.cd},{"RD",lights.rd},{"SD",lights.sd},{"SC",lights.sc},{"MR",lights.mr}}}});
    }
    return {{"bbs_name",value.bbs_name},{"running",value.lifecycle.state==app::RuntimeState::online||value.lifecycle.state==app::RuntimeState::draining},{"state",static_cast<unsigned>(value.state)},
        {"epoch",epoch.data()},{"run",std::to_string(controls.run_sequence)},{"draining",controls.draining},{"drain_remaining_ms",controls.drain_remaining_milliseconds},{"lifecycle_detail",controls.diagnostic},
        {"callers",value.lifecycle.active_work.caller_sessions},{"configured_nodes",value.lifecycle.configured_nodes},{"nodes",nodes},
        {"telnet_port",value.telnet_active?value.telnet_port:0},{"ssh_port",value.ssh_active?value.ssh_port:0},{"setup_required",!value.bootstrap_complete},{"recovery_required",value.state==app::AlphaRuntimePresentationState::recovery_required},{"diagnostic",value.diagnostic_code}};
}
#include "linux_web_launcher.inc"
#include "linux_desktop_client_launcher.inc"
bool desktop_request(const httplib::Request& request){return request.path=="/api/desktop-node/reserve"||request.path=="/api/desktop-node/cancel";}
int run(const std::filesystem::path& root,const std::filesystem::path& tzdata,unsigned requested_port,bool service){
    if(geteuid()==0||requested_port>65535||sodium_init()<0||!host::LinuxBbsTimeZoneRuleProvider::initialize(tzdata))throw std::runtime_error("web_start_context_invalid");
    Clock clock;rt::LinuxRuntimeHost runtime(root,clock,[&]{return rt::verified_linux_starter_doors(root);});const auto opened=runtime.open();
    if(opened.code!=Code::ok&&opened.code!=Code::setup_required&&!(opened.code==Code::recovery_required&&runtime.management_owned()))throw std::runtime_error(opened.diagnostic_code);
    app::AnsiPlaybackSettings playback_settings([&] { return rt::read_ansi_playback_file(root); });
    const auto root_pin=host::open_owned_directory(root);if(mkdirat(root_pin.get(),"management",0700)!=0&&errno!=EEXIST)throw std::runtime_error("web_management_directory_unavailable");
    const auto assets=host::open_owned_resource_directory(root_pin.get(),"web");const auto management=host::open_owned_directory(root/"management");
    rt::web::BrowserSessions sessions(clock,[&](const std::string& token){const auto text=token+'\n';host::publish_private_file(management.get(),"launch-secret.v1",{text.begin(),text.end()},true);});
    // Non-secret browser draft namespace, unique to this management lifetime.
    // Reusing a port for another board cannot replay its predecessor's draft.
    auto draft_scope=random_token();std::shared_mutex request_gate;
    rt::web::LinuxDosSetup dos_setup(root);
    std::string startup_detail="The BBS was left stopped at management startup.";
    rt::web::RemoteBrowserSessions remote_sessions(clock);
    std::mutex web_access_mutex;bool lan_active{};std::string lan_detail="LAN access is off.",lan_fingerprint;
    const auto allowed_remote=[&](std::string_view source){std::lock_guard<std::mutex> lock(web_access_mutex);try{return rt::web::web_ip_allowed(rt::web::read_web_access(root),source);}catch(const std::exception&){return false;}};
    rt::web::BrowserTerminals terminals({clock,[&](std::uint16_t node){return runtime.open_browser_node(node);},[&](app::NodeAssignmentIdentity identity){(void)runtime.disconnect(identity);}});
    rt::web::WebCallerActions caller_actions(clock,[&](const auto& request){return runtime.chat_operator(request);},[&](const auto& request){return runtime.send_chat_notice(request);});
    rt::web::WebBinkpActions binkp_actions(clock,[&](const auto& request,auto epoch,auto revision){return runtime.ftn_runtime_checked(request,epoch,revision);});
    rt::web::WebFileActions file_actions(clock,[&](const auto& task){return runtime.start_file_management(task);},[&](auto job){return runtime.file_management_status(job);},[&](auto job){return runtime.cancel_file_management(job);});
    std::mutex monitor_mutex;rt::LinuxMonitorRates monitor_rates;rt::LinuxMonitorSnapshot monitor_snapshot;std::optional<std::uint64_t> monitor_sample_ms;
    rt::web::WebFtnDrafts ftn_drafts([&](const auto& request){return runtime.ftn_configuration(request);},
        [&](std::string_view network,std::string_view link,unsigned kind,std::string_view secret){return host::protect_ftn_secret(root.string(),network,link,static_cast<host::FtnCredentialKind>(kind),secret);},
        []{app::FtnTransferToken token{};randombytes_buf(token.data(),token.size());return token;},[&]{return clock.monotonic_millis();});
    const auto serve_endpoint=[&](httplib::Server& server,bool remote,int port,std::string identity){
    server.new_task_queue=[] {return new httplib::ThreadPool(4,4,16);};
    server.set_read_timeout(3);server.set_write_timeout(3);server.set_keep_alive_timeout(2);server.set_keep_alive_max_count(20);server.set_payload_max_length(131072);
    const auto host_name=identity.find(':')==std::string::npos?identity:"["+identity+"]";
    const auto authority=host_name+((remote&&port==443)?"":":"+std::to_string(port));
    const auto origin=std::string(remote?"https://":"http://")+authority;
    const auto cookie_name=std::string(remote?"__Host-sfx_lan_":"sfx_local_")+std::to_string(port);
    const auto authorized=[&](const httplib::Request& request,std::optional<std::string_view> csrf){
        return remote?remote_sessions.authorized(cookie(request,cookie_name),csrf,request.remote_addr,runtime.web_authentication_epoch()):sessions.authorized(cookie(request,cookie_name),csrf);
    };
    const auto before_restore=[&]{std::unique_lock<std::shared_mutex> exclusive(request_gate);dos_setup.reset();caller_actions.clear();file_actions.clear();ftn_drafts.clear();sessions.revoke_all();remote_sessions.revoke_all();draft_scope=random_token();};
    const auto guarded_handler=[&](httplib::Server::Handler handler){return [&,handler=std::move(handler)](const httplib::Request& request,httplib::Response& response){
        // Serialize DOS setup transitions with other web requests. A stopped
        // check and starting its worker must not race BBS Start or maintenance.
        const bool dos_transition=request.method=="POST"&&request.path.find("/api/config/doors/dos/")==0;
        std::shared_lock<std::shared_mutex> shared(request_gate,std::defer_lock);
        std::unique_lock<std::shared_mutex> exclusive(request_gate,std::defer_lock);
        if(dos_transition)exclusive.lock();else shared.lock();
        if(interrupted.load()){json_response(response,{{"detail","Management is closing. Open the launcher again when ready."}},503);return;}
        if(remote&&!allowed_remote(request.remote_addr)){json_response(response,{{"error","address_not_allowed"}},403);return;}
        if(request.path!="/api/launch"&&request.path!="/api/local-launch"&&request.path!="/api/login"&&!desktop_request(request)){
            std::optional<std::string_view> csrf;const auto supplied=request.get_header_value("X-SFX-CSRF");if(request.method=="POST")csrf=supplied;
            if(!authorized(request,csrf)){json_response(response,{{"error","open_with_launcher"},{"detail","Management access ended. Open the launcher again to review the restored board."}},401);return;}
        }
        const bool active=runtime.compaction_status().active||runtime.offline_status().active;
        if(dos_setup.active()&&request.method=="POST"&&request.path!="/api/config/doors/dos/cancel"&&request.path!="/api/config/doors/dos/start"&&request.path!="/api/login"&&request.path!="/api/local-launch"&&request.path!="/api/launch"){
            json_response(response,{{"detail","DOS setup is working. Wait for its result or cancel it before starting other work."}},409);return;
        }
        if(active&&request.path!="/api/maintenance"&&request.path!="/api/maintenance/confirm"&&request.path!="/api/maintenance/contents-select"&&request.path!="/api/maintenance/deletion-backup"&&request.path!="/api/maintenance/deletion-confirm"&&request.path!="/api/maintenance/file-deletion-confirm"&&request.path!="/api/maintenance/cancel"&&request.path!="/api/compaction"&&request.path!="/api/compaction/cancel"&&request.path!="/api/session"&&request.path!="/api/launch"&&request.path!="/api/local-launch"&&request.path!="/api/login"&&request.path!="/api/config/routes"){
            json_response(response,{{"detail","Maintenance is active. Read its current status; this request was not applied."}},409);return;
        }
        handler(request,response);
    };};
    const auto api_get=[&](const std::string& path,httplib::Server::Handler handler){server.Get(path,guarded_handler(std::move(handler)));};
    const auto api_post=[&](const std::string& path,httplib::Server::Handler handler){server.Post(path,guarded_handler(std::move(handler)));};
    std::mutex external_event_editor;
    const auto external_catalog = [&]() -> std::optional<app::ExternalEventCatalog> {
        const auto bytes = app::read_external_event_file(root, {}, app::ExternalEventFile::catalog);
        if (!bytes) return {};
        return bytes->empty() ? std::optional{app::ExternalEventCatalog{}} : app::parse_external_events(*bytes);
    };
    const auto external_event_change = [&](const httplib::Request& request, httplib::Response& response, bool erase) {
        const auto value = parse(request);
        if (erase) require_keys(value,{"revision","record"});
        else require_keys(value,{"revision","record","values"});
        const auto id = number(value.at("record").get<std::string>());
        const auto revision = number(value.at("revision").get<std::string>());
        app::ExternalEvent event;
        try { if (!erase) event = rt::web::external_event_update(value.at("values")); }
        catch (const std::exception&) {
            json_response(response,{{"detail","Enter a name, full paths, a valid schedule and a wait limit for exclusive events."}},400);
            return;
        }
        event.id = id;
        std::lock_guard<std::mutex> lock(external_event_editor);
        auto catalog = external_catalog();
        if (!runtime.configuration() || !catalog || catalog->revision != revision) {
            json_response(response,{{"detail","External Events changed elsewhere or could not be read. Reload the saved list."}},409);
            return;
        }
        const auto saved = id ? id : catalog->next_id;
        const bool changed = app::change_external_events(*catalog, std::move(event), erase, clock.unix_seconds());
        const bool written = changed && app::publish_external_event_file(root, {}, app::ExternalEventFile::catalog,
            app::serialize_external_events(*catalog));
        json_response(response,{{"ok",written},{"record",std::to_string(erase ? 0 : saved)},
            {"detail", !written ? "The change could not be verified. Reload the saved list." :
                erase ? "Event schedule deleted." : "External Event saved."}},written ? 200 : 409);
    };
    api_post("/api/config/external-events/delete",[&](const auto& request, auto& response) {
        external_event_change(request,response,true);
    });
    server.set_default_headers({{"Cache-Control","no-store"},{"Referrer-Policy","no-referrer"},{"X-Content-Type-Options","nosniff"},
        {"X-Frame-Options","DENY"},{"Content-Security-Policy","default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"}});
    server.set_pre_routing_handler([&](const auto& request,auto& response){
        if((remote?!allowed_remote(request.remote_addr):request.remote_addr!="127.0.0.1")||request.get_header_value("Host")!=authority){json_response(response,{{"error","address_not_allowed"}},403);return httplib::Server::HandlerResponse::Handled;}
        const auto supplied_origin=request.get_header_value("Origin");
        if((!supplied_origin.empty()&&supplied_origin!=origin)||(request.method=="POST"&&supplied_origin!=origin)){
            json_response(response,{{"error","origin_not_allowed"}},403);return httplib::Server::HandlerResponse::Handled;}
        if(request.path.rfind("/api/",0)==0&&request.path!="/api/launch"&&request.path!="/api/local-launch"&&request.path!="/api/login"&&!desktop_request(request)){
            std::optional<std::string_view> csrf;const auto supplied=request.get_header_value("X-SFX-CSRF");if(request.method=="POST")csrf=supplied;
            if(!authorized(request,csrf)){json_response(response,{{"error","open_with_launcher"}},401);return httplib::Server::HandlerResponse::Handled;}}
        const bool compact_active=runtime.compaction_status().active,offline_active=runtime.offline_status().active;
        if(request.path.rfind("/api/",0)==0&&(compact_active||offline_active)&&request.path!="/api/compaction"&&request.path!="/api/compaction/cancel"&&request.path!="/api/maintenance"&&request.path!="/api/maintenance/cancel"&&request.path!="/api/maintenance/confirm"&&request.path!="/api/maintenance/contents-select"&&request.path!="/api/maintenance/deletion-backup"&&request.path!="/api/maintenance/deletion-confirm"&&request.path!="/api/maintenance/file-deletion-confirm"&&request.path!="/api/session"&&request.path!="/api/launch"&&request.path!="/api/local-launch"&&request.path!="/api/login"&&request.path!="/api/config/routes"){
            if(request.path=="/api/status")json_response(response,{{"maintenance",true},{"maintenance_kind",offline_active?"offline":"compaction"}});
            else json_response(response,{{"detail","Maintenance is active. Check its status; this request was not applied."}},409);
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    server.set_exception_handler([](const auto&,auto& response,std::exception_ptr){json_response(response,{{"error","request_could_not_be_completed"}},400);});
    server.Get(R"(/(?:index\.html)?)",[&](const auto&,auto& response){const auto bytes=host::read_owned_resource(assets.get(),"index.html",262144);response.set_content(std::string(bytes.begin(),bytes.end()),"text/html; charset=utf-8");});
    for(const auto* name:{"app.css","app.js"}){const auto file=std::string(name);server.Get("/"+file,[&,file](const auto&,auto& response){const auto bytes=host::read_owned_resource(assets.get(),file,524288);response.set_content(std::string(bytes.begin(),bytes.end()),file=="app.js"?"text/javascript; charset=utf-8":"text/css; charset=utf-8");});}
    api_post("/api/local-launch",[&](const auto& request,auto& response){
        if(remote){json_response(response,{{"error","local_launcher_only"}},403);return;}
        auto value=parse(request,256);require_keys(value,{"owner"});auto owner=value.at("owner").template get<std::string>();
        const auto token=sessions.renew(owner);sodium_memzero(owner.data(),owner.size());
        auto& original=value.at("owner").template get_ref<std::string&>();sodium_memzero(original.data(),original.size());
        if(!token){json_response(response,{{"error","native_launcher_required"}},401);return;}
        json_response(response,{{"token",*token}});
    });
    api_post("/api/launch",[&](const auto& request,auto& response){if(remote){json_response(response,{{"error","local_launcher_only"}},403);return;}auto value=parse(request);require_keys(value,{"token"});auto token=value.at("token").template get<std::string>();
        const auto session=sessions.launch(token,cookie(request,cookie_name));sodium_memzero(token.data(),token.size());value.clear();
        if(!session){json_response(response,{{"error","launch_expired_or_invalid"}},401);return;}
        response.set_header("Set-Cookie",cookie_name+'='+session->first+"; HttpOnly; SameSite=Strict; Path=/");json_response(response,{{"csrf",session->second},{"draft_scope",draft_scope}});
    });
    api_get("/api/session",[&](const auto& request,auto& response){const auto csrf=authorized(request,{});if(!csrf){json_response(response,{{"error","open_with_launcher"}},401);return;}json_response(response,{{"csrf",*csrf},{"draft_scope",draft_scope}});});
    api_post("/api/login",[&](const auto& request,auto& response){
        if(!remote){json_response(response,{{"detail","Use the local launcher on this computer."}},403);return;}
        auto value=parse(request,1024);require_keys(value,{"password"});auto& password=value.at("password").template get_ref<std::string&>();
        const auto checked=runtime.authenticate_web(password,request.remote_addr);
        if(!checked.verified){json_response(response,{{"detail",checked.retry_seconds?"Login is temporarily delayed. Wait before trying again.":"The System Password was not accepted, or the BBS needs local attention."},{"retry_seconds",checked.retry_seconds}},checked.retry_seconds?429:401);return;}
        const auto session=remote_sessions.establish(request.remote_addr,checked.epoch);
        if(!session){json_response(response,{{"detail","Too many browser sessions are open. End an older session or use local Web Access recovery."}},429);return;}
        response.set_header("Set-Cookie",cookie_name+'='+session->first+"; HttpOnly; Secure; SameSite=Strict; Path=/");json_response(response,{{"csrf",session->second},{"draft_scope",draft_scope}});
    });
#include "linux_web_access_http.inc"
#include "linux_web_setup_http.inc"
#include "linux_browser_terminal_http.inc"
#include "linux_web_controls_http.inc"
#include "linux_web_botgate_http.inc"
#include "linux_web_caller_actions_http.inc"
#include "linux_web_observations_http.inc"
#include "linux_web_binkp_http.inc"
#include "linux_web_file_manager_http.inc"
#include "linux_desktop_client_http.inc"
#include "linux_web_ftn_http.inc"
#include "linux_web_compaction.inc"
#include "linux_web_maintenance.inc"
    api_get("/api/status",[&](const auto&,auto& response){json_response(response,status_json(runtime));});
    // Exclusive with normal requests: a racing Start or maintenance request
    // cannot slip between the stopped-board check and closing management.
    server.Post("/api/management/close",[&](const auto& request,auto& response){
        std::unique_lock<std::shared_mutex> gate(request_gate);
        const auto csrf=request.get_header_value("X-SFX-CSRF");
        if(!authorized(request,csrf)){json_response(response,{{"detail","Open the local launcher again before closing management."}},401);return;}
        if(remote){json_response(response,{{"detail","Close management from the local Spitfire-X launcher."}},403);return;}
        const auto value=parse(request,128);require_keys(value,{});
        if(service){const auto plan=rt::web::hosting_service_plan(std::filesystem::read_symlink("/proc/self/exe"),root,tzdata,getuid(),getgid(),false);
            json_response(response,{{"detail","This host runs as a Linux service. Stop the BBS and wait for work to finish, then have an administrator run: sudo systemctl stop "+plan.at("unit_name").template get<std::string>()+". Nothing was stopped."}},409);return;}
        if(dos_setup.active()||runtime.management_work_active()){json_response(response,{{"detail","Stop the BBS and wait for DOS setup, file work, maintenance and antivirus updates to finish, then try again. Nothing was stopped."}},409);return;}
        json_response(response,{{"ok",true},{"detail","Closing stopped management for this installation."}});interrupted=true;
    });
#include "linux_web_help_http.inc"
#include "linux_web_monitor_http.inc"
#include "linux_web_sounds_http.inc"
    api_get("/api/config/time-zones",[&](const auto&,auto& response){json_response(response,host::LinuxBbsTimeZoneRuleProvider::available_zones());});
    api_get("/api/config/routes",[&](const auto&,auto& response){
        auto routes=rt::web::configuration_routes();
        auto security=std::find_if(routes.begin(),routes.end(),[](const auto& item){return item.at("id")=="security.clamav";});
        if(security!=routes.end())++security;
        routes.insert(security,Json{{"id","security.web_access"},{"title","Web Access"},{"category","Security"},{"available",true}});
        auto system=routes.end();for(auto it=routes.begin();it!=routes.end();++it)if(it->at("category")=="System")system=std::next(it);
        routes.insert(system,Json{{"id","system.event_sounds"},{"title","Event Sounds"},{"category","System"},{"available",true}});
        json_response(response,routes);
    });
    api_post("/api/config/menu-preview",[&](const auto& request,auto& response){
        const auto current=runtime.configuration();if(!current){json_response(response,{{"detail","Board settings could not be read."}},409);return;}
        auto reader=rt::open_native_display_reader(root/"display");
        try{auto preview=rt::web::menu_preview(parse(request),*current,reader.get());preview["folder"]=(root/"display").string();json_response(response,preview);}
        catch(const std::exception&){json_response(response,{{"detail","Choose a menu, security level from 1 to 999 and distinguishable colors. Preview does not save settings."}},400);}
    });
    api_get("/api/config/qwk",[&](const auto&,auto& response){const auto current=runtime.qwk_policy_snapshot();if(current.code!=app::LocalAccountResultCode::ok){json_response(response,{{"detail","QWK settings could not be read. Try again after checking the BBS status."}},409);return;}
        json_response(response,{{"revision",std::to_string(current.policy.revision)},{"operation",random_token().substr(0,32)},{"board_id",current.policy.board_id},{"maximum_messages",current.policy.maximum_messages}});});
    api_post("/api/config/qwk",[&](const auto& request,auto& response){
        app::QwkPolicyChange change;try{change=rt::web::qwk_update(parse(request));}catch(const std::exception&){json_response(response,{{"detail","Use a QWK ID of 1-8 safe DOS characters starting with a letter, and a message count from 1 to 65,535."},{"unresolved",false}},400);return;}
        const auto result=runtime.change_qwk_policy(change);const bool ok=result.code==app::LocalAccountResultCode::ok;
        const bool unresolved=result.code==app::LocalAccountResultCode::unavailable||result.code==app::LocalAccountResultCode::storage_failure;
        json_response(response,{{"ok",ok},{"unresolved",unresolved},{"replayed",result.policy.replayed},{"detail",ok?(result.policy.replayed?"QWK settings confirmed. No duplicate change was made.":"QWK settings saved."):unresolved?"The QWK save outcome is uncertain. Use Reconcile QWK before editing again.":"QWK settings were refused or changed elsewhere. Your draft has been kept; reload before retrying."}},ok?200:409);});
    api_get("/api/config/statistics",[&](const auto&,auto& response){const auto current=runtime.chat_configuration();
        if(current.code!=app::ChatResult::ok||!current.configuration){json_response(response,{{"detail","Statistics settings could not be read."}},409);return;}
        json_response(response,rt::web::statistics_page(*current.configuration));});
    api_post("/api/config/statistics",[&](const auto& request,auto& response){const auto current=runtime.chat_configuration();
        if(current.code!=app::ChatResult::ok||!current.configuration){json_response(response,{{"detail","Statistics settings could not be read. Your draft has been kept."}},409);return;}
        const auto result=runtime.change_chat_configuration(rt::web::statistics_update(parse(request),*current.configuration));const bool ok=result.code==app::ChatResult::ok;
        json_response(response,{{"ok",ok},{"detail",ok?"Statistics setting saved.":"Statistics or Chat settings changed elsewhere, or the save was refused. Reload before trying again."}},ok?200:409);});
    const auto change_room=[&](const httplib::Request& request,httplib::Response& response,bool remove){
        const auto current=runtime.chat_configuration();if(current.code!=app::ChatResult::ok||!current.configuration){json_response(response,{{"detail","Chat settings could not be read."}},409);return;}
        app::ChatConfiguration next;Json value;try{value=parse(request);next=rt::web::chat_room_update(value,*current.configuration,remove);}catch(const std::exception& error){json_response(response,{{"detail",std::string_view(error.what())=="room_not_found"?"This room was removed. Review your draft, then discard it to select another room. Saving cannot recreate it.":"Check the room name, selection and security level (1 to 999). The entry room must remain enabled and cannot be deleted."}},400);return;}
        const auto result=runtime.change_chat_configuration(next);const bool ok=result.code==app::ChatResult::ok&&result.configuration;
        const auto selected=remove?next.entry:value.at("record")==0?next.rooms.back().id:value.at("record").template get<std::uint16_t>();
        json_response(response,{{"ok",ok},{"record",selected},{"detail",ok?(remove?"Chat room deleted.":"Chat room saved."):result.code==app::ChatResult::busy?"This room is in use. Try again when it is empty.":"Chat settings changed elsewhere or the save could not be confirmed. Reload and review before trying again."}},ok?200:409);
    };
    api_post("/api/config/chat/delete",[&](const auto& request,auto& response){change_room(request,response,true);});
    const auto change_command=[&](const httplib::Request& request,httplib::Response& response,bool remove){
        const auto current=runtime.chat_configuration();const auto catalog=runtime.complete_door_catalog();
        if(current.code!=app::ChatResult::ok||!current.configuration||!catalog){json_response(response,{{"detail","Door Commands could not be read. Your draft has been kept."}},409);return;}
        app::ChatConfiguration next;Json value;try{value=parse(request);next=rt::web::door_command_update(value,*current.configuration,*catalog,remove);}
        catch(const std::exception& error){json_response(response,{{"detail",std::string_view(error.what())=="command_not_found"?"This command was removed. Discard your draft to select another command. Saving cannot recreate it.":"Choose a registered Door, an unused uppercase letter, a menu name and security level from 1 to 999. Built-in menu keys cannot be replaced."}},400);return;}
        const auto result=runtime.change_chat_configuration(next);const bool ok=result.code==app::ChatResult::ok&&result.configuration;
        const auto selected=remove?0U:value.at("values").at("menu").template get<unsigned>()*26U+static_cast<unsigned>(value.at("values").at("key").template get<std::string>()[0]-'A')+1U;
        json_response(response,{{"ok",ok},{"record",selected},{"detail",ok?(remove?"Door command removed. The registered Door and its files remain.":"Door command saved."):"Chat, Statistics or Door settings changed elsewhere, or saving was refused. Reload and review before trying again."}},ok?200:409);
    };
    api_post("/api/config/door-commands/delete",[&](const auto& request,auto& response){change_command(request,response,true);});
#include "linux_web_message_utilities.inc"
#include "linux_web_messages_http.inc"
#include "linux_web_file_catalog_http.inc"
#include "linux_web_doors_http.inc"
#include "linux_web_dos_setup_http.inc"
#include "linux_web_door_bulletins_http.inc"
#include "linux_web_accounts_http.inc"
    api_post("/api/config/jobs/run",[&](const auto& request,auto& response){
        const auto value=parse(request);require_keys(value,{"record"});const auto& selected=value.at("record");
        if(!selected.is_number_integer()||selected<Json(1)||selected>Json(3))throw std::runtime_error("job_invalid");
        const auto id=selected.template get<unsigned>();
        const auto result=id==1?runtime.run_scheduler_now({}):runtime.run_message_maintenance_now(id==2?app::MessageMaintenanceKind::retention:app::MessageMaintenanceKind::integrity);
        const bool ok=result.code==Code::ok;json_response(response,{{"ok",ok},{"diagnostic",result.diagnostic_code},{"detail",ok?"Job accepted. Refresh to check its actual result; acceptance does not mean completion.":"Run was not confirmed. Refresh and check job history before trying again. No automatic retry will occur."}},ok?202:409);
    });
    api_post("/api/config/throttle/clear",[&](const auto& request,auto& response){
        app::LocalThrottleClearRequest change;try{change=rt::web::throttle_update(parse(request));}
        catch(const std::exception&){json_response(response,{{"unresolved",false},{"detail","Choose one canonical IP address or positive user number. Leave the entry blank for a shared delay."}},400);return;}
        const auto result=runtime.clear_throttle(change);const bool ok=result.code==Code::ok;
        json_response(response,{{"ok",ok},{"unresolved",!ok},{"detail",ok?"Selected login delay cleared and logged. Other protection is unchanged. A short wait already in progress may finish normally.":"Not confirmed. Check the caller and logs before trying again; the action will not be repeated automatically."}},ok?200:409);
    });
    const auto security_change=[&](const httplib::Request& request,httplib::Response& response,bool remove){
        const auto current=runtime.complete_security_snapshot();app::LocalSecurityChange change;
        try{change=remove?rt::web::security_level_removal(parse(request),current):rt::web::security_level_update(parse(request),current);}
        catch(const std::exception& error){const bool stale=std::string_view(error.what())=="security_revision_changed";
            json_response(response,{{"committed",false},{"detail",stale?"Security policy changed. Reload and review your edits before saving.":"Check the selected level, printable label/description, whole minutes and active defaults. Removed numbers cannot be reused."}},stale?409:400);return;}
        const auto result=runtime.change_security(change);const bool ok=result.code==app::LocalAccountResultCode::ok;
        const auto selected=remove?change.replacement:change.action==app::LocalSecurityAction::set_defaults?1000:change.level;
        json_response(response,{{"ok",ok},{"record",selected},{"committed",result.committed},{"detail",ok?"Security policy saved and audited. Running callers use the new access rules.":result.committed?"The database change was saved, but publishing the active policy needs recovery. Do not repeat this change. Close and reopen management to recover the saved policy.":result.code==app::LocalAccountResultCode::system_sysop_protected?"This would exclude the designated System Sysop. Assign another eligible Sysop first. Nothing was saved.":"The policy or affected callers changed, or protection of the last Sysop refused this save. Reload and review."}},ok?200:409);
    };
    api_get("/api/config/security/removal",[&](const auto& request,auto& response){
        const auto text=request.get_param_value("record"),position=request.has_param("offset")?request.get_param_value("offset"):"0";const auto level=number(text),offset=number(position);
        if(level<1||level>999||std::to_string(level)!=text||request.get_param_value_count("record")!=1||offset>1000000||std::to_string(offset)!=position||request.get_param_value_count("offset")>1)throw std::runtime_error("security_review_invalid");
        const auto preview=runtime.security_removal_preview(static_cast<std::uint16_t>(level),static_cast<std::uint32_t>(offset));
        if(preview.code!=app::LocalAccountResultCode::ok){json_response(response,{{"detail","The affected callers could not be read. Reload Security Levels and review again."}},409);return;}
        Json accounts=Json::array(),replacements=Json::array();for(const auto& account:preview.accounts)accounts.push_back({{"id",std::to_string(account.first)},{"name",account.second}});
        for(const auto& value:preview.security.levels)if(value.level!=level&&value.state==app::SecurityLevelState::active)replacements.push_back({{"id",value.level},{"name",std::to_string(value.level)+"  "+value.label}});
        json_response(response,{{"record",level},{"revision",std::to_string(preview.security.policy.revision)},{"affected_hash",preview.affected_hash},{"total",preview.total},{"offset",offset},{"accounts",accounts},{"replacements",replacements},{"new_user_level",preview.security.policy.new_user_level},{"sysop_threshold",preview.security.policy.sysop_threshold}});
    });
    api_post("/api/config/security/remove",[&](const auto& request,auto& response){security_change(request,response,true);});
    api_get(R"(/api/config/page/([a-z_]+\.[a-z_]+))",[&](const auto& request,auto& response){
        if(request.matches[1].str()=="connections.external_client"){
            try{json_response(response,rt::web::desktop_client_page(rt::web::read_desktop_client(root),!remote,root,std::filesystem::read_symlink("/proc/self/exe")));}
            catch(const std::exception&){json_response(response,{{"detail","The desktop client profile could not be read. Check Config/desktop-client-linux.v1 and its permissions."}},409);}return;}
        if(request.matches[1].str()=="system.runtime_behavior"){
            try{json_response(response,rt::web::hosting_page(rt::web::read_hosting(root),service,startup_detail));}
            catch(const std::exception&){json_response(response,{{"detail","The Linux boot setting could not be read. Check Config/hosting-linux.v1 and its permissions. No BBS start was requested."}},409);}return;}
        const auto special=request.matches[1].str();if(special=="tools.bbs_health"||special=="tools.backup_restore"){
            json_response(response,{{"route",special},{"title",special=="tools.bbs_health"?"BBS Health Check":"Backup & Restore"},{"category","Tools & Maintenance"},{"revision","0"},{"fields",Json::array()},{"active_fields",Json::array()},{"can_save",false},{"activation","Manual stopped-board tools. Review actual results; nothing runs merely by opening this page."}});return;}
        if(request.matches[1].str()=="security.throttle_recovery"){json_response(response,rt::web::throttle_page());return;}
        if(request.matches[1].str()=="system.system_password"){json_response(response,rt::web::system_password_page(runtime.account_snapshot({})));return;}
        if(request.matches[1].str()=="users.accounts"){read_accounts_page(request,response);return;}
        if(request.matches[1].str()=="users.security_levels"){
            std::optional<std::uint16_t> selected;if(request.has_param("record")){const auto text=request.get_param_value("record");const auto id=number(text);if(id>1000||std::to_string(id)!=text||request.get_param_value_count("record")!=1)throw std::runtime_error("security_record_invalid");selected=static_cast<std::uint16_t>(id);}
            const auto current=runtime.complete_security_snapshot();if(current.code!=app::LocalAccountResultCode::ok){json_response(response,{{"detail","Security policy could not be read."}},409);return;}
            json_response(response,rt::web::security_level_page(current,selected));return;}
        const auto route=request.matches[1].str();
        if(route=="files.groups_areas"){json_response(response,{{"route",route},{"title","Groups & Areas"},{"category","Files"},{"fields",Json::array()},{"active_fields",Json::array()},{"can_save",false},{"activation","Use the paged file catalog below. Folder changes never move files."}});return;}
        if(route=="messages.local_echoes"||route=="messages.clone_permissions"){
            json_response(response,{{"route",route},{"title",route=="messages.local_echoes"?"Groups & Local Echoes":"Clone Options"},{"category","Messages"},{"fields",Json::array()},{"active_fields",Json::array()},{"can_save",false},{"activation","Use the paged message catalog below. Deleting an area requires the separate stopped-board review."}});return;
        }
        if(route=="files.caller_exemptions"){
            app::LocalAccountQuery query;query.name_prefix=request.get_param_value("prefix");if(query.name_prefix.size()>30||query.name_prefix.find('\0')!=std::string::npos||request.get_param_value_count("prefix")>1)throw std::runtime_error("caller_filter_invalid");
            if(request.has_param("offset")){const auto text=request.get_param_value("offset");const auto offset=number(text);if(offset>1000000||std::to_string(offset)!=text||request.get_param_value_count("offset")!=1)throw std::runtime_error("caller_offset_invalid");query.offset=static_cast<std::uint32_t>(offset);}
            const auto accounts=runtime.account_snapshot(query);auto files=runtime.file_configuration_snapshot({});
            if(accounts.code!=app::LocalAccountResultCode::ok||files.code!=app::LocalAccountResultCode::ok){json_response(response,{{"detail","Caller list or file settings could not be read."}},409);return;}
            std::optional<app::AccountId> selected;std::optional<app::FileCallerExemption> exemption;bool available=false;std::string name;
            if(request.has_param("record")){const auto text=request.get_param_value("record");const auto id=number(text);if(!id||id>static_cast<std::uint64_t>(INT64_MAX)||std::to_string(id)!=text||request.get_param_value_count("record")!=1)throw std::runtime_error("caller_id_invalid");selected=static_cast<app::AccountId>(id);
                app::LocalAccountQuery exact;exact.account_id=*selected;const auto detail=runtime.account_snapshot(exact);
                if(detail.code==app::LocalAccountResultCode::ok&&detail.accounts.size()==1&&detail.accounts.front().account_id==*selected){const auto& account=detail.accounts.front();name=account.display_name.value_or("");available=account.state!=app::AccountState::tombstoned;}
                app::LocalFileConfigurationQuery selected_query;selected_query.part=app::FileConfigurationPart::caller_exemption;selected_query.account=*selected;const auto current=runtime.file_configuration_snapshot(selected_query);
                if(current.code==app::LocalAccountResultCode::ok&&current.exemption){files=current;exemption=current.exemption;}else available=false;
                if(!available)name="Unavailable caller (draft retained)";
            }
            auto page=rt::web::caller_exemption_page(files.policy,exemption,selected,name,available,random_token().substr(0,32));Json records=Json::array();
            for(const auto& account:accounts.accounts)records.push_back({{"id",std::to_string(account.account_id)},{"name",std::to_string(account.account_id)+"  "+account.display_name.value_or("")}});
            if(selected&&std::none_of(records.begin(),records.end(),[&](const auto& row){return row.at("id")==std::to_string(*selected);}))records.push_back({{"id",std::to_string(*selected)},{"name",std::to_string(*selected)+"  "+name}});
            page["records"]=records;page["prefix"]=query.name_prefix;page["offset"]=query.offset;page["has_more"]=accounts.has_more;json_response(response,page);return;}
        if(route=="tools.scheduled_jobs"){
            const auto catalog = external_catalog();
            const auto configuration = runtime.configuration();
            if (!catalog || !configuration) {
                json_response(response,{{"detail","The protected External Events list is unavailable."}},409);
                return;
            }
            const auto rules = host::LinuxBbsTimeZoneRuleProvider::create(configuration->saved.bbs_time_zone);
            if (!rules) {
                json_response(response,{{"detail","The BBS time zone is unavailable."}},409);
                return;
            }
            const app::BbsLocalTimeAdapters zone(rules->configuration(), *rules, clock);
            const auto selected = request.has_param("record") ?
                std::optional{number(request.get_param_value("record"))} : std::nullopt;
            json_response(response,rt::web::external_events_page(*catalog,selected,zone,clock.unix_seconds()));
            return;
        }
        if(route=="tools.backup_schedule"){
            unsigned selected=1;if(request.has_param("record")){const auto text=request.get_param_value("record");const auto id=number(text);if(id<1||id>3||std::to_string(id)!=text||request.get_param_value_count("record")!=1)throw std::runtime_error("job_invalid");selected=static_cast<unsigned>(id);}
            const auto snapshot=runtime.scheduled_job(rt::web::scheduled_job_id(selected));if(!snapshot.available){json_response(response,{{"detail","Job settings could not be read. Check BBS status."}},409);return;}
            std::optional<app::MessageMaintenanceStatus> maintenance;if(selected>1)maintenance=runtime.message_maintenance_status(selected==2?app::MessageMaintenanceKind::retention:app::MessageMaintenanceKind::integrity);
            auto page=rt::web::scheduled_job_page(snapshot,selected,maintenance?&*maintenance:nullptr);const auto config=runtime.configuration();if(config){const auto zone=host::LinuxBbsTimeZoneRuleProvider::create(config->active.bbs_time_zone);if(zone){page["time_zone"]=zone->iana_zone();const app::BbsLocalTimeAdapters local(zone->configuration(),*zone,clock);
                const auto format=[&](const Json& stamp){const auto time=stamp.template get<std::int64_t>();return time>0?local.format_utc_seconds(time).value_or("Unavailable"):std::string{};};
                auto& state=page["job_status"];state["next_local"]=format(state["next"]);state["active_started_local"]=format(state["active_started"]);state["last_local"]=format(state["last"]);for(auto& row:state["history"]){row["started_local"]=format(row["started"]);row["finished_local"]=format(row["finished"]);}}}json_response(response,page);return;}
        if(route=="files.count_rules"){
            const auto current=runtime.file_count_rules();if(current.code!=app::LocalAccountResultCode::ok){json_response(response,{{"detail","File-count rules or security levels could not be read."}},409);return;}
            std::optional<std::uint16_t> selected;if(request.has_param("record")){const auto text=request.get_param_value("record");const auto id=number(text);if(id<1||id>999||std::to_string(id)!=text||request.get_param_value_count("record")!=1)throw std::runtime_error("security_level_invalid");selected=static_cast<std::uint16_t>(id);}
            json_response(response,rt::web::file_count_rule_page(current.policy,current.rules,current.levels,selected,random_token().substr(0,32)));return;}
        if(route=="files.transfer_policy"||route=="security.clamav"){
            const auto current=runtime.file_configuration_snapshot({});if(current.code!=app::LocalAccountResultCode::ok){json_response(response,{{"detail","File settings could not be read."}},409);return;}
            json_response(response,(route=="security.clamav"?rt::web::clamav_policy_page(current.policy,random_token().substr(0,32)):rt::web::transfer_policy_page(current.policy,random_token().substr(0,32))));return;}
        if(route=="general.door_commands"){
            const auto current=runtime.chat_configuration();const auto catalog=runtime.complete_door_catalog();if(current.code!=app::ChatResult::ok||!current.configuration||!catalog){json_response(response,{{"detail","Door Commands could not be read."}},409);return;}
            std::optional<unsigned> selected;if(request.has_param("record")){const auto text=request.get_param_value("record");const auto id=number(text);if(id>234||std::to_string(id)!=text||request.get_param_value_count("record")!=1)throw std::runtime_error("command_identity_invalid");selected=static_cast<unsigned>(id);}
            json_response(response,rt::web::door_command_page(*current.configuration,*catalog,selected));return;}
        if(route=="chat.rooms"){
            const auto current=runtime.chat_configuration();if(current.code!=app::ChatResult::ok||!current.configuration){json_response(response,{{"detail","Chat settings could not be read."}},409);return;}
            std::optional<std::uint16_t> selected;if(request.has_param("record")){const auto text=request.get_param_value("record");const auto id=number(text);if(id>65535||std::to_string(id)!=text||request.get_param_value_count("record")!=1)throw std::runtime_error("room_id_invalid");selected=static_cast<std::uint16_t>(id);}
            json_response(response,rt::web::chat_room_page(*current.configuration,selected));return;}
        if(!rt::web::operational_route(route)&&route!="general.system_information"){json_response(response,{{"error","page_not_available"}},404);return;}
        const auto current=runtime.configuration();if(!current){json_response(response,{{"error","initial_setup_required"}},409);return;}
        if(route=="general.system_information"){const auto snapshot=runtime.classic_information_snapshot(0);if(snapshot.code!=app::LocalAccountResultCode::ok){json_response(response,{{"detail","The opening date could not be read."}},409);return;}
            json_response(response,rt::web::opening_date_page(snapshot,current->saved.bbs_time_zone,clock));return;}
        json_response(response,rt::web::operational_page(route,*current));});
    api_post(R"(/api/config/page/([a-z_]+\.[a-z_]+))",[&](const auto& request,auto& response){
        if(request.matches[1].str()=="connections.external_client"){
            rt::web::DesktopClientProfile profile;
            try{profile=rt::web::desktop_client_update(parse(request));if(profile.type)(void)rt::web::pin_desktop_client(profile);}
            catch(const std::exception&){json_response(response,{{"detail","Choose an installed executable and valid arguments. Custom needs {HOST} and {PORT} once each. None needs an empty path and arguments. The program must be executable and not writable by other accounts."},{"unresolved",false}},400);return;}
            try{rt::web::write_desktop_client(root,profile,profile.revision-1);json_response(response,{{"ok",true},{"detail",profile.type?"Desktop client saved. Run the local desktop command to use it; no program was started.":"Desktop client cleared. Installed program files remain."}});}
            catch(const std::exception& error){const auto reason=std::string_view(error.what());const bool refused=reason=="desktop_profile_busy"||reason=="desktop_profile_changed";
                json_response(response,{{"detail",refused?"The profile changed elsewhere or its launcher is using it. Nothing was saved. Your draft remains; read the saved profile before trying again.":"The client save was not confirmed. Discard Changes to read the saved profile before another save."},{"unresolved",!refused}},409);}return;}
        if(request.matches[1].str()=="system.runtime_behavior"){
            rt::web::HostingSettings value;
            try{value=rt::web::hosting_update(parse(request));}
            catch(const std::exception&){json_response(response,{{"detail","Choose whether the BBS should start with the Linux service. Reload if the saved revision is no longer available."},{"unresolved",false}},400);return;}
            try{rt::web::write_hosting(root,value,value.revision-1);json_response(response,{{"ok",true},{"detail","BBS boot choice saved. The running BBS and callers were not changed. The Linux service must be installed and enabled for startup at boot."}});}
            catch(const std::exception& error){const bool conflict=std::string_view(error.what())=="hosting_changed_elsewhere";json_response(response,{{"detail",conflict?"The boot choice changed elsewhere. Discard Changes to load the saved setting before trying again.":"The save was not confirmed. Discard Changes to read the saved boot choice before another save."},{"unresolved",!conflict}},409);}return;}
        if(request.matches[1].str()=="tools.scheduled_jobs"){external_event_change(request,response,false);return;}
        if(request.matches[1].str()=="tools.backup_schedule"){
            const auto value=parse(request);app::AlphaControlResponse result;
            try{if(value.at("record")==1){const auto change=rt::web::backup_schedule_update(value);result=runtime.configure_scheduler(change.request,change.retained);}else result=runtime.configure_message_schedule(rt::web::message_schedule_update(value));}
            catch(const std::exception&){json_response(response,{{"detail","Choose a built-in job, valid local time and folder, and a whole retention count from 1 to 365."}},400);return;}
            const bool ok=result.code==Code::ok;json_response(response,{{"ok",ok},{"record",value.at("record")},{"diagnostic",result.diagnostic_code},{"detail",ok?"Schedule saved. No job was started and no content was removed.":"Schedule save was not confirmed. Refresh and review the saved settings before another save. Retention changes require an idle, stopped BBS."}},ok?200:409);return;}
        if(request.matches[1].str()=="general.door_commands"){change_command(request,response,false);return;}
        if(request.matches[1].str()=="system.system_password"){
            app::LocalAccountRequest change;struct Wipe{app::LocalAccountRequest& value;~Wipe(){app::erase_local_account_secrets(value);}}wipe{change};
            try{change=rt::web::system_password_update(parse(request));}catch(const std::exception&){json_response(response,{{"committed",false},{"detail","Enter and confirm the same permanent System Password. Password fields must be entered again after submitting."}},400);return;}
            const auto result=runtime.administer_account(change);const bool ok=result.code==app::LocalAccountResultCode::ok;
            json_response(response,{{"ok",ok},{"committed",result.committed},{"sessions_revoked",result.sessions_revoked},{"revision",std::to_string(result.revision)},
                {"detail",ok?"Permanent System Password replaced. Active Sysop sessions were disconnected; ordinary callers remain.":result.committed?"The password was replaced, but session cleanup was not confirmed. Check Runtime status; do not repeat the replacement.":"Replacement refused. The password policy or saved revision may have changed. Refresh Status and enter the passwords again."}},ok?200:409);return;
        }
        if(request.matches[1].str()=="users.accounts"){account_change(request,response,{});return;}
        if(request.matches[1].str()=="users.security_levels"){security_change(request,response,false);return;}
        const auto route=request.matches[1].str();if(route=="files.count_rules"||route=="files.caller_exemptions"){
            app::FileConfigurationChange change;try{change=route=="files.caller_exemptions"?rt::web::caller_exemption_update(parse(request)):rt::web::file_count_rule_update(parse(request));}catch(const std::exception&){json_response(response,{{"detail",route=="files.caller_exemptions"?"Select a current caller and choose the file-count exemptions.":"Select an existing security level and enter explicit counts from 1 to 1,000,000 for enabled limits."},{"unresolved",false}},400);return;}
            const auto result=runtime.change_file_configuration(change);const bool ok=result.code==app::LocalAccountResultCode::ok,unresolved=result.code==app::LocalAccountResultCode::unavailable||result.code==app::LocalAccountResultCode::storage_failure;
            json_response(response,{{"ok",ok},{"record",route=="files.caller_exemptions"?Json(std::to_string(change.exemption.account)):Json(change.security.level)},{"replayed",result.result.replayed},{"unresolved",unresolved},{"detail",ok?(result.result.replayed?"File-count settings confirmed. No duplicate change was made.":"File-count settings saved."):unresolved?"The save outcome is uncertain. Reconcile the same save before editing again.":"The selected caller, level or file settings changed, or the save was refused. Reload and review before trying again."}},ok?200:409);return;}
        if(route=="files.transfer_policy"||route=="security.clamav"){
            const auto current=runtime.file_configuration_snapshot({});if(current.code!=app::LocalAccountResultCode::ok){json_response(response,{{"detail","File settings could not be read. Reconcile the same save before editing again."},{"unresolved",true}},409);return;}
            app::FileConfigurationChange change;try{change=rt::web::transfer_policy_update(parse(request),current.policy,route=="security.clamav");}catch(const std::exception& error){const bool conflict=std::string_view(error.what())=="file_policy_baseline_changed"||std::string_view(error.what())=="file_policy_revision_changed";
                json_response(response,{{"detail",conflict?"File settings changed elsewhere. Reload and review before trying again.":route=="security.clamav"?"Choose whether to scan caller uploads.":"Enter a maximum of 1 to 1,073,741,824 bytes and explicit values from 1 to 1,000,000 for every enabled count or ratio."},{"unresolved",false}},conflict?409:400);return;}
            const auto result=runtime.change_file_configuration(change);const bool ok=result.code==app::LocalAccountResultCode::ok;
            const bool unresolved=result.code==app::LocalAccountResultCode::unavailable||result.code==app::LocalAccountResultCode::storage_failure;
            json_response(response,{{"ok",ok},{"replayed",result.result.replayed},{"unresolved",unresolved},{"detail",ok?(result.result.replayed?"File settings confirmed. No duplicate change was made.":"File settings saved."):unresolved?"The save outcome is uncertain. Reconcile the same save before editing again.":"File settings changed elsewhere or the save was refused. Reload and review before trying again."}},ok?200:409);return;}
        if(route=="chat.rooms"){change_room(request,response,false);return;}
        if(route=="general.system_information"){
            app::ClassicInformationChange change;try{change=rt::web::opening_date_update(parse(request));}catch(const std::exception&){json_response(response,{{"detail","Enter a real date in YYYY-MM-DD format. Your draft has been kept."}},400);return;}
            const auto result=runtime.change_classic_information(change);const bool ok=result.code==app::LocalAccountResultCode::ok&&result.committed;
            json_response(response,{{"ok",ok},{"detail",ok?"Opening date saved.":"The opening date changed elsewhere or could not be saved. Reload before trying again."}},ok?200:409);return;}
        if(!rt::web::operational_route(route)){json_response(response,{{"error","page_not_available"}},404);return;}
        const auto current=runtime.configuration();if(!current){json_response(response,{{"error","initial_setup_required"}},409);return;}
        const auto result=runtime.save_configuration(rt::web::operational_update(route,parse(request),*current));
        json_response(response,{{"ok",result.code==Code::ok},{"diagnostic",result.diagnostic_code},{"detail",result.detail}},result.code==Code::ok?200:409);});
    api_get("/api/antivirus",[&](const auto&,auto& response){const auto value=runtime.clamav_status();const auto& av=value.status;Json databases=Json::array();
        for(const auto& database:av.databases)if(database)databases.push_back({{"version",database->version},{"signatures",database->signatures},{"published",database->published}});else databases.push_back(nullptr);
        json_response(response,{{"installed",av.installed},{"component_verified",av.verified},{"definitions_present",av.definitions_present},{"definitions_verified",av.definitions_verified},
            {"history_available",av.history_available},{"busy",av.busy},{"engine_version",av.version},{"diagnostic",av.diagnostic},{"last_result",av.last_result},
            {"last_attempt",av.last_attempt},{"last_success",av.last_success},{"last_change",av.last_change},{"change",static_cast<unsigned>(av.change)},
            {"manual",av.last_mode==host::LinuxClamAvUpdateMode::manual},{"databases",databases},{"active",value.active},{"queued",value.queued},{"paused",value.paused},
            {"operation",std::to_string(value.operation)},{"completed_operation",std::to_string(value.completed_operation)},{"elapsed_milliseconds",value.elapsed_milliseconds},
            {"cleanup_complete",value.last_update.cleanup_complete},{"update_succeeded",value.last_update.succeeded},{"update_cancelled",value.last_update.cancelled},{"update_diagnostic",value.last_update.diagnostic}});});
    api_get("/api/antivirus/log",[&](const auto&,auto& response){const auto log=runtime.clamav_update_log();json_response(response,{{"available",log.has_value()},{"text",log.value_or("")},{"detail",log?"Latest update output, at most 64 KiB. Older output is not kept here.":"No readable update log is available. It may be missing, busy or unsafe to read."}});});
    api_post("/api/antivirus/update",[&](const auto& request,auto& response){require_keys(parse(request),{});const auto operation=runtime.request_clamav_update();
        json_response(response,{{"ok",operation!=0},{"operation",std::to_string(operation)}},operation?202:409);});
    api_post("/api/antivirus/cancel",[&](const auto& request,auto& response){const auto value=parse(request);require_keys(value,{"operation"});const auto cancelled=runtime.cancel_clamav_update(number(value.at("operation").template get<std::string>()));
        json_response(response,{{"ok",cancelled}},cancelled?200:409);});
    api_post("/api/start",[&](const auto& request,auto& response){const auto value=parse(request);require_keys(value,{});const auto result=runtime.start_bbs();json_response(response,{{"ok",result.code==Code::ok},{"diagnostic",result.diagnostic_code},{"detail",result.code==Code::ok?"BBS started.":rt::linux_startup_message(result.diagnostic_code)}},result.code==Code::ok?200:409);});
    api_post("/api/stop",[&](const auto& request,auto& response){const auto value=parse(request);std::optional<std::uint64_t> ticket;
        if(value.contains("confirmation")){require_keys(value,{"confirmation"});ticket=number(value.at("confirmation").template get<std::string>());}else require_keys(value,{});
        const auto result=runtime.stop_bbs(ticket);json_response(response,{{"ok",result.response.code==Code::ok},{"callers",result.callers},{"confirmation",result.confirmation?std::to_string(result.confirmation):""},{"diagnostic",result.response.diagnostic_code}},result.confirmation||result.response.code==Code::ok?200:409);});
    api_post("/api/stop/cancel",[&](const auto& request,auto& response){require_keys(parse(request),{});runtime.cancel_stop();json_response(response,{{"ok",true}});});
    api_get("/api/config/board",[&](const auto&,auto& response){const auto value=runtime.configuration();if(!value){json_response(response,{{"error","initial_setup_required"}},409);return;}
        const auto zone=host::LinuxBbsTimeZoneRuleProvider::create(value->saved.bbs_time_zone);if(!zone)throw std::runtime_error("time_zone_unavailable");
        json_response(response,{{"revision",std::to_string(value->saved.configuration_revision)},{"bbs_name",value->saved.bbs_name},{"time_zone",zone->iana_zone()},{"activation",value->detail}});});
    api_post("/api/config/board",[&](const auto& request,auto& response){const auto value=parse(request);require_keys(value,{"revision","bbs_name","time_zone"});const auto current=runtime.configuration();
        if(!current){json_response(response,{{"error","initial_setup_required"}},409);return;}
        app::AlphaConfigurationUpdateRequest update;update.expected_revision=number(value.at("revision").template get<std::string>());update.configuration=current->saved;
        if(update.expected_revision==UINT64_MAX)throw std::runtime_error("configuration_revision_exhausted");
        update.configuration.configuration_revision=update.expected_revision+1;
        update.configuration.bbs_name=value.at("bbs_name").template get<std::string>();
        const auto zone=host::LinuxBbsTimeZoneRuleProvider::create(value.at("time_zone").template get<std::string>());
        if(!zone)throw std::runtime_error("time_zone_invalid");
        update.configuration.bbs_time_zone=host::LinuxBbsTimeZoneRuleProvider::windows_key_for_zone(zone->iana_zone())
            .value_or(zone->configuration().canonical_windows_registry_key);
        const auto result=runtime.save_configuration(update);json_response(response,{{"ok",result.code==Code::ok},{"diagnostic",result.diagnostic_code},{"detail",result.detail}},result.code==Code::ok?200:409);});
    return server.listen_after_bind();
    };
    httplib::Server server;
    const int port=requested_port?(server.bind_to_port("127.0.0.1",static_cast<int>(requested_port))?static_cast<int>(requested_port):-1):server.bind_to_any_port("127.0.0.1");
    if(port<=0)throw std::runtime_error("web_loopback_bind_failed");
    if(service){
        try{const auto policy=rt::web::read_hosting(root);if(policy.start_bbs_at_boot){const auto started=runtime.start_bbs();
            startup_detail=started.code==Code::ok?"The BBS started with this service invocation.":"The requested BBS start did not complete. Check Control Panel status and logs, then use Start when ready.";}}
        catch(const std::exception&){startup_detail="The saved boot choice could not be read; the BBS was left stopped. Check Config/hosting-linux.v1 and its permissions.";}
    }
    const auto launcher_record=Json{{"version",1},{"port",port},{"owner",sessions.owner_secret()}}.dump()+"\n";
    host::publish_private_file(management.get(),"web-launcher.v1",{launcher_record.begin(),launcher_record.end()},true);
#include "linux_web_https_lifecycle.inc"
    std::cout<<"WEB_READY "<<port<<(runtime.status().lifecycle.state==app::RuntimeState::online?" RUNNING":" STOPPED")<<" LOOPBACK_AND_OPTIONAL_HTTPS\n"<<std::flush;
    bool served=false;
    try{served=serve_endpoint(server,false,port,"127.0.0.1");}catch(...){interrupted=true;watcher.join();(void)runtime.shutdown();throw;}
    interrupted=true;watcher.join();terminals.close_all();const bool closed=runtime.shutdown().code==Code::ok;
    std::cout<<(closed?"WEB_STOPPED\n":"WEB_RECOVERY_REQUIRED\n")<<std::flush;return served&&closed?0:2;
}
}
int main(int argc,char** argv){try{
    if(argc==4&&std::string_view(argv[1])=="--stop-management"&&std::string_view(argv[2])=="--root")return stop_management(argv[3]);
    if(argc==3&&(std::string_view(argv[1])=="--ftn-list-url"||std::string_view(argv[1])=="--ftn-list-file"))return rt::web::run_ftn_echo_source_worker(argv[2],std::string_view(argv[1])=="--ftn-list-url");
    if(argc==6&&std::string_view(argv[1])=="--local-client"&&std::string_view(argv[2])=="--root"&&std::string_view(argv[4])=="--node"){
        const auto node=number(argv[5]);if(node<1||node>app::kAlphaNodePoolCapacity)throw std::runtime_error("desktop_node_invalid");
        return launch_desktop_client(argv[3],static_cast<std::uint16_t>(node));}
    if((argc==6||argc==7)&&std::string_view(argv[1])=="--launch"&&std::string_view(argv[2])=="--root"&&std::string_view(argv[4])=="--tzdata"&&(argc==6||std::string_view(argv[6])=="--print-url"))return launch_browser(argv[3],argv[5],argc==7);
    if((argc==6||argc==7)&&std::string_view(argv[1])=="--service-plan"&&std::string_view(argv[2])=="--root"&&std::string_view(argv[4])=="--tzdata"&&(argc==6||std::string_view(argv[6])=="--low-ports")){
        if(getuid()!=geteuid()||getgid()!=getegid()||access("/usr/bin/env",X_OK)!=0)throw std::runtime_error("service_identity_or_launcher_invalid");
        host::layout::require_current(argv[3]);
        const auto root=host::open_owned_directory(argv[3]),zones=host::open_owned_directory(argv[5],false);
        std::cout<<rt::web::hosting_service_plan(std::filesystem::read_symlink("/proc/self/exe"),argv[3],argv[5],getuid(),getgid(),argc==7).dump(2)<<'\n';return 0;
    }
    if((argc!=7&&argc!=8)||std::string_view(argv[1])!="--root"||std::string_view(argv[3])!="--tzdata"||std::string_view(argv[5])!="--port"||(argc==8&&std::string_view(argv[7])!="--service")){
        std::cerr<<"Usage: sfxweb --root /absolute/board --tzdata /absolute/tzdata --port number [--service]\n"
            "       sfxweb --service-plan --root /absolute/board --tzdata /absolute/tzdata [--low-ports]\n"
            "       sfxweb --local-client --root /absolute/board --node number\n";return 64;}
    const auto port=number(argv[6]);if(port>65535)throw std::runtime_error("web_port_invalid");
    std::signal(SIGINT,interrupt);std::signal(SIGTERM,interrupt);return run(argv[2],argv[4],static_cast<unsigned>(port),argc==8);
}catch(const BrowserLaunchError& error){
    std::cerr<<error.what()<<'\n';
    return 1;
}catch(const std::exception& error){
    const std::string_view reason=error.what();
    if(reason.rfind("linux_lowercase_layout_",0)==0)std::cerr<<"This Linux build requires a fresh lowercase SFX layout with its matching marker. Old or mixed-case installations are not migrated.\n";
    else std::cerr<<"Web controls could not start. Check the selected folder and port.\n";
    return 1;}}
