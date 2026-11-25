// Test-only composition for R1 loopback journal checks. It uses production
// listener/authentication/storage/journal classes, NOT the shipping installation
// entry point. No receipt, elevated work, structural repair, or Runtime bypass.
#include "alpha_telnet_listener.hpp"
#include "alpha_durable_journal.hpp"
#include "alpha_ssh_stream.hpp"
#include "alpha_hostname_lookup.hpp"
#include "alpha_botgate_feed.hpp"
#include "sfx/platform/win32/botgate_rule_files.hpp"
#include "sfx/platform/win32/other_board_text_file.hpp"
#include "sfx/application/local_account_administration.hpp"
#include "../support/section7_host_commands.hpp"
#include "../support/section8a_host_commands.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/storage_path_policy.hpp"
#include "sfx/platform/win32/secure_identifier_generator.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <windows.h>

namespace app = sfx::application;
namespace rt = sfx::apps::runtime;
namespace win = sfx::platform::win32;
namespace fs = std::filesystem;
namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
std::int64_t utc() { return std::chrono::duration_cast<std::chrono::seconds>(
    std::chrono::system_clock::now().time_since_epoch()).count(); }
class AdministrationClock final : public app::IClock {
public:
    std::int64_t unix_seconds() const noexcept override { return utc(); }
    std::uint64_t monotonic_millis() const noexcept override { return GetTickCount64(); }
};
class Revocations final : public app::ISessionRevocationSink {
public:
    rt::AlphaTelnetListener* listener{};
    bool revoke(const app::SessionRevocationCommand& command) override {
        return listener ? listener->revoke_sessions(command) : true; // fixture seeding only
    }
    bool refresh_account_access(app::AccountId id) override {return listener && listener->refresh_account_access(id);}
};
void seed_file(const fs::path& path, std::string_view value, win::WindowsStorageDaclPolicy& policy) {
    require(!fs::exists(path), "fixture_content_exists");
    { std::ofstream out(path, std::ios::binary); out.write(value.data(), static_cast<std::streamsize>(value.size()));
      require(out.good(), "fixture_content_write"); }
    require(policy.apply(path, win::StorageDaclObjectKind::file).succeeded(), "fixture_content_protection");
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 5) return 2;
        const auto root = fs::absolute(argv[1]).lexically_normal();
        const auto permitted = fs::path(SFX_R1_JOURNAL_SCRATCH).lexically_normal();
        require(root.parent_path() == permitted && root.filename().wstring().find(L"r1-journal-") == 0,
            "fixture_root_out_of_scope");
        const std::string mode = argv[4];
        require(mode == "section6" || mode == "login-menu" || mode == "valid" || mode == "missing" || mode == "invalid" ||
            mode == "write-failure" || mode == "custom-security" || mode == "high-capacity" ||
            mode == "high-start-failure" || mode == "connection-limits" || mode == "botgate" || mode == "botgate-closed", "fixture_mode");
        const auto telnet = std::stoul(argv[2]), ssh = std::stoul(argv[3]);
        require(telnet > 1024 && telnet <= 65535 && ssh > 1024 && ssh <= 65535 && telnet != ssh, "fixture_ports");
        const auto sid = win::current_process_user_sid_string();
        require(!sid.empty(), "fixture_identity");
        win::WindowsStorageDaclPolicy protection(sid);
        const bool fresh = !fs::exists(root);
        if (fresh) {
            require(fs::create_directories(root), "fixture_create");
            for (const auto* leaf : {L"Config", L"Data", L"Logs", L"Display", L"BotGate"}) {
                require(fs::create_directory(root / leaf), "fixture_directory");
                require(protection.apply(root / leaf, win::StorageDaclObjectKind::directory).succeeded(), "fixture_private_boundary");
            }
            seed_file(root / "fixture.marker", "R1 journal loopback fixture only\r\n", protection);
            if (mode != "missing") seed_file(root / "Config/JOKER.DAT", mode == "invalid" ? "bad\x1bname\r\n" :
                "; R1/F-06 test-only exact names\r\n  Blocked Caller  \r\nCaller One\r\nhax0r\r\n", protection);
            if (mode == "write-failure") require(fs::create_directory(root / "Logs/HEYSYSOP.LOG"), "fixture_unavailable_sink");
        } else {
            std::ifstream marker(root / "fixture.marker", std::ios::binary);
            const std::string value((std::istreambuf_iterator<char>(marker)), std::istreambuf_iterator<char>());
            require(value == "R1 journal loopback fixture only\r\n", "fixture_restart_marker");
            require(!(GetFileAttributesW(root.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT), "fixture_restart_boundary");
        }
        win::WindowsLocalStoragePathPolicy paths(sid);
        std::weak_ptr<rt::AlphaDurableJournal> board_journal;
        win::initialize_other_board_text(root,sid);
        sfx::adapters::storage::SqliteStorageSettings settings; settings.data_directory = root / "Data"; settings.utc_now = utc;
        settings.other_board_text=std::make_shared<win::WindowsOtherBoardTextStorage>(root,sid,
            [&](const app::OtherBoardLogRecord& record){auto journal=board_journal.lock();return journal && journal->other_board(record);});
        sfx::adapters::storage::SqliteStorage storage(settings, paths);
        require(storage.start().succeeded(), "fixture_storage_start");
        sfx::adapters::authentication::SodiumPasswordKdf kdf(2, 16);
        Revocations revocations;
        app::AuthenticationSecurityService authentication(storage, storage, kdf, nullptr, revocations);
        win::WindowsSecureIdentifierGenerator ids;
        app::OperationalConfiguration config;
        if(mode=="section6") config.logical_nodes=4;
        if (mode == "high-capacity" || mode == "high-start-failure") config.logical_nodes = 128;
        if (mode == "connection-limits") {
            config.logical_nodes = 12;
            config.maximum_connections_per_source = 2;
            config.maximum_sessions_per_account = 2;
            config.source_connection_exceptions = {{"127.0.0.1",3},{"127.0.0.2",1}};
        }
        config.bbs_name = "SFX R1 Journal QA"; config.bbs_time_zone = "Pacific Standard Time";
        config.hostname_lookup = false; // no network DNS dependency in this journal gate
        config.telnet = {true, "127.0.0.1", static_cast<std::uint16_t>(telnet), false};
        config.botgate.enabled=mode=="botgate" || mode=="botgate-closed";
        config.botgate.ssh_enabled=config.botgate.enabled; // explicit opt-in fixture, not a fresh-install default
        config.allow_new_users=mode!="botgate-closed";
        config.botgate.timeout_seconds=5;
        config.telnet.bot_gate_enabled=mode=="botgate";
        config.ssh = {true, "127.0.0.1", static_cast<std::uint16_t>(ssh)};
        const auto policy = storage.security_policy(); require(policy && policy.value, "fixture_policy");
        config.security_policy_revision = policy.value->revision; config.security_policy_hash = policy.value->policy_hash;
        if (fresh) {
            app::CancellationSource cancellation;
            const auto account = [&](const std::string& name, bool sysop) {
                app::CreateAccountRequest request; request.display_name = name;
                require(authentication.create_password_credential("Sfx-test-password-123",
                    {config.password_mode, name, config.bbs_name}, utc(), false, cancellation.token(), request.credential).succeeded(),
                    "fixture_credential");
                request.audit = {ids.generate(), ids.generate(), {}, utc(), sysop ? app::AuditReasonCode::initial_setup : app::AuditReasonCode::registration};
                const auto result = sysop ? storage.stage_initial_sysop(request) : storage.create_account(request);
                require(result && result.account_id, "fixture_account"); return *result.account_id;
            };
            const auto sysop = account("SSH Fixture", true);
            app::SystemPasswordReplacementOperation replace;
            replace.new_password = "Sfx-system-password-456";
            replace.policy = {config.password_mode, "SSH Fixture", config.bbs_name};
            replace.audit = {ids.generate(), ids.generate(), sysop, utc()};
            require(authentication.replace_system_password(replace, cancellation.token()).succeeded(), "fixture_system_credential");
            require(storage.complete_first_run({sysop, policy.value->revision, policy.value->policy_hash, policy.value->policy_hash,
                {ids.generate(), ids.generate(), sysop, utc(), app::AuditReasonCode::initial_setup}}).succeeded(), "fixture_bootstrap");
            for (const auto* name : {"Caller One", "Caller Two", "Caller Three", "Caller Four"}) (void)account(name, false);
            if(mode=="login-menu") for(const auto* name:{"xbit","xbit ops","solo ops"}) (void)account(name,false);
            if (mode == "custom-security") {
                app::PublishSecurityPolicyRequest custom;
                custom.expected_revision = policy.value->revision;
                custom.levels = {{10,"Original",{},app::SecurityLevelState::active},
                    {20,"New Caller",{},app::SecurityLevelState::active},
                    {500,"Operator",{},app::SecurityLevelState::active},
                    {999,"Owner",{},app::SecurityLevelState::active}};
                custom.new_user_level = 20; custom.sysop_threshold = 500;
                custom.policy_hash = custom.published_policy_hash = std::string(64, 'a');
                custom.audit = {ids.generate(), ids.generate(), sysop, utc(), app::AuditReasonCode::security_policy};
                require(storage.publish_security_policy(custom).succeeded(), "fixture_custom_policy");
                const auto caller = storage.find_account_by_name("Caller One");
                require(caller && caller.value && storage.assign_security_level({caller.value->account_id,
                    caller.value->revision, custom.expected_revision + 1, 500,
                    {ids.generate(),ids.generate(),sysop,utc(),app::AuditReasonCode::sysop_action}}).succeeded(),
                    "fixture_custom_assignment");
                config.security_policy_revision = custom.expected_revision + 1;
                config.security_policy_hash = custom.policy_hash;
            }
        }
        const auto epoch = ids.generate();
        auto operational = std::make_shared<rt::AlphaOperationalJournal>(root / "Logs/SFXRuntime.log", sid, epoch);
        auto durable = std::make_shared<rt::AlphaDurableJournal>(storage, ids, root, sid, config.bbs_time_zone, operational);
        board_journal=durable;
        require(durable->start(), "fixture_authoritative_recovery");
        operational->attention_sink([weak=std::weak_ptr<rt::AlphaDurableJournal>(durable)](app::AttentionRecord record) {
            if (auto journal=weak.lock()) journal->attention(std::move(record));
        });
        std::atomic<bool> feed_block{false};
        std::shared_ptr<rt::AlphaBotGateFeed> feed;
        win::initialize_botgate_files(root,sid,config.botgate);
        if (mode=="botgate") {
            feed=std::make_shared<rt::AlphaBotGateFeed>(root,sid,operational,
                [&](const std::atomic<bool>&,rt::AlphaBotGateFeed::Progress,std::string&)->std::optional<std::string> {
                    return feed_block ? "127.0.0.1\n" : "192.0.2.1\n";
                });
            require(feed->start(),"fixture_feed_start");
        }
        std::string diagnostic;
        auto backend = rt::AlphaSshBackend::acquire(root, sid, diagnostic);
        require(static_cast<bool>(backend), "fixture_shared_ssh_backend");
        {
            std::mutex handoff_mutex;
            std::condition_variable handoff_changed;
            bool hold_handoff = false, handoff_entered = false;
            unsigned worker_starts = 0;
            rt::AlphaTelnetListener listener(storage, authentication, kdf, ids, config, {}, root, sid,
                backend, [&](rt::AlphaCallerBoundary boundary) {
                    if (boundary == rt::AlphaCallerBoundary::worker_start && mode == "high-start-failure" &&
                        ++worker_starts == 64) throw std::runtime_error("fixture_worker_64_failure");
                    if (boundary != rt::AlphaCallerBoundary::ready_handoff) return;
                    std::unique_lock<std::mutex> lock(handoff_mutex);
                    if (!hold_handoff) return;
                    handoff_entered = true;
                    require(handoff_changed.wait_for(lock, std::chrono::seconds(20), [&] {
                        return !hold_handoff;
                    }), "fixture_handoff_timeout");
                }, app::default_node_enablement(), epoch, {}, operational, durable,feed);
            revocations.listener = &listener;
            AdministrationClock administration_clock;
            app::LocalAccountAdministration administration(storage, storage, authentication,
                revocations, ids, administration_clock, config, app::LocalAccountAuthority::installation_owner);
            app::LocalClassicInformationAdministration classic(storage,ids,administration_clock,app::LocalAccountAuthority::installation_owner);
            app::LocalMessageAdministration messages(storage,app::LocalAccountAuthority::installation_owner);
            const bool listener_started = listener.start();
            require(listener_started == (mode != "high-start-failure"), "fixture_listener_start");
            if (!listener_started) listener.stop(); // same retire/join contract as Runtime host
            if (!listener_started) require(worker_starts == 64 && rt::alpha_caller_resource_counts().workers == 0,
                "fixture_partial_start_cleanup");
            std::cout << "READY" << std::endl;
            std::array<std::optional<app::NodeAssignmentIdentity>, app::kAlphaNodePoolCapacity> local_reservations{};
            std::string command;
            while (std::getline(std::cin, command)) {
                if (command == "stop") break;
#if defined(SFX_CHAT_EARLY_FIXTURE)
                if(command=="sound-poll"){
                    static app::SoundPollRequest cursor{ids.generate(),{},0,false};
                    const auto sounds=operational->sound_poll(cursor,config.event_sounds);
                    cursor.epoch=sounds.epoch;cursor.after=sounds.through;
                    std::cout<<"SOUNDS [";bool comma{};for(const auto& item:sounds.events){if(comma)std::cout<<',';comma=true;std::cout<<static_cast<unsigned>(item.event);}std::cout<<']'<<std::endl;continue;
                }
                if(command.rfind("operator ",0)==0){
                    const auto text=command.substr(9);std::vector<std::uint8_t> bytes;
                    const auto nibble=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
                    bool valid=text.size()<=1200&&text.size()%2==0;
                    for(std::size_t i=0;valid&&i<text.size();i+=2){const int a=nibble(text[i]),b=nibble(text[i+1]);if(a<0||b<0)valid=false;else bytes.push_back(static_cast<std::uint8_t>((a<<4)|b));}
                    const auto request=valid?app::decode_chat_operator_request(bytes):std::nullopt;
                    const auto result=request?listener.chat_operator(*request):app::ChatOperatorSnapshot{};
                    const auto encoded=app::encode_chat_operator_snapshot(result);std::string hex;constexpr char digits[]="0123456789abcdef";
                    for(const auto byte:encoded){hex+=digits[byte>>4];hex+=digits[byte&15];}std::cout<<"OPERATOR "<<hex<<std::endl;continue;
                }
                if(command=="rooms"||command=="rename") {auto c=storage.chat_configuration({true,{}});app::ChatResult result=app::ChatResult::unavailable;if(c&&c.value){auto value=*c.value;if(command=="rooms")value.rooms.push_back({2,"Games",true,{app::DoorSecurityMode::minimum,10,false}});else value.rooms[0].name="Renamed Lobby";result=listener.change_chat_configuration(value).code;}std::cout<<(command=="rooms"?"ROOMS ":"RENAME ")<<static_cast<unsigned>(result)<<std::endl;continue;}
                if(command=="notice") {app::ChatNoticeRequest request;request.text="Controlled system notice";std::cout<<"NOTICE "<<static_cast<unsigned>(listener.send_chat_notice(request))<<std::endl;continue;}
                if(command=="notice-wrap") {app::ChatNoticeRequest request;request.text="Controlled wrapping notice with  repeated spaces. "+std::string(113,'W')+" Final complete sentence. ENDNOTICE";std::cout<<"NOTICE "<<static_cast<unsigned>(listener.send_chat_notice(request))<<std::endl;continue;}
                if(command.rfind("screen ",0)==0) {const auto number=std::stoul(command.substr(7));bool found=false;for(const auto& node:listener.status().nodes)if(node.number==number){std::string hex;constexpr char digits[]="0123456789abcdef";for(const auto& cell:node.screen.cells){const auto c=static_cast<unsigned char>(cell.character);hex+=digits[c>>4];hex+=digits[c&15];hex+=digits[cell.attribute>>4];hex+=digits[cell.attribute&15];}std::cout<<"SCREEN "<<node.terminal_columns<<' '<<node.terminal_rows<<' '<<hex<<std::endl;found=true;}if(!found)std::cout<<"SCREEN 0 0 -"<<std::endl;continue;}
                if(command=="audit") {bool clean=true;std::ifstream file(root/"Logs/SFXRuntime.log",std::ios::binary);const std::string text((std::istreambuf_iterator<char>(file)),{});for(const auto* body:{"Synthetic private hello","Controlled system notice","Synthetic public hello"})if(text.find(body)!=std::string::npos)clean=false;std::cout<<"AUDIT "<<clean<<std::endl;continue;}
#endif
                if(section8a_command(command,[&](const app::LocalMessageConfigurationQuery& q){return messages.snapshot(q);},
                    [&](const app::MessageConfigurationChange& c){return messages.execute(c);}))continue;
                if(command=="menu-colors" || command=="menu-defaults" || command=="menu-compact" || command=="menu-original") {
                    auto candidate=config;++candidate.configuration_revision;
                    if(command=="menu-compact" || command=="menu-original") candidate.menus.compact_footer=command=="menu-compact";
                    else candidate.menus=command=="menu-colors" ? app::MenuOperationalConfiguration{2,14,false,15,1} : app::MenuOperationalConfiguration{};
                    std::string detail;const bool changed=listener.reconfigure_live_settings(candidate,detail);
                    if(changed) config=candidate;
                    std::cout<<"{\"changed\":"<<changed<<'}'<<std::endl;continue;
                }
                if(mode=="botgate" && (command=="ssh-challenge-off" || command=="filters-off")) {
                    auto candidate=config; ++candidate.configuration_revision;
                    if(command=="ssh-challenge-off") candidate.botgate.ssh_enabled=false;
                    else candidate.botgate.enabled=false;
                    std::string filter_diagnostic;
                    const bool changed=listener.reconfigure_live_settings(candidate,filter_diagnostic);
                    if(changed) config=candidate;
                    std::cout<<"GATE "<<changed<<' '<<filter_diagnostic<<std::endl; continue;
                }
                if (mode=="botgate" && (command=="feed-block" || command=="feed-clear")) {
                    const auto deadline=GetTickCount64()+5000;
                    while (feed->status().updating && GetTickCount64()<deadline) Sleep(10);
                    feed_block=command=="feed-block";
                    require(feed->update_now(),"fixture_feed_update");
                    const bool blocked=feed_block;
                    while (GetTickCount64()<deadline) {
                        const auto snapshot=feed->snapshot();
                        if (!feed->status().updating && !snapshot->empty() && (snapshot->front()==0x7f000001U)==blocked) break;
                        Sleep(10);
                    }
                    require(feed->status().result=="feed_update_success" &&
                        (feed->snapshot()->front()==0x7f000001U)==blocked,"fixture_feed_activation");
                    std::cout << "GATE 1 feed_updated" << std::endl; continue;
                }
                if (mode=="botgate" && (command=="gate-off" || command=="gate-on" || command=="gate-trust" || command=="gate-block" ||
                        command=="gate-telnet-off" || command=="gate-ssh-off" || command=="gate-except")) {
                    auto candidate=config;
                    ++candidate.configuration_revision;
                    candidate.botgate.enabled=command!="gate-off";
                    win::save_botgate_rule(root,sid,0,command=="gate-trust" ? "127.0.0.1\n" : "");
                    win::save_botgate_rule(root,sid,1,command=="gate-block" ? "127.0.0.1\n" : "");
                    candidate.telnet.bot_gate_enabled=command!="gate-telnet-off";
                    candidate.botgate.ssh_enabled=command!="gate-ssh-off";
                    win::save_botgate_rule(root,sid,5,command=="gate-except" ? "127.0.0.1\n" : "");
                    std::this_thread::sleep_for(std::chrono::milliseconds(2300));
                    std::string gate_diagnostic;
                    const bool changed=listener.reconfigure_live_settings(candidate,gate_diagnostic);
                    if (changed) config=candidate;
                    std::cout << "GATE " << changed << ' ' << gate_diagnostic << std::endl;
                    continue;
                }
                if (mode=="botgate" && (command=="art-one" || command=="art-two" || command=="art-invalid" || command=="art-remove")) {
                    const auto asset=root/"Display"/"bg-prelog.ans";
                    if (command=="art-remove") require(fs::remove(asset),"fixture_remove_art");
                    else {
                        std::ofstream out(asset,std::ios::binary|std::ios::trunc);
                        out << (command=="art-invalid" ? "\x1b]0;unsafe\a" :
                            command=="art-one" ? "\x1b[2J\x1b[HART ONE @literal ## seconds." : "\x1b[2J\x1b[HART TWO @literal ## seconds.");
                        require(out.good(),"fixture_art_write"); out.close();
                        require(protection.apply(asset,win::StorageDaclObjectKind::file).succeeded(),"fixture_art_protection");
                    }
                    std::cout << "GATE 1 artwork_changed" << std::endl; continue;
                }
                if (command == "clear-fixture-source-throttle") {
                    const auto result = listener.clear_throttle({app::RateLimitScope::source,"127.0.0.1"},
                        {ids.generate(),ids.generate(),{},utc()});
                    std::cout << "{\"succeeded\":" << result.succeeded() << '}' << std::endl;
                    continue;
                }
                if (command == "drain" || command == "resume") {
                    std::cout << "{\"changed\":" << listener.set_draining(command == "drain") << '}' << std::endl;
                    continue;
                }
                if (command == "notice-five" || command == "notice-one") {
                    const auto seconds = command == "notice-five" ? 300U : 60U;
                    std::cout << "{\"changed\":" << listener.set_draining(true,
                        GetTickCount64() + seconds * 1000ULL) << '}' << std::endl;
                    continue;
                }
                if (command == "disable-last" || command == "enable-last") {
                    std::cout << "{\"changed\":" << listener.set_node_enabled(config.logical_nodes,
                        command == "enable-last") << '}' << std::endl;
                    continue;
                }
                if (command == "disable-all") {
                    bool changed = true;
                    for (std::uint16_t node = 1; node <= config.logical_nodes; ++node)
                        changed = listener.set_node_enabled(node, false) && changed;
                    std::cout << "{\"changed\":" << changed << '}' << std::endl;
                    continue;
                }
                if (command == "disconnect-last") {
                    const auto result = listener.disconnect_caller(listener.status().nodes.back().screen_owner);
                    std::cout << "{\"code\":" << static_cast<unsigned>(result.code)
                        << ",\"diagnostic\":" << std::quoted(result.diagnostic_code) << '}' << std::endl;
                    continue;
                }
                if (command == "disconnect-caller-one") {
                    const auto current = listener.status();
                    const auto target = std::find_if(current.nodes.begin(), current.nodes.end(),
                        [](const auto& node) { return node.caller_handle == "Caller One"; });
                    require(target != current.nodes.end(), "fixture_disconnect_target");
                    const auto result = listener.disconnect_caller(target->screen_owner);
                    std::cout << "{\"code\":" << static_cast<unsigned>(result.code)
                        << ",\"diagnostic\":" << std::quoted(result.diagnostic_code) << '}' << std::endl;
                    continue;
                }
                if (command == "hold-handoff" || command == "release-handoff" || command == "handoff") {
                    std::lock_guard<std::mutex> lock(handoff_mutex);
                    if (command == "hold-handoff") { hold_handoff = true; handoff_entered = false; }
                    if (command == "release-handoff") { hold_handoff = false; handoff_changed.notify_all(); }
                    std::cout << "{\"held\":" << hold_handoff << ",\"entered\":" << handoff_entered << '}' << std::endl;
                    continue;
                }
                // Fixed disposable fixture credentials, never a shipping command
                // or command-line recovery surface; exercises the real service.
                if(section7_command(command,[&](std::int64_t after){return classic.snapshot(after);},
                    [&](const app::ClassicInformationChange& change){return classic.execute(change);}))continue;
                if(command.rfind("editor ",0)==0) {
                    std::istringstream input(command.substr(7));
                    std::string action;app::AccountId id{};unsigned level{};std::int64_t expected{};
                    input>>action>>id>>level>>expected;
                    const auto account=storage.find_account_by_id(id);
                    require(account && account.value,"fixture_editor_account");
                    app::LocalAccountRequest change;change.account_id=id;change.expected_revision=expected ? expected : account.value->revision;
                    change.expected_policy_revision=storage.security_policy().value->revision;
                    change.name=account.value->display_name.value_or("");change.profile=account.value->profile;
                    change.security_level=static_cast<std::uint16_t>(level);change.purge_allowed=account.value->purge_allowed.value_or(false);
                    if(action=="save") {change.action=app::LocalAccountAction::save_user;change.profile.location="Live editor saved";}
                    else if(action=="deactivate") change.action=app::LocalAccountAction::lock;
                    else if(action=="reactivate") change.action=app::LocalAccountAction::unlock;
                    else if(action=="delete") change.action=app::LocalAccountAction::tombstone;
                    else if(action=="new") {change.action=app::LocalAccountAction::create_user;change.account_id=0;change.name="Editor New";change.expected_revision=1;change.password=change.confirmation="Sfx-test-password-123";}
                    else {std::cout<<"INVALID"<<std::endl;continue;}
                    app::CancellationSource cancellation;const auto result=administration.execute(change,cancellation.token());
                    const auto current=storage.find_account_by_id(id);
                    std::cout<<"{\"ok\":"<<(result.code==app::LocalAccountResultCode::ok)<<",\"last_sysop_protected\":"<<(result.code==app::LocalAccountResultCode::last_sysop_protected)<<",\"committed\":"<<result.committed<<",\"revision\":"<<result.revision
                        <<",\"online\":"<<listener.account_online(id)<<",\"level\":"<<current.value->security_level.value_or(0)<<'}'<<std::endl;
                    continue;
                }
                if(command=="policy-threshold-ten" || command=="policy-threshold-999" ||
                    command=="limits-call-one" || command=="limits-day-one" || command=="limits-sixty-180" || command=="limits-unlimited") {
                    class Publication final:public app::ISecurityPolicyPublication {
                    public:
                        std::function<bool(const app::LocalSecuritySnapshot&)> apply;
                        std::string fingerprint(std::string_view definition) override {
                            auto hash=win::installation::sha256(std::vector<std::uint8_t>(definition.begin(),definition.end()));
                            std::transform(hash.begin(),hash.end(),hash.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});return hash;
                        }
                        app::StorageStatus publish(const app::LocalSecuritySnapshot& value) override {
                            return apply(value) ? app::StorageStatus::success() : app::StorageStatus{app::StorageErrorCode::internal,"fixture_live_policy"};
                        }
                    } publication;
                    publication.apply=[&](const app::LocalSecuritySnapshot& value){config.security_policy_revision=value.policy.revision;config.security_policy_hash=value.policy.policy_hash;return listener.refresh_security_policy(config);};
                    app::LocalSecurityAdministration service(storage,publication,ids,administration_clock,app::LocalAccountAuthority::installation_owner);
                    const auto snapshot=service.snapshot();
                    app::LocalSecurityChange change{snapshot.policy.revision,app::LocalSecurityAction::set_defaults,0,0,10,static_cast<std::uint16_t>(command=="policy-threshold-ten" ? 10 : 999),{},{}};
                    if(command.find("limits-")==0) {
                        const auto level=std::find_if(snapshot.levels.begin(),snapshot.levels.end(),[](const auto& item){return item.level==10;});
                        require(level!=snapshot.levels.end(),"fixture limits level exists");
                        change.action=app::LocalSecurityAction::upsert_level;change.level=10;change.label=level->label;change.description=level->description;
                        change.sysop_threshold=snapshot.policy.sysop_threshold;
                        if(command=="limits-call-one") change.minutes_per_call=1;
                        if(command=="limits-day-one") change.minutes_per_day=1;
                        if(command=="limits-sixty-180") {change.minutes_per_call=60;change.minutes_per_day=180;}
                    }
                    const auto result=service.execute(change);
                    std::cout<<"{\"ok\":"<<(result.code==app::LocalAccountResultCode::ok)<<",\"committed\":"<<result.committed<<'}'<<std::endl;continue;
                }
                if (command == "reset-caller-one" || command == "replace-sy") {
                    app::CancellationSource cancellation;
                    app::LocalAccountRequest request;
                    if (command == "reset-caller-one") {
                        const auto account = storage.find_account_by_name("Caller One");
                        require(account && account.value, "fixture_reset_account");
                        request.action = app::LocalAccountAction::issue_temporary_password;
                        request.account_id = account.value->account_id;
                        request.expected_revision = account.value->revision;
                        request.password = request.confirmation = "Fixture-temporary-789";
                    } else {
                        const auto current = storage.system_password();
                        require(current && current.value, "fixture_SY_revision");
                        request.action = app::LocalAccountAction::replace_system_password;
                        request.expected_revision = current.value->revision;
                        request.password = request.confirmation = "Fixture-new-system-789";
                    }
                    const auto mutation = administration.execute(request, cancellation.token());
                    require(request.password.empty() && request.confirmation.empty(), "fixture_admin_secret_erasure");
                    std::cout << "{\"succeeded\":" << (mutation.code == app::LocalAccountResultCode::ok)
                        << ",\"committed\":" << mutation.committed
                        << ",\"revoked\":" << mutation.sessions_revoked << '}' << std::endl;
                    continue;
                }
                if (command.rfind("local ", 0) == 0) {
                    std::istringstream input(command.substr(6));
                    unsigned number = 0;
                    std::string extra;
                    if (!(input >> number) || (input >> extra) || number < 1 || number > config.logical_nodes) {
                        std::cout << "INVALID" << std::endl; continue;
                    }
                    const auto result = listener.start_local_session(static_cast<std::uint16_t>(number));
                    if (result.response.code == app::LocalControlResponseCode::ok)
                        local_reservations[number - 1U] = result.reservation;
                    std::cout << "{\"code\":" << static_cast<unsigned>(result.response.code)
                        << ",\"diagnostic\":" << std::quoted(result.response.diagnostic_code)
                        << ",\"host\":" << std::quoted(result.host) << ",\"port\":" << result.port
                        << ",\"node\":" << result.reservation.node
                        << ",\"generation\":" << result.reservation.generation
                        << ",\"timeout\":" << result.timeout_milliseconds << '}';
                    std::cout << std::endl; continue;
                }
                if (command.rfind("cancel ", 0) == 0) {
                    std::istringstream input(command.substr(7));
                    unsigned number = 0;
                    std::string extra;
                    if (!(input >> number) || (input >> extra) || number < 1 || number > config.logical_nodes ||
                        !local_reservations[number - 1U]) {
                        std::cout << "INVALID" << std::endl; continue;
                    }
                    const auto result = listener.cancel_local_session({
                        *local_reservations[number - 1U],
                        app::AlphaLocalSessionCancelReason::client_launch_failed});
                    std::cout << "{\"code\":" << static_cast<unsigned>(result.code)
                        << ",\"diagnostic\":" << std::quoted(result.diagnostic_code) << '}';
                    std::cout << std::endl; continue;
                }
                if (command.rfind("reconfigure ", 0) == 0) {
                    std::istringstream input(command.substr(12));
                    unsigned port = 0;
                    std::string extra;
                    if (!(input >> port) || (input >> extra) ||
                        port <= 1024 || port > 65535 || port == config.ssh.port) {
                        std::cout << "INVALID" << std::endl; continue;
                    }
                    auto replacement = config;
                    replacement.telnet.port = static_cast<std::uint16_t>(port);
                    std::string reconfigure_diagnostic;
                    const auto started = std::chrono::steady_clock::now();
                    const bool changed = listener.reconfigure_listener_endpoints(
                        replacement, reconfigure_diagnostic);
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started).count();
                    if (changed) config = replacement;
                    const auto state = listener.status();
                    std::cout << "{\"changed\":" << changed
                        << ",\"diagnostic\":" << std::quoted(reconfigure_diagnostic)
                        << ",\"elapsed_ms\":" << elapsed
                        << ",\"listening\":" << state.listening
                        << ",\"port\":" << state.port << '}' << std::endl;
                    continue;
                }
                if (command == "afk-two") {
                    auto replacement = config;
                    replacement.caller_afk_minutes = 2;
                    ++replacement.configuration_revision;
                    std::string detail;
                    const bool changed = listener.reconfigure_live_settings(replacement, detail);
                    if (changed) config = replacement;
                    std::cout << "{\"changed\":" << changed << '}' << std::endl;
                    continue;
                }
                if(command=="login-afk-thirty") {
                    auto replacement=config;replacement.login_afk_seconds=30;++replacement.configuration_revision;
                    std::string detail;
                    const bool changed=listener.reconfigure_live_settings(replacement,detail);
                    if(changed) config=replacement;
                    std::cout<<"{\"changed\":"<<changed<<'}'<<std::endl;continue;
                }
                if (command == "remove-offline") {
                    const auto asset = root / "Display" / "nonodes.asc";
                    require(fs::remove(asset), "fixture_remove_exact_offline_asset");
                    std::cout << "{\"changed\":1}" << std::endl;
                    continue;
                }
                if (command == "custom-offline" || command == "invalid-offline") {
                    const auto display = root / "Display";
                    if (!fs::exists(display)) {
                        require(fs::create_directory(display), "fixture_notice_directory");
                        require(protection.apply(display, win::StorageDaclObjectKind::directory).succeeded(),
                            "fixture_notice_protection");
                    }
                    const auto asset = display / "nonodes.asc";
                    { std::ofstream out(asset, std::ios::binary | std::ios::trunc);
                      out << (command == "custom-offline" ? "Custom offline notice\r\n" : "\x1b]0;invalid\a");
                      require(out.good(), "fixture_notice_write"); }
                    require(protection.apply(asset, win::StorageDaclObjectKind::file).succeeded(),
                        "fixture_notice_file_protection");
                    std::cout << "{\"changed\":1}" << std::endl;
                    continue;
                }
                if (command == "modem") {
                    const auto state = listener.status();
                    std::cout << '[';
                    bool first = true;
                    for (const auto& node : state.nodes) {
                        if (!first) std::cout << ',';
                        first = false;
                        std::cout << "{\"node\":" << node.number
                            << ",\"connected\":" << node.caller_connected
                            << ",\"encrypted\":" << node.ssh_encrypted
                            << ",\"rx\":" << node.receive_activity
                            << ",\"tx\":" << node.send_activity
                            << ",\"generation\":" << node.assignment_sequence
                            << ",\"caller\":" << std::quoted(node.caller_handle) << '}';
                    }
                    std::cout << ']' << std::endl; continue;
                }
                if (command != "status") { std::cout << "INVALID" << std::endl; continue; }
                const auto status = listener.status();
                const auto resources = rt::alpha_caller_resource_counts();
                std::cout << "{\"active\":" << status.active_connections << ",\"health\":" << static_cast<unsigned>(status.log_health)
                    << ",\"total\":" << status.statistics.successful_logons.total << ",\"workers\":" << resources.workers
                    << ",\"contexts\":" << resources.contexts << ",\"sockets\":" << resources.sockets << ",\"nodes\":[";
                bool first = true;
                for (const auto& node : status.nodes) {
                    if (!first) std::cout << ','; first = false;
                    std::cout << "{\"node\":" << node.number << ",\"generation\":" << node.assignment_sequence
                        << ",\"account_id\":" << node.account_id
                        << ",\"caller\":" << std::quoted(node.caller_handle) << ",\"transport\":" << std::quoted(node.connection)
                        << ",\"activity\":" << std::quoted(node.activity)
                        << ",\"state\":" << static_cast<unsigned>(node.state)
                        << ",\"available\":" << node.available
                        << ",\"source\":" << std::quoted(node.source_ip) << '}';
                }
                std::cout << "],\"events\":["; first = true;
                for (const auto& event : status.logs) {
                    if (!first) std::cout << ','; first = false;
                    std::cout << std::quoted(event.message);
                }
                std::cout << "]}" << std::endl;
            }
            { std::lock_guard<std::mutex> lock(handoff_mutex); hold_handoff = false; handoff_changed.notify_all(); }
            listener.stop();
            revocations.listener = nullptr;
            require(listener.status().diagnostic_code != "caller_cleanup_failed", "fixture_authoritative_cleanup");
        }
        if (feed) feed->stop();
        operational->flush(); durable->stop(); durable.reset(); backend.reset();
        require(storage.shutdown().succeeded(), "fixture_storage_shutdown"); kdf.shutdown();
        rt::close_alpha_hostname_process();
        const auto resources = rt::alpha_caller_resource_counts();
        require(!resources.workers && !resources.contexts && !resources.sockets && !rt::AlphaSshBackend::statistics().live_sessions &&
            !rt::alpha_hostname_worker_count(), "fixture_leaked_resources");
        std::cout << "STOPPED clean" << std::endl; return 0;
    } catch (const std::exception& error) { std::cout << "FAILED " << error.what() << std::endl; return 1; }
}
