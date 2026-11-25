#include "linux_runtime_store.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include "sfx/platform/linux/portable_key_store.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>

namespace fs=std::filesystem;
namespace app=sfx::application;
namespace host=sfx::platform::linux_host;
namespace rt=sfx::apps::runtime;
namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Clock final:app::IClock{
    std::int64_t unix_seconds()const noexcept override{return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
    std::uint64_t monotonic_millis()const noexcept override{return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
};
struct Revocations final:app::ISessionRevocationSink{bool revoke(const app::SessionRevocationCommand&)override{return true;}};
void directory(const fs::path& path){require(fs::create_directory(path)&&chmod(path.c_str(),0700)==0,"new private fixture directory");}
void run(const fs::path& root,const fs::path& tzdata,const fs::path& artwork,const fs::path& door_fixture,unsigned port,std::string_view mode,unsigned rlogin_port,unsigned telnet_port){
    const bool second_caller=mode=="--second-caller",menu_doors=mode=="--menu-doors",ini_door=mode=="--ini-door";
    require(geteuid()!=0&&root.is_absolute()&&!fs::exists(root)&&port>1024&&port<65536,"normal user fresh fixture high port");
    require(host::LinuxBbsTimeZoneRuleProvider::initialize(tzdata),"bundled timezone");directory(root);
    for(const auto* dir:{"display","games","bin"})directory(root/dir);
    if(mode=="--optional-av"||mode=="--optional-av-pre-off"||mode=="--optional-av-retry-off")directory(root/"bin/clamav");
    for(const auto* name:{"main.ans","main.asc","your.ans","your.asc"}){
        require(!fs::is_symlink(artwork/name)&&fs::is_regular_file(artwork/name)&&fs::copy_file(artwork/name,root/"display"/name),"fixture owner artwork");
        require(chmod((root/"display"/name).c_str(),0600)==0,"private artwork");}
    require(!fs::is_symlink(door_fixture)&&fs::is_regular_file(door_fixture)&&fs::copy_file(door_fixture,root/"games/fixture"),"approved native fixture executable");
    require(chmod((root/"games/fixture").c_str(),0700)==0,"private executable");
    Clock clock;Revocations revocations;rt::LinuxRuntimeStore store(root,revocations,clock,[]{return false;});
    const auto opened=store.open();
    if(opened.code!=app::LocalControlResponseCode::setup_required)
        throw std::runtime_error("new store needs bootstrap: "+opened.diagnostic_code);
    if(mode=="--optional-av-pre-off"){
        const auto files=store.storage()->file_configuration({true,{}});require(files&&files.value,"pre-setup File policy");
        app::FileConfigurationChange change;change.operation=store.identifiers().generate();change.expected_catalog_revision=files.value->policy.catalog_revision;
        change.action=app::FileConfigurationAction::policy;change.policy=files.value->policy;change.policy.scan_uploads=false;
        require(store.storage()->change_file_configuration({true,{}},change).succeeded(),"explicit pre-setup Off save");
    }
    app::AlphaBootstrapRequest request;request.configuration.bbs_name="Standalone Linux fixture";request.configuration.bbs_time_zone="Etc/UTC";
    request.configuration.telnet.bind_address="127.0.0.1";request.configuration.telnet.port=static_cast<std::uint16_t>(port);
    request.configuration.ssh.enabled=false;request.configuration.ssh.bind_address="127.0.0.1";
    request.configuration.botgate.enabled=false;request.configuration.telnet.bot_gate_enabled=false;
    request.configuration.hostname_lookup=false;request.configuration.menus.compact_footer=true;
    request.sysop_name="Fixture Sysop";request.sysop_password="Standalone fixture initial setup 83";request.system_password="Standalone fixture system secret 71";
    if(mode=="--optional-av-retry-off"){
        auto rejected=request;rejected.sysop_name="";const auto outcome=store.bootstrap(rejected);app::erase_bootstrap_secrets(rejected);
        require(outcome.code==app::LocalControlResponseCode::invalid_request,"invalid first bootstrap rejected");
        require(fs::is_regular_file(root/"config/clamav-first-run.v1"),"first-run intent survives rejected attempt");
        const auto files=store.storage()->file_configuration({true,{}});require(files&&files.value,"retry File policy");
        app::FileConfigurationChange change;change.operation=store.identifiers().generate();change.expected_catalog_revision=files.value->policy.catalog_revision;
        change.action=app::FileConfigurationAction::policy;change.policy=files.value->policy;change.policy.scan_uploads=false;
        require(store.storage()->change_file_configuration({true,{}},change).succeeded(),"explicit Off saved between first-run attempts");
    }
    const auto bootstrapped=store.bootstrap(request);app::erase_bootstrap_secrets(request);
    if(bootstrapped.code!=app::LocalControlResponseCode::ok)
        throw std::runtime_error("actual bootstrap transaction: "+bootstrapped.diagnostic_code);
    app::CancellationSource cancel;app::CredentialEnvelope credential;
    require(store.authentication()->create_password_credential("Harness night caller 83",{app::PasswordMode::standard,"Native Caller","Standalone Linux fixture"},clock.unix_seconds(),false,cancel.token(),credential).succeeded(),"real ordinary caller credential");
    app::CreateAccountRequest create;create.display_name="Native Caller";create.credential=credential;
    create.audit={store.identifiers().generate(),store.identifiers().generate(),{},clock.unix_seconds(),app::AuditReasonCode::registration};
    const auto created=store.storage()->create_account(create);require(created.succeeded()&&created.account_id.has_value(),"ordinary fixture caller");
    if(second_caller){
        create.display_name="Second Caller";create.audit.event_id=store.identifiers().generate();create.audit.correlation_id=store.identifiers().generate();
        const auto second=store.storage()->create_account(create);require(second.succeeded()&&second.account_id.has_value(),"second ordinary fixture caller");
    }
    if(mode=="--file-exemption-callers")for(unsigned i=1;i<=35;++i){
        create.display_name="Count Caller "+std::string(i<10?"0":"")+std::to_string(i);create.audit.event_id=store.identifiers().generate();create.audit.correlation_id=store.identifiers().generate();
        const auto added=store.storage()->create_account(create);require(added.succeeded()&&added.account_id.has_value(),"bounded exemption search fixture caller");
    }
    if(mode=="--file-scanning"||mode=="--file-scanning-off"){
        const auto files=store.storage()->file_configuration({true,{}});require(files&&files.value,"fixture File policy");
        app::FileConfigurationChange change;change.operation=store.identifiers().generate();change.expected_catalog_revision=files.value->policy.catalog_revision;
        change.action=app::FileConfigurationAction::policy;change.policy=files.value->policy;change.policy.scan_uploads=mode=="--file-scanning";change.policy.maximum_upload_bytes=1024*1024;
        require(store.storage()->change_file_configuration({true,{}},change).succeeded(),"fixture explicitly enables native scanning");
    }
    if(mode=="--starter-doors"||mode=="--gateway-doors"||mode=="--gateway-hash-doors"||mode=="--gateway-saved-doors"){
        unsigned number=98;
        auto definitions=app::fresh_install_starter_doors(app::StarterDoorPlatform::linux_native);
        if(mode!="--starter-doors"){
            require(rlogin_port>1024&&rlogin_port<65536&&telnet_port>1024&&telnet_port<65536,"fixture gateway ports");
            app::DoorDefinition rlogin;rlogin.id=1;rlogin.name="Native RLogin check";rlogin.code="RLOGIN";rlogin.enabled=true;rlogin.kind=app::DoorExecutionKind::rlogin;
            rlogin.rlogin.host="localhost";rlogin.rlogin.port=static_cast<std::uint16_t>(rlogin_port);rlogin.rlogin.prompt_credential=true;rlogin.rlogin.quiet=true;rlogin.rlogin.pause=false;
            if(mode=="--gateway-hash-doors"){
                rlogin.rlogin.authentication=app::RloginAuthentication::synchronet_caller_hash;
                const auto keys=host::load_portable_board_key(root);require(static_cast<bool>(keys),"fixture portable key");
                const auto pepper=sfx::platform::portable::seal_portable_credential(keys.value->key,keys.value->board,{'S','F','X','R'},"RLOGIN","fixture hash pepper");
                require(pepper.has_value(),"fixture portable pepper");rlogin.rlogin.protected_pepper=*pepper;
            }
            if(mode=="--gateway-saved-doors"){
                rlogin.rlogin.authentication=app::RloginAuthentication::synchronet_predefined_hash;rlogin.rlogin.prompt_credential=false;rlogin.rlogin.salt="FIXTURE-SALT";
                const auto keys=host::load_portable_board_key(root);require(static_cast<bool>(keys),"fixture portable key");
                const auto secret=sfx::platform::portable::seal_portable_credential(keys.value->key,keys.value->board,{'S','F','X','R'},"RLOGIN","fixture saved gateway secret");
                require(secret.has_value(),"fixture portable saved password");rlogin.rlogin.protected_credential=*secret;
            }
            app::DoorDefinition telnet=rlogin;telnet.id=2;telnet.name="Native Telnet check";telnet.code="TELNET";telnet.kind=app::DoorExecutionKind::telnet;
            telnet.telnet.host="127.0.0.1";telnet.telnet.port=static_cast<std::uint16_t>(telnet_port);
            definitions={rlogin,telnet};
        }
        for(const auto& starter:definitions){
            if(mode=="--starter-doors"&&starter.kind!=app::DoorExecutionKind::local)continue;
            const auto catalog=store.storage()->door_configuration({true,{}});require(catalog&&catalog.value,"starter catalog");
            app::DoorConfigurationChange change;change.operation=store.identifiers().generate();change.expected_revision=catalog.value->revision;
            change.action=app::DoorConfigurationAction::save_door;change.create_only=true;change.door=starter;
            const auto saved=store.storage()->change_door_configuration({true,{}},change);require(saved&&saved.value,"native starter definition");
            app::DoorConfigurationChange place;place.operation=store.identifiers().generate();place.expected_revision=saved.value->revision;
            place.action=app::DoorConfigurationAction::save_entry;place.create_only=true;
            place.entry={0,static_cast<std::uint8_t>(number++),app::DoorEntryKind::door,starter.id,starter.name,""};
            require(store.storage()->change_door_configuration({true,{}},place).succeeded(),"native starter placement");
        }
    }else{
    const auto catalog=store.storage()->door_configuration({true,{}});require(catalog&&catalog.value,"Door catalog");
    app::DoorConfigurationChange change;change.operation=store.identifiers().generate();change.expected_revision=catalog.value->revision;change.action=app::DoorConfigurationAction::save_door;change.create_only=true;
    auto& door=change.door;door.id=1;door.code="NATIVE";door.name="Native echo check";door.enabled=true;door.kind=app::DoorExecutionKind::local;
    door.program.type=app::DoorProgramType::native;door.program.io=app::DoorIo::standard;door.program.startup_directory=(root/"games").string();door.program.command_line="fixture stdio";door.program.drop_file=app::DoorDropFile::none;
    if(ini_door){door.program.command_line="fixture ini";door.program.drop_file=app::DoorDropFile::doordrop_ini;}
    const auto saved=store.storage()->change_door_configuration({true,{}},change);require(saved&&saved.value,"fixture Door saved");
    app::DoorConfigurationChange placement;placement.operation=store.identifiers().generate();placement.expected_revision=saved.value->revision;
    placement.action=app::DoorConfigurationAction::save_entry;placement.create_only=true;
    placement.entry={0,99,app::DoorEntryKind::door,1,"Native echo check",""};
    const auto placed=store.storage()->change_door_configuration({true,{}},placement);require(placed&&placed.value,"fixture Door placed");
    if(menu_doors){
        const auto chat=store.storage()->chat_configuration({true,{}});require(chat&&chat.value,"fixture Chat settings");
        auto next=*chat.value;
        next.commands={{app::MenuId::main,'K',1,"Native echo shortcut",true,{}},{app::MenuId::chat,'K',1,"Native echo shortcut",true,{}}};
        require(store.storage()->change_chat_configuration({true,{}},next).succeeded(),"fixture main and Chat Door shortcuts");
        for(const auto event:{app::DoorAutomaticEvent::logon,app::DoorAutomaticEvent::logoff}){
            const auto current=store.storage()->door_configuration({true,{}});require(current&&current.value,"fixture event catalog");
            app::DoorConfigurationChange automatic;automatic.operation=store.identifiers().generate();automatic.expected_revision=current.value->revision;
            automatic.action=app::DoorConfigurationAction::save_door;automatic.create_only=true;automatic.door=door;
            automatic.door.id=event==app::DoorAutomaticEvent::logon?2:3;automatic.door.code=event==app::DoorAutomaticEvent::logon?"LOGON":"LOGOFF";
            automatic.door.name=event==app::DoorAutomaticEvent::logon?"Native logon check":"Native logoff check";
            automatic.door.automatic_event=event;automatic.door.event_only=true;
            require(store.storage()->change_door_configuration({true,{}},automatic).succeeded(),"fixture automatic Door event");
        }
    }
    }
    const auto preference=store.storage()->menu_preferences(*created.account_id);require(preference&&preference.value,"fixture menu preference");
    require(store.close_for_maintenance().code==app::LocalControlResponseCode::ok,"fixture prep releases ownership");
    std::cout<<"PREPARED ordinary_account="<<*created.account_id<<" hotkey="<<(preference.value->preferences.hotkey?1:0)<<"\n";
}
}
int main(int argc,char** argv){try{
    require(argc==6||(argc==7&&(std::string_view(argv[6])=="--second-caller"||std::string_view(argv[6])=="--file-exemption-callers"||std::string_view(argv[6])=="--menu-doors"||std::string_view(argv[6])=="--ini-door"||std::string_view(argv[6])=="--starter-doors"||std::string_view(argv[6])=="--file-scanning"||std::string_view(argv[6])=="--file-scanning-off"||std::string_view(argv[6])=="--optional-av"||std::string_view(argv[6])=="--optional-av-pre-off"||std::string_view(argv[6])=="--optional-av-retry-off"))||(argc==9&&(std::string_view(argv[6])=="--gateway-doors"||std::string_view(argv[6])=="--gateway-hash-doors"||std::string_view(argv[6])=="--gateway-saved-doors")),"root tzdata artwork fixture port and optional fixture mode required");
    run(argv[1],argv[2],argv[3],argv[4],static_cast<unsigned>(std::stoul(argv[5])),argc>=7?std::string_view(argv[6]):std::string_view{},argc==9?static_cast<unsigned>(std::stoul(argv[7])):0,argc==9?static_cast<unsigned>(std::stoul(argv[8])):0);return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
