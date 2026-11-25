#include "linux_caller_session.hpp"
#include "linux_caller_registry.hpp"
#include "linux_caller_listener.hpp"
#include "caller_configuration.hpp"
#include "alpha_trusted_displays.hpp"
#include "alpha_local_door.hpp"
#include "alpha_telnet_stream.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include "sfx/adapters/authentication/phase3a_authentication_adapters.hpp"
#include "sfx/adapters/authentication/sodium_authentication.hpp"
#include "sfx/adapters/storage/control_panel_statistics.hpp"
#include "sfx/application/runtime_statistics.hpp"
#include "sfx/application/menu_session.hpp"
#include "sfx/application/door_session.hpp"
#include "sfx/platform/network.hpp"
#include <sys/stat.h>
#include <future>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
namespace app=sfx::application;
namespace rt=sfx::apps::runtime;
namespace host=sfx::platform::linux_host;
namespace auth=sfx::adapters::authentication;
namespace store=sfx::adapters::storage;
namespace fs=std::filesystem;
unsigned checks=0;
void require(bool value,const char* message) {
    if(!value) throw std::runtime_error(message);
    ++checks;
}
struct Clock final:app::IClock {
    std::int64_t unix_seconds() const noexcept override {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    std::uint64_t monotonic_millis() const noexcept override {
        return sfx::platform::network::monotonic_millis();
    }
};
// This harness does not change credentials after callers connect. Unexpected
// revocation is refused rather than claiming that a live session was cancelled.
struct Revocations final:app::ISessionRevocationSink {
    bool revoke(const app::SessionRevocationCommand&) override {return false;}
};
struct Statistics final:app::ICallerWelcomeStatisticsPort {
    fs::path database; Clock& clock; app::RuntimeStatisticsPublisher publisher;
    Statistics(fs::path path,Clock& value):database(std::move(path)),clock(value) {}
    app::StorageResult<std::uint64_t> record_successful_call_and_count_today() override {
        const auto begin=(clock.unix_seconds()/86400)*86400;
        return publisher.refresh([&]{return store::ControlPanelStatisticsReader::read_checked(
            database,begin,begin+86400);});
    }
};
void private_directory(const fs::path& path) {
    require(fs::create_directory(path),"new fixture directory");
    require(chmod(path.c_str(),0700)==0,"private fixture directory");
}
host::FileDescriptor connect_to(std::uint16_t port) {
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    address.sin_port=htons(port);
    host::FileDescriptor client(socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0));
    require(bool(client) && connect(client.get(),reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,
        "real TCP client");
    require(sfx::platform::network::set_timeout(client.get(),SO_RCVTIMEO,200),"client deadline polling");
    return client;
}
bool write_all(app::IInteractiveSessionByteStream& stream,std::string_view bytes) {
    while(!bytes.empty()) {
        const auto result=stream.write_some(bytes);
        if(result.outcome!=app::InteractiveStreamOutcome::completed || !result.bytes_written ||
            result.bytes_written>bytes.size()) return false;
        bytes.remove_prefix(result.bytes_written);
    }
    return true;
}
std::string visible_text(std::string_view bytes) {
    std::string result;
    for(std::size_t i=0;i<bytes.size();++i) {
        if(bytes[i]=='\x1b' && i+1<bytes.size() && bytes[i+1]=='[') {
            i+=2;
            while(i<bytes.size() && !(bytes[i]>='@' && bytes[i]<='~')) ++i;
        } else result.push_back(bytes[i]);
    }
    return result;
}
void run(const fs::path& root,const fs::path& tzdata,const fs::path& artwork,const fs::path& door_fixture,bool ssh_probe) {
    require(geteuid()!=0,"normal user only");
    private_directory(root);private_directory(root/"data");private_directory(root/"display");
    if(ssh_probe)private_directory(root/"config");
    for(const auto* name:{"main.ans","main.asc","your.ans","your.asc"}) {
        require(!fs::is_symlink(artwork/name) && fs::is_regular_file(artwork/name),"exact artwork input");
        require(fs::copy_file(artwork/name,root/"display"/name),"copy owner artwork into isolated fixture");
        require(chmod((root/"display"/name).c_str(),0600)==0,"private fixture artwork");
    }
    private_directory(root/"games");private_directory(root/"bin");
    require(!fs::is_symlink(door_fixture) && fs::is_regular_file(door_fixture),"qualified native Door fixture");
    require(fs::copy_file(door_fixture,root/"games/fixture"),"new owned native Door program");
    require(chmod((root/"games/fixture").c_str(),0700)==0,"private executable Door fixture");
    const auto owner=std::to_wstring(geteuid());
    std::shared_ptr<const rt::AlphaSshBackend> ssh_backend;
    if(ssh_probe) {
        std::string diagnostic;
        ssh_backend=rt::AlphaSshBackend::acquire(root,owner,diagnostic);
        require(bool(ssh_backend),"native SSH host identity loads");
    }
    rt::ensure_door_node_directories(root,owner,4);
    Clock clock;host::LinuxSecureIdentifierGenerator ids;host::LinuxLocalStoragePathPolicy policy;
    require(host::LinuxBbsTimeZoneRuleProvider::initialize(tzdata),"pinned time zone database");
    auto zone=host::LinuxBbsTimeZoneRuleProvider::create("Etc/UTC");
    require(bool(zone),"explicit UTC zone");
    app::BbsLocalTimeAdapters local(zone->configuration(),*zone,clock);
    store::SqliteStorageSettings settings;settings.layout_profile=sfx::adapters::storage::SqliteStorageLayoutProfile::linux_lowercase_v1;settings.data_directory=root/"data";
    settings.utc_now=[&]{return clock.unix_seconds();};
    store::SqliteStorage database(settings,policy,store::sfx_storage_migrations());
    require(database.start().succeeded(),"native protected SQLite starts");
    auth::SodiumPasswordKdf kdf;auth::PinnedSecListsBlocklist blocklist;Revocations revocations;
    app::AuthenticationSecurityService authentication(database,database,kdf,&blocklist,revocations);
    app::CancellationSource setup;
    app::CredentialEnvelope credential;
    constexpr std::string_view password="Harness night caller 83";
    require(authentication.create_password_credential(password,
        {app::PasswordMode::standard,"Native Caller","Native test BBS"},clock.unix_seconds(),
        false,setup.token(),credential).succeeded(),"shipping password policy and Argon2 credential");
    const auto audit=[&](app::AuditReasonCode reason){return app::AuditContext{
        ids.generate(),ids.generate(),{},clock.unix_seconds(),reason};};
    app::StageInitialSysopRequest initial;initial.display_name="Fixture Sysop";
    initial.credential=credential;initial.audit=audit(app::AuditReasonCode::initial_setup);
    const auto staged=database.stage_initial_sysop(initial);
    require(staged.succeeded() && staged.account_id.has_value(),"stage real initial Sysop");
    const auto security=database.security_policy();require(bool(security)&&security.value.has_value(),"security policy");
    app::CompleteFirstRunRequest finish;finish.staged_sysop_account_id=*staged.account_id;
    finish.expected_policy_revision=security.value->revision;finish.expected_policy_hash=security.value->policy_hash;
    finish.published_policy_hash=security.value->policy_hash;finish.audit=audit(app::AuditReasonCode::initial_setup);
    require(database.complete_first_run(finish).succeeded(),"complete durable first run");
    app::CreateAccountRequest create;create.display_name="Native Caller";create.credential=credential;
    create.audit=audit(app::AuditReasonCode::registration);
    const auto created=database.create_account(create);
    require(created.succeeded()&&created.account_id.has_value(),"ordinary caller account");
    auto catalog=database.door_configuration({true,{}});
    require(catalog && catalog.value,"initial Door catalog");
    app::DoorConfigurationChange door_change;door_change.operation=ids.generate();
    door_change.expected_revision=catalog.value->revision;
    door_change.action=app::DoorConfigurationAction::save_door;door_change.create_only=true;
    auto& door=door_change.door;door.id=1;door.code="NATIVE";door.name="Native echo check";
    door.enabled=true;door.kind=app::DoorExecutionKind::local;door.program.type=app::DoorProgramType::native;
    door.program.io=app::DoorIo::standard;door.program.startup_directory=(root/"games").string();
    door.program.command_line="fixture stdio";door.program.drop_file=app::DoorDropFile::none;
    const auto saved_door=database.change_door_configuration({true,{}},door_change);
    require(saved_door && saved_door.value,"native Door saved through typed catalog");
    app::DoorConfigurationChange placement;placement.operation=ids.generate();
    placement.expected_revision=saved_door.value->revision;placement.action=app::DoorConfigurationAction::save_entry;
    placement.create_only=true;placement.entry={0,99,app::DoorEntryKind::door,1,"Native echo check",""};
    require(bool(database.change_door_configuration({true,{}},placement)),"Door placed on normal root menu");
    app::DoorReservations reservations;
    app::OperationalConfiguration config;config.bbs_name="Native test BBS";config.bbs_time_zone="Etc/UTC";
    config.security_policy_revision=security.value->revision;config.security_policy_hash=security.value->policy_hash;
    const auto snapshot=rt::make_caller_configuration(database,config,"Fixture Sysop","test");
    require(bool(snapshot),"shared caller policy binds published revision");
    auto mismatch=config;++mismatch.security_policy_revision;
    require(!rt::make_caller_configuration(database,mismatch,"Fixture Sysop","test"),"mismatched policy refuses callers");
    auto credentials=auth::Phase3aCredentialOperationsBridge::create(authentication,kdf,clock.unix_seconds(),setup.token());
    require(bool(credentials),"actual authentication bridge");
    auth::Phase3aRateControlCoordinator rates(database,clock.unix_seconds(),4095);
    require(rates.ready(),"durable rate coordinator");
    app::HostIndependentAccountSessionAdmissionCoordinator admission;
    Statistics statistics(root/"data"/"sfx.db",clock);
    for(unsigned mode=ssh_probe?3U:0U;mode<(ssh_probe?4U:6U);++mode) {
        const auto prior_preferences=database.menu_preferences(*created.account_id);
        require(prior_preferences && prior_preferences.value,"current saved preference before session");
        std::shared_ptr<app::CallerCleanupHealth> health;
        app::NodeAssignmentIdentity served_identity;
        rt::AlphaTrustedDisplays displays(root);
        sfx::core::Session session("native-tcp-"+std::to_string(mode),*sfx::core::NodeNumber::create(1),
            {ssh_probe?sfx::core::TransportKind::ssh:sfx::core::TransportKind::telnet,"127.0.0.1",{},"ansi"});
        std::promise<rt::LinuxCallerSessionResult> completion;
        auto future=completion.get_future();
        bool retained=false,finalized=false,released=false,authored_main=false,authored_settings=false,door_returned=false;
        std::string observed_output;bool encrypted=false;
        const auto epoch=ids.generate();rt::LinuxCallerListener* listener_owner=nullptr;
        rt::LinuxCallerRegistry registry(epoch,{
            [&](app::NodeAssignmentIdentity id,std::uint32_t wait){return listener_owner&&listener_owner->wait_for_retirement(id,wait);},
            [&]{if(listener_owner)(void)listener_owner->set_draining(true);}});
        auto listener_config=config;listener_config.telnet.bind_address="127.0.0.1";
        listener_config.telnet.port=0;listener_config.ssh.enabled=false;
        if(ssh_probe) {
            listener_config.telnet.enabled=false;listener_config.ssh.enabled=true;
            listener_config.ssh.bind_address="127.0.0.1";listener_config.ssh.port=0;
        }
        rt::LinuxCallerListenerPorts listener_ports{clock,{},{}};
        listener_ports.admit=[&](std::string_view source,bool ssh,const std::atomic<bool>& stop){
            return rt::LinuxConnectionAdmission{source=="127.0.0.1" && ssh==ssh_probe && !stop.load(),false};
        };
        listener_ports.serve=[&](int socket,bool ssh,const std::shared_ptr<app::CallerAssignment>& assignment) {
            health=assignment->cleanup_health;
            served_identity=assignment->identity();
            require(registry.begin(assignment),"caller enters revocation gate before password work");
            app::Phase3aPrivateCompositionDependencies dependencies{database,*credentials,database,database,
                database,database,admission,database,displays,rates,local,statistics,local,clock,ids,assignment.get(),health};
            rt::LinuxCallerSessionPorts ports{dependencies,*snapshot,session,
                {"127.0.0.1",ids.generate(),{}},assignment->cancellation,assignment->stopping,{}, {},
                static_cast<std::uint16_t>(mode==0?1:30),{},{},{},{},{},{},{}};
            ports.authorize_ready=[&](const app::AuthenticatedCallerReady& ready){
                if(mode==4)require(registry.revoke({app::SessionRevocationKind::account_all,ready.account_id,{}}),
                    "revocation committed while authenticated handoff is pending");
                return registry.publish(assignment->identity(),ready.account_id,
                    ready.durable_session_id,ready.security_level>=security.value->sysop_threshold);
            };
            ports.admission_gate=[](auto&){return true;}; // Isolated test has BotGate disabled.
            ports.ssh_backend=ssh_backend;
            ports.output=[&,assignment](std::string_view bytes){assignment->output(bytes);if(ssh_probe)observed_output.append(bytes);};
            ports.encrypted=[&]{encrypted=true;assignment->encrypted_transport_established();};
            ports.resize=[assignment](rt::SshTerminalDimensions size){assignment->resize(size.columns,size.rows);};
            ports.payload_activity=[assignment](bool input){assignment->payload_activity(input);};
            ports.menus=[&](app::AuthenticatedCallerReady& ready,auto& stream){
                assignment->authenticated(ready.display_name,ready.account_id);
                if(ready.account_id!=*created.account_id)return false;
                app::StreamCallerTerminal terminal(stream);
                (void)terminal.set_text_style(ready.graphics==app::GraphicsMode::ansi?
                    app::TerminalTextStyle::ogsf_ansi:app::TerminalTextStyle::plain);
                app::MenuSessionPorts menus;
                menus.snapshot=[&]() -> std::optional<app::MenuSessionSnapshot> {
                    if(assignment->stopping.load())return {};
                    const auto account=database.find_account_by_id(ready.account_id);
                    const auto preferences=database.menu_preferences(ready.account_id);
                    const auto label=local.format_utc_seconds(clock.unix_seconds());
                    if(!account || !account.value || !account.value->security_level ||
                        !preferences || !preferences.value || !label || label->size()<19)return {};
                    app::MenuSessionSnapshot state;
                    state.access={*account.value->security_level,false,static_cast<std::uint64_t>(account.value->revision)};
                    state.configuration_revision=config.configuration_revision;
                    state.preferences=*preferences.value;state.graphics=ready.graphics;
                    state.compact_footer=true;
                    state.context.bbs_name=config.bbs_name;state.context.sysop="Fixture Sysop";
                    state.context.version="test";state.context.alias=ready.display_name;
                    state.context.name=ready.display_name;state.context.node="1";
                    state.context.nodes=std::to_string(config.logical_nodes);state.context.transport="Telnet";
                    state.context.date=label->substr(0,10);state.context.time=label->substr(11,8);
                    state.context.datetime=*label;state.context.security_level=std::to_string(ready.security_level);
                    return state;
                };
                menus.artwork=[&](const app::TrustedDisplaySelectionRequest& request) {
                    auto selected=displays.select(request);
                    if(selected.available()) {
                        if(request.identity==app::TrustedDisplayIdentity::menu_main)authored_main=true;
                        if(request.identity==app::TrustedDisplayIdentity::menu_settings)authored_settings=true;
                    }
                    return selected;
                };
                menus.display=[&](const app::TerminalDisplayProgram& program){return terminal.present_display(
                    {"menu.artwork",&program,{true,24}})==app::TerminalOutcome::completed;};
                menus.write=[&](std::string_view text){return write_all(stream,text);};
                menus.read=[&](app::MenuInputDecoder& decoder,const std::function<bool()>&) -> std::optional<app::MenuInput> {
                    const auto until=clock.monotonic_millis()+30000;
                    while(!assignment->stopping.load() && clock.monotonic_millis()<until) {
                        const auto input=stream.read_some(1);
                        if(input.outcome==app::InteractiveStreamOutcome::timed_out)continue;
                        if(input.outcome!=app::InteractiveStreamOutcome::completed)return std::nullopt;
                        for(const auto byte:input.bytes) {
                            const auto decoded=decoder.feed(byte);
                            if(decoded.kind!=app::MenuInputKind::pending)return decoded;
                        }
                    }
                    return std::nullopt;
                };
                menus.save_preferences=[&](const app::UpdateMenuPreferenceRequest& request){
                    return request.account_id==ready.account_id && database.update_menu_preferences(request).succeeded();
                };
                // This bounded integration test exercises only native menu
                // navigation/preferences. Any unexpected feature action fails.
                menus.action=[](app::MenuAction)->std::optional<std::string>{return {};};
                menus.utc_now=[&]{return clock.unix_seconds();};
                menus.menu_entered=[assignment](app::MenuId menu){assignment->menu_entered(menu);};
                app::DoorSessionPorts doors{database,menus,{},{},{}};
                doors.actor=[&]() -> std::optional<app::MessageActor> {
                    const auto account=database.find_account_by_id(ready.account_id);
                    const auto current_policy=database.security_policy();
                    if(!account || !account.value || !current_policy || !current_policy.value)return {};
                    return app::MessageActor{ready.durable_session_id,ready.account_id,
                        account.value->revision,current_policy.value->revision};
                };
                doors.launch=[&](const app::DoorLaunch& launch) -> std::optional<app::DoorEnd> {
                    auto reservation=reservations.acquire(launch.door.id,launch.door.allow_multiple);
                    if(!reservation)return app::DoorEnd::launch_failed;
                    const auto sequence=assignment->begin_door(launch.door.id,launch.door.name,app::DoorExecutionKind::local);
                    if(!sequence)return std::nullopt;
                    auto* telnet=ssh?nullptr:&static_cast<rt::AlphaTelnetStream&>(stream);
                    if(telnet && !telnet->begin_gateway())return std::nullopt;
                    struct Restore {
                        rt::AlphaTelnetStream* stream;std::shared_ptr<app::CallerAssignment> state;std::uint64_t sequence;
                        ~Restore(){if(stream && !stream->end_gateway())state->cleanup_health->failed=true;state->end_door(sequence);}
                    } restore{telnet,assignment,sequence};
                    app::DoorChannel channel;
                    channel.read=[&](std::size_t maximum){return ssh?static_cast<rt::AlphaSshStream&>(stream).read_ready(maximum):telnet->read_ready(maximum);};
                    channel.write=[&](std::string_view bytes){return ssh?static_cast<rt::AlphaSshStream&>(stream).write_ready(bytes):telnet->write_ready(bytes);};
                    const auto deadline=clock.monotonic_millis()+10000;
                    channel.stop=[&]() -> std::optional<app::DoorEnd> {
                        if(assignment->stopping.load())return app::DoorEnd::shutdown;
                        if(clock.monotonic_millis()>=deadline)return app::DoorEnd::calling_time;
                        return std::nullopt;
                    };
                    rt::LocalDoorCaller caller;caller.account=static_cast<std::uint32_t>(ready.account_id);
                    caller.alias=ready.display_name;caller.node=ready.node_id;caller.security_level=ready.security_level;
                    caller.board_name=config.bbs_name;caller.ansi=ready.graphics==app::GraphicsMode::ansi;
                    caller.remaining_seconds=60;caller.captured_utc_seconds=clock.unix_seconds();
                    caller.generation=assignment->identity().generation;caller.sequence=sequence;
                    const auto label=local.format_utc_seconds(caller.captured_utc_seconds);
                    if(!label || label->size()<19)return app::DoorEnd::launch_failed;
                    auto& drop=caller.drop_context;drop.nodes=config.logical_nodes;drop.sysop="Fixture Sysop";
                    drop.date=label->substr(5,2)+"/"+label->substr(8,2)+"/"+label->substr(2,2);
                    drop.clock=label->substr(11,5);drop.logon_date=drop.date;drop.logon_clock=drop.clock;
                    auto prefix=terminal.take_binary_input_prefix();
                    const auto result=rt::run_alpha_local_door(root,owner,launch.door,caller,channel,
                        std::move(prefix.bytes),prefix.discard_delimiter_lf);
                    if(result.cleanup_complete)reservation->release_after_cleanup();
                    else assignment->cleanup_health->failed=true;
                    door_returned=result.end==app::DoorEnd::returned && result.cleanup_complete;
                    return result.end;
                };
                menus.doors=[&]{return app::run_door_session(doors);};
                return app::run_menu_session(menus);
            };
            auto result=rt::run_linux_caller_session(socket,ssh,std::move(ports));
            bool clean=!health->failed.load();
            if(result.ready) {
                retained=admission.active_leases()==1;
                finalized=result.ready->durable_cleanup->finalize(0,result.normal_logoff?app::SessionOutcomeCode::normal:
                    assignment->stopping.load()?app::SessionOutcomeCode::shutdown:app::SessionOutcomeCode::disconnected);
                released=result.ready->admission_lease->release();
                clean=clean && finalized && released;
            }
            clean=registry.forget(assignment->identity(),clean)&&clean;
            completion.set_value(std::move(result));
            return clean;
        };
        rt::LinuxCallerListener listener(epoch,listener_config,std::move(listener_ports));
        listener_owner=&listener;
        require(listener.start(),"native product listener starts");
        const auto listening=listener.status();
        require(listening.listening && (ssh_probe?(!listening.telnet_port && listening.ssh_port):(listening.telnet_port && !listening.ssh_port)),"exact selected endpoint published");
        host::FileDescriptor client;
        if(!ssh_probe)client=connect_to(listening.telnet_port);
        const auto begin=clock.monotonic_millis();
        std::string transcript;
        if(ssh_probe) {
            std::cout<<"SSH_PORT "<<listening.ssh_port<<std::endl;
            if(future.wait_for(std::chrono::seconds(40))!=std::future_status::ready)listener.stop();
            require(future.wait_for(std::chrono::seconds(1))==std::future_status::ready,"external SSH client completes bounded session");
            transcript=observed_output;
            require(encrypted,"actual SSH encrypted observer reached");
            require(transcript.find(password)==std::string::npos,"SSH password absent from server output");
            std::ofstream(root/"ssh-server-output.txt",std::ios::binary)<<transcript;
            require(chmod((root/"ssh-server-output.txt").c_str(),0600)==0,"private SSH transcript");
        } else if(mode==0) {
            require(future.wait_for(std::chrono::seconds(4))==std::future_status::ready,"silent login wakes at configured deadline");
        } else if(mode==1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            listener.stop();
            require(future.wait_for(std::chrono::seconds(2))==std::future_status::ready,"Stop reaps idle caller promptly");
        } else {
            // Synthetic fixture password is sent only after its actual prompt;
            // the test retains output, never client input or production secrets.
            const bool before_hotkey=prior_preferences.value->preferences.hotkey;
            const auto command=[](std::string text,bool hotkey){if(!hotkey)text+='\r';return text;};
            std::vector<std::pair<std::string,std::string>> script{
                {"[Y/n] ",mode==3?"y":"n"},{"Enter your first name: ","Native\r"},
                {"Enter your last name: ","Caller\r"},{"password",std::string(password)+"\r"},
                {"Press <ENTER> to continue.","\r"},
                {"Command: ",command("Y",before_hotkey)},{"Command: ",command("H",before_hotkey)},
                {"Command: ",command("Q",!before_hotkey)},{"Command: ",command("D",!before_hotkey)},
                {"Command: ",command("99",!before_hotkey)},{"LINUX READY","hello q"},
                {"Command: ",command("Q",!before_hotkey)},{"Command: ",command("/G",!before_hotkey)}};
            if(mode>=4)script.resize(5);
            std::size_t step=0,consumed=0;
            bool revoked_live=false;
            while(future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready && clock.monotonic_millis()-begin<15000) {
                char bytes[2048];const auto n=recv(client.get(),bytes,sizeof(bytes),0);
                if(n>0) transcript.append(bytes,static_cast<std::size_t>(n));
                if(mode==5&&!revoked_live&&step==5&&visible_text(std::string_view(transcript).substr(consumed)).find("Command: ")!=std::string::npos) {
                    require(registry.account_online(*created.account_id),"published caller is live before revocation");
                    require(registry.revoke({app::SessionRevocationKind::account_all,*created.account_id,{}}),
                        "live revocation waits for actual listener worker retirement");
                    revoked_live=true;
                }
                if(step<script.size()) {
                    auto lower=visible_text(std::string_view(transcript).substr(consumed));
                    if(step==3) for(auto& c:lower) if(c>='A'&&c<='Z') c=static_cast<char>(c-'A'+'a');
                    if(lower.find(script[step].first)!=std::string::npos) {
                        const auto& answer=script[step].second;
                        require(send(client.get(),answer.data(),answer.size(),MSG_NOSIGNAL)==static_cast<ssize_t>(answer.size()),"scripted prompt reply");
                        consumed=transcript.size();++step;
                    }
                }
            }
            if(future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) {
                listener.stop();
            }
            require(transcript.find(password)==std::string::npos,"password never echoed in TCP output");
            const auto output=root/("login-output-"+std::to_string(mode)+".txt");
            std::ofstream(output,std::ios::binary)<<transcript;
            require(chmod(output.c_str(),0600)==0,"private synthetic output evidence");
            require(step==script.size(),"all real login prompts reached");
            if(mode==5)require(revoked_live,"live-menu revocation was exercised");
            require((transcript.find('\x1b')!=std::string::npos)==(mode==3),"ANSI selection controls actual output escapes");
        }
        auto result=future.get();
        std::cout<<"CASE "<<mode<<" login="<<static_cast<unsigned>(result.login)
            <<" ready="<<result.caller_ready<<" diagnostic="<<result.diagnostic<<'\n';
        if(mode>=4) {
            require(result.ready.has_value()&&!result.normal_logoff,"revoked authenticated session retains checked cleanup handoff");
            require(retained&&finalized&&released,"revoked caller finalizes before account admission release");
            require(!registry.account_online(*created.account_id),"revoked worker removes exact published identity");
            if(mode==4)require(!result.caller_ready&&!authored_main&&result.diagnostic=="caller_access_revoked",
                "delayed revoked login never enters main menu");
            else require(result.caller_ready&&authored_main,"live-menu caller retired after publication");
            result.ready.reset();
        } else if(mode>=2) {
            require(result.caller_ready && result.ready.has_value(),"real TCP reaches authenticated durable handoff");
            require(result.ready->graphics==(mode==3?app::GraphicsMode::ansi:app::GraphicsMode::ascii),"selected graphics reaches durable caller handoff");
            require(result.normal_logoff,"authenticated callback completed");
            require(authored_main && authored_settings,"actual owner Main and Your Info artwork rendered");
            require(door_returned,"authenticated caller entered native Door and returned with complete cleanup");
            require(transcript.find("LINUX RETURN")!=std::string::npos && transcript.find("PRIVATE DIAGNOSTIC")==std::string::npos,
                "Door output returns over TCP and private child diagnostics stay out of caller stream");
            const auto preferences=database.menu_preferences(*created.account_id);
            require(preferences && preferences.value &&
                preferences.value->preferences.hotkey!=prior_preferences.value->preferences.hotkey &&
                preferences.value->revision==prior_preferences.value->revision+1,
                "caller preference saved through shared menu and SQLite");
            require(retained,"handoff retains exclusive account lease");
            require(finalized,"durable finalization succeeds");
            require(released,"admission released after durable finalization");
            result.ready.reset();
        } else {
            require(!result.caller_ready && !result.ready,"idle/stop never confers account authority");
            if(mode==0) require(result.login==app::Phase3aWorkflowCode::timed_out,"idle login reports actual timeout");
        }
        listener.stop();
        const auto stopped=listener.status();
        require(!stopped.listening && stopped.pool.active==0 && stopped.diagnostic=="stopped","Stop joins workers before releasing nodes");
        require(listener.wait_for_retirement(served_identity,0),"exact assignment retirement is confirmed after joined cleanup");
        auto foreign=served_identity;foreign.runtime_epoch=ids.generate();
        require(!listener.wait_for_retirement(foreign,0),"foreign Runtime retirement proof refused");
        auto future_identity=served_identity;++future_identity.generation;
        require(!listener.wait_for_retirement(future_identity,0),"future unserved assignment cannot claim retirement");
        require(health && !health->failed.load() && admission.active_leases()==0,"no leaked caller cleanup or admission lease");
        (void)session.request_disconnect();(void)session.finalize_disconnect();
    }
    require(database.shutdown().succeeded(),"storage stops cleanly");
}
}
int main(int argc,char** argv) {
    try {
        require(argc==5 || (argc==6 && std::string(argv[5])=="ssh-probe"),"explicit new fixture, pinned tzdata, owner artwork and native Door paths");
        run(fs::path(argv[1]),fs::path(argv[2]),fs::path(argv[3]),fs::path(argv[4]),argc==6);
        std::cout<<"PASS native TCP caller session: "<<checks<<" checks\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"FAIL native caller after "<<checks<<": "<<error.what()<<'\n';return 1;
    }
}
