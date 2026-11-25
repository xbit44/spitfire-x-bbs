#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/control_panel_metrics.hpp"
#include "installed_qualification_fixture.hpp"

#include <filesystem>
#include <fstream>
#include <chrono>
#include <atomic>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

namespace app = sfx::application;
namespace win = sfx::platform::win32;

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

app::OperationalConfiguration configuration(const std::string& name) {
    app::OperationalConfiguration value;
    value.bbs_name = name;
    value.bbs_time_zone = "Pacific Standard Time";
    value.logical_nodes = 4;
    value.security_policy_revision = 1;
    value.security_policy_hash =
        "71cb5a767e365786bbedaa779e4dc743473fb70c2b9dd3a99e4f5ce154062a95";
    return value;
}

void qualify_node_controls(const std::filesystem::path& output, const std::wstring& sid) {
    const app::NodeEnablement all_on = app::default_node_enablement(), all_off{};
    const auto create_controls_fixture = [&sid](const std::filesystem::path& root) {
        // Atomic mutable-file tests only: this is deliberately NOT an installed
        // SFX root and makes no assertion about Setup's protected receipt/tree.
        require(!std::filesystem::exists(root), "node controls fixture must be new");
        require(std::filesystem::create_directories(root / L"Config"), "create new controls fixture");
        win::WindowsStorageDaclPolicy policy(sid);
        require(policy.apply(root / L"Config", win::StorageDaclObjectKind::directory).succeeded(),
            "protect exact fixture configuration directory");
    };
    const auto write = [](const std::filesystem::path& path, const std::string& text) {
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        require(file != INVALID_HANDLE_VALUE, "open bounded node-state corruption fixture");
        DWORD written = 0;
        const bool okay = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
            written == text.size();
        CloseHandle(file);
        require(okay, "write bounded node-state fixture");
    };
    for (int legacy = 0; legacy < 3; ++legacy) {
        const auto root = output / ("controls-upgrade-" + std::to_string(legacy));
        create_controls_fixture(root);
        if (legacy) require(win::publish_node_one_enabled(root, sid, legacy == 1).succeeded(),
            "publish protected legacy state");
        const auto loaded = win::load_alpha_node_controls(root, sid);
        require(loaded && loaded.value && *loaded.value == (legacy == 2 ? all_off : all_on),
            "missing/enabled legacy maps all on; old disabled stop-all maps all off");
        if (legacy) require(win::read_node_one_enabled(root) == (legacy == 1), "legacy rollback file retained unchanged");
        win::WindowsStorageDaclPolicy policy(sid);
        const auto state = root / L"Config" / L"SFXNodes.state";
        require(policy.assess(state, win::StorageDaclObjectKind::file).status.succeeded(),
            "new controls have exact protected owner and permissions");
        for (unsigned mask = 0; mask < 16; ++mask) {
            app::NodeEnablement flags{};
            for (unsigned index = 0; index < 4; ++index) flags[index] = (mask & (1U << index)) != 0;
            require(win::publish_alpha_node_controls(root, sid, flags).succeeded(), "publish all 16 per-node combinations");
            const auto reread = win::load_alpha_node_controls(root, sid);
            require(reread && reread.value && *reread.value == flags, "exact complete per-node persistence round trip");
            require(policy.assess(state, win::StorageDaclObjectKind::file).status.succeeded(),
                "replacement retains exact protected file policy");
        }
        auto high = all_on;
        for (std::size_t index = 0; index < high.size(); ++index) high[index] = index % 2 == 0;
        require(win::publish_alpha_node_controls(root, sid, high).succeeded() &&
            *win::load_alpha_node_controls(root, sid).value == high,
            "128 canonical enablement fields round trip including disabled node 128");
        const std::string legacy_v2 = "SFX_NODE_CONTROL=2\nnode1_enabled=0\nnode2_enabled=1\nnode3_enabled=0\nnode4_enabled=1\n";
        write(state, legacy_v2);
        const auto old_four = win::load_alpha_node_controls(root, sid);
        auto expected_old = all_on; expected_old[0] = expected_old[2] = false;
        require(old_four && old_four.value && *old_four.value == expected_old,
            "strict legacy four-node state preserves old choices and defaults new nodes enabled");
        for (const auto* invalid : {"", "SFX_NODE_CONTROL=3\n", "SFX_NODE_CONTROL=2\nnode1_enabled=1\n",
            "SFX_NODE_CONTROL=2\nnode1_enabled=1\nnode2_enabled=1\nnode3_enabled=1\nnode4_enabled=2\n",
            "SFX_NODE_CONTROL=2\nnode1_enabled=1\nnode2_enabled=1\nnode3_enabled=1\nnode4_enabled=1\nextra\n"}) {
            write(state, invalid);
            require(!win::load_alpha_node_controls(root, sid), "malformed state never enables default nodes");
            require(win::publish_alpha_node_controls(root, sid, all_on).succeeded(), "restore only disposable malformed fixture");
        }
        const auto pending = root / L"Config" / L"SFXNodes.state.writing";
        write(pending, "retained interrupted publication");
        require(!win::publish_alpha_node_controls(root, sid, all_off), "existing staging object is never truncated or followed");
        require(*win::load_alpha_node_controls(root, sid).value == all_on, "failed publication leaves old complete state authoritative");
        require(DeleteFileW(pending.c_str()), "remove bounded fixture staging obstruction");
        const HANDLE locked = CreateFileW(state.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        require(locked != INVALID_HANDLE_VALUE, "lock isolated node state");
        const auto unreadable = win::load_alpha_node_controls(root, sid);
        CloseHandle(locked);
        require(!unreadable, "unreadable state fails closed, not missing/default");
    }
    const auto bad_legacy = output / "controls-malformed-legacy";
    create_controls_fixture(bad_legacy);
    require(win::publish_node_one_enabled(bad_legacy, sid, true).succeeded(), "protect legacy fixture");
    write(bad_legacy / L"Config" / L"SFXNode1.state", "invalid legacy state");
    require(!win::load_alpha_node_controls(bad_legacy, sid) &&
        !std::filesystem::exists(bad_legacy / L"Config" / L"SFXNodes.state"), "malformed legacy is retained and upgrade fails closed");
    std::cout << "PASS node controls: 3 upgrades, 48 protected combinations, 15 malformed records, 3 staging obstructions, 3 unreadable files, malformed legacy\n";
}

void qualify_security_snapshot(const std::filesystem::path& output,const std::wstring& sid) {
    require(!std::filesystem::exists(output),"new security-snapshot evidence root required");
    require(std::filesystem::create_directories(output / L"Config"),"create private snapshot fixture");
    win::WindowsStorageDaclPolicy protection(sid);
    require(protection.apply(output / L"Config",win::StorageDaclObjectKind::directory).succeeded(),"protect fixture config");
    auto configured = configuration("Security snapshot fixture");
    require(win::publish_operational_configuration_file(output,sid,configured).succeeded(),"publish fixture configuration");
    app::LocalSecuritySnapshot truth{app::LocalAccountResultCode::ok,
        {1,10,999,configured.security_policy_hash,1,configured.security_policy_hash},
        {{10,"Caller",{},app::SecurityLevelState::active},{999,"Sysop",{},app::SecurityLevelState::active}}};
    require(win::reconcile_security_snapshot(output,sid,truth,configured).succeeded() && configured.configuration_revision == 1,
        "missing catalog snapshot regenerates from accepted database policy without changing configuration revision");
    const auto file = output / L"Config" / L"SFXSecurity.conf";
    require(protection.assess(file,win::StorageDaclObjectKind::file).status.succeeded(),"catalog snapshot protected");
    require(win::reconcile_security_snapshot(output,sid,truth,configured).succeeded(),"matching publication is idempotent");
    // Model a committed DB policy followed by interrupted filesystem publication.
    truth.policy.revision = 2; truth.policy.published_revision = 2;
    truth.policy.policy_hash = std::string(64,'b'); truth.policy.published_policy_hash = truth.policy.policy_hash;
    truth.policy.new_user_level = 20;
    truth.levels.insert(truth.levels.begin()+1,{20,"New Caller",{},app::SecurityLevelState::active});
    const auto blocker_path = output / L"Config" / L"SFX.conf";
    const HANDLE blocker = CreateFileW(blocker_path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    require(blocker != INVALID_HANDLE_VALUE,"retain original configuration against replacement");
    const auto interrupted = win::reconcile_security_snapshot(output,sid,truth,configured);
    CloseHandle(blocker);
    require(!interrupted && configured.security_policy_revision == 1,"snapshot binding failure is not reported as complete");
    require(win::reconcile_security_snapshot(output,sid,truth,configured).succeeded() &&
        configured.security_policy_revision == 2 && configured.configuration_revision == 2 &&
        configured.security_policy_hash == truth.policy.policy_hash,"retry restores exact database binding after publication interruption");
    require(win::read_operational_configuration_file(output).configuration->security_policy_revision == 2,
        "regenerated binding persists across reload");
    auto future = configured; ++future.security_policy_revision;
    require(!win::reconcile_security_snapshot(output,sid,truth,future),"future configuration revision fails closed");
    auto wrong_hash = configured; wrong_hash.security_policy_hash.assign(64,'c');
    require(!win::reconcile_security_snapshot(output,sid,truth,wrong_hash),"same-revision unexplained hash mismatch refuses");
    {
        std::ofstream changed(file,std::ios::app|std::ios::binary); changed << "unexplained=true\n";
        require(changed.good(),"create exact same-revision negative fixture");
    }
    require(!win::reconcile_security_snapshot(output,sid,truth,configured),"same-revision catalog content change is not silently merged or repaired");
    std::cout << "PASS security snapshot: missing, matching, interrupted/stale recovery, exact DACL, future/hash/content refusal\n";
}

}  // namespace

void qualify_maintenance_files(const std::filesystem::path& root){
    require(!std::filesystem::exists(root),"fresh isolated maintenance-file root required");
    const auto sid=win::current_process_user_sid_string();require(!sid.empty(),"native maintenance fixture owner");
    win::WindowsStorageDaclPolicy policy(sid);
    for(const auto* role:{L"Config",L"Data"}){
        require(std::filesystem::create_directories(root/role),"new maintenance fixture private role");
        require(policy.apply(root/role,win::StorageDaclObjectKind::directory).succeeded(),"protect only new exact maintenance fixture folder");
    }
    app::SchedulerConfiguration backup;backup.enabled=false;backup.local_minute=17;
    require(win::publish_scheduler_configuration_file(root,sid,backup).succeeded(),"backup file control");
    for(const auto* job:{app::kMessageRetentionJobId,app::kMessageIntegrityJobId}){
        require(win::read_scheduler_configuration_file(root,sid,job).state==win::ConfigurationFileState::missing,"missing maintenance distinguished from invalid");
        auto c=app::default_message_scheduler_configuration(job);auto s=app::default_message_scheduler_state(job);
        require(win::publish_scheduler_configuration_file(root,sid,c).succeeded() && win::publish_scheduler_state_file(root,sid,s).succeeded(),"distinct protected defaults publication");
        c.enabled=false;c.revision=3;c.local_hour=23;c.local_minute=57;s.configuration_revision=3;
        if(c.job_id==app::kMessageIntegrityJobId)s.requires_fresh_check=true;
        require(win::publish_scheduler_configuration_file(root,sid,c).succeeded() && win::publish_scheduler_state_file(root,sid,s).succeeded(),"saved maintenance state atomic publication");
        const auto loaded=win::read_scheduler_configuration_file(root,sid,job);const auto state=win::read_scheduler_state_file(root,sid,job);
        require(loaded.value && state.value && app::serialize_scheduler_configuration(*loaded.value)==app::serialize_scheduler_configuration(c) && app::serialize_scheduler_state(*state.value)==app::serialize_scheduler_state(s),"Off/custom time/revision/check guard preserved exactly");
    }
    require(win::read_scheduler_configuration_file(root,sid,"../other").state==win::ConfigurationFileState::invalid,"closed job paths never arbitrary traversal");
    require(app::serialize_scheduler_configuration(*win::read_scheduler_configuration_file(root,sid).value)==app::serialize_scheduler_configuration(backup),"maintenance leaves backup settings/file exact");
    const auto integrity_configuration=*win::read_scheduler_configuration_file(root,sid,app::kMessageIntegrityJobId).value;
    auto prior_clean=*win::read_scheduler_state_file(root,sid,app::kMessageIntegrityJobId).value;prior_clean.requires_fresh_check=false;
    require(win::publish_scheduler_state_file(root,sid,prior_clean).succeeded(),"isolated prior clean result fixture");
    require(win::invalidate_restored_message_verification(root,sid).succeeded(),"restored old clean result cannot clear current verification prerequisite");
    const auto restored=*win::read_scheduler_state_file(root,sid,app::kMessageIntegrityJobId).value;
    prior_clean.requires_fresh_check=true;
    require(restored.requires_fresh_check && app::serialize_scheduler_state(restored)==app::serialize_scheduler_state(prior_clean) &&
        app::serialize_scheduler_configuration(*win::read_scheduler_configuration_file(root,sid,app::kMessageIntegrityJobId).value)==app::serialize_scheduler_configuration(integrity_configuration),"restore prerequisite preserves Off/custom day/time/revision/history exactly");
    std::cout << "PASS isolated native maintenance file protection/publication; NOT installed Setup/Runtime qualification\n";
}

int main(int argc, char** argv) {
    try {
        if(argc==3 && std::string(argv[1])=="--maintenance-files-only"){
            qualify_maintenance_files(std::filesystem::absolute(argv[2]));return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--security-snapshot-only") {
            qualify_security_snapshot(std::filesystem::absolute(argv[2]),win::current_process_user_sid_string());
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--node-controls-only") {
            const auto output = std::filesystem::absolute(argv[2]);
            require(!std::filesystem::exists(output), "new node-controls evidence directory required");
            std::filesystem::create_directories(output);
            qualify_node_controls(output, win::current_process_user_sid_string());
            return 0; // preserve evidence; no installed SFX root or receipt created
        }
        require(argc == 2, "Alpha platform tests require an output directory");
        const std::filesystem::path output = std::filesystem::absolute(argv[1]);
        require(!std::filesystem::exists(output), "new retained platform evidence directory required");
        std::filesystem::create_directories(output);
        sfx::test::InstalledQualificationFixture fixture(output / "installed-cases");
        const auto root = fixture.next("platform");
        const std::wstring sid = win::current_process_user_sid_string();
        require(!sid.empty(), "resolve Desktop installation owner SID");
        qualify_node_controls(output, sid);

        const auto provisioned = win::provision_alpha_installation_tree(root, sid);
        if (!provisioned) throw std::runtime_error(provisioned.operation);
        constexpr const wchar_t* leaves[] = {
            L"Bin", L"Config", L"Data", L"Display", L"Files", L"Doors",
            L"Backup", L"Logs", L"Work"};
        for (const auto* leaf : leaves) {
            require(std::filesystem::is_directory(root / leaf),
                "required installation directory exists");
        }
        win::WindowsStorageDaclPolicy storage_policy(sid);
        const auto structural = win::installation::structural_descriptor(sid);
        const auto root_handle = win::installation::open_object(root, true);
        require(win::installation::exact_security(root_handle.get(), structural.get()),
            "Setup root has exact protected structural DACL, not mutable-storage DACL");
        require(win::assess_alpha_bin_directory(root, sid).succeeded(),
            "Bin has exact protected owner read-execute DACL");
        for (const auto* leaf : {L"Config", L"Data", L"Display", L"Files", L"Doors",
                                 L"Backup", L"Logs", L"Work"}) {
            require(storage_policy.assess(root / leaf,
                    win::StorageDaclObjectKind::directory).status.succeeded(),
                "mutable installation directory has exact protected storage DACL");
        }
        const HANDLE forbidden = CreateFileW((root / L"Bin" / L"runtime-write.tmp").c_str(),
            GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(forbidden == INVALID_HANDLE_VALUE,
            "Desktop runtime identity cannot write into Bin");

        auto first = configuration("Spitfire-X Alpha QA");
        require(win::publish_operational_configuration_file(root, sid, first).succeeded(),
            "publish first strict operational configuration");
        auto loaded = win::read_operational_configuration_file(root);
        require(loaded.state == win::ConfigurationFileState::loaded && loaded.configuration &&
                app::equivalent_operational_configuration(first, *loaded.configuration),
            "read back exact operational configuration");
        auto replacement = configuration("Spitfire-X Alpha Replacement");
        replacement.logical_nodes = 8;
        require(win::publish_operational_configuration_file(root, sid, replacement).succeeded(),
            "atomically replace operational configuration");
        loaded = win::read_operational_configuration_file(root);
        require(loaded.state == win::ConfigurationFileState::loaded && loaded.configuration &&
                app::equivalent_operational_configuration(replacement, *loaded.configuration) &&
                !std::filesystem::exists(root / L"Config" / L"SFX.conf.pending"),
            "replacement is complete and no pending file remains");
        require(win::publish_pending_operational_configuration_file(root, sid, first).succeeded(),
            "stage bounded listener settings separately from active configuration");
        const auto pending = win::read_pending_operational_configuration_file(root);
        require(pending.state == win::ConfigurationFileState::loaded && pending.configuration &&
                app::equivalent_operational_configuration(first, *pending.configuration),
            "read back exact pending settings candidate");
        win::remove_pending_operational_configuration_file(root);
        require(win::read_pending_operational_configuration_file(root).state ==
                win::ConfigurationFileState::missing,
            "pending settings candidate is removable after activation or rollback");

        app::SchedulerConfiguration schedule;
        schedule.local_hour = 2;
        schedule.local_minute = 17;
        require(win::read_scheduler_configuration_file(root, sid).state ==
                    win::ConfigurationFileState::missing &&
                win::publish_scheduler_configuration_file(root, sid, schedule).succeeded(),
            "missing scheduler configuration publishes protected default");
        const auto loaded_schedule = win::read_scheduler_configuration_file(root, sid);
        require(loaded_schedule.state == win::ConfigurationFileState::loaded &&
                    loaded_schedule.value && loaded_schedule.value->revision == 1 &&
                    loaded_schedule.value->local_minute == 17,
            "scheduler configuration exact typed round trip");
        app::SchedulerState scheduler_state;
        scheduler_state.next_run_utc = 2'000'000'000;
        require(win::publish_scheduler_state_file(root, sid, scheduler_state).succeeded(),
            "scheduler state publishes atomically");
        const auto loaded_scheduler_state = win::read_scheduler_state_file(root, sid);
        require(loaded_scheduler_state.state == win::ConfigurationFileState::loaded &&
                    loaded_scheduler_state.value &&
                    loaded_scheduler_state.value->next_run_utc == 2'000'000'000,
            "scheduler state exact typed round trip");
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto next_run = win::next_local_daily_run(now, 2, 17);
        require(next_run && *next_run > now && *next_run <= now + 27 * 60 * 60,
            "daily local schedule resolves to one bounded strictly-future UTC instant");

        app::ControlPanelUiPreferences preferences;
        preferences.left = -20;
        preferences.top = 30;
        preferences.width = 1280;
        preferences.height = 720;
        preferences.system_monitor_visible = true;
        preferences.system_monitor_grayscale = true;
        require(win::publish_control_panel_ui_preferences(root, sid, preferences).succeeded(),
            "publish protected Control Panel UI preferences");
        const auto loaded_preferences = win::read_control_panel_ui_preferences(root);
        require(loaded_preferences && loaded_preferences->left == -20 &&
                loaded_preferences->width == 1280 && loaded_preferences->system_monitor_visible &&
                loaded_preferences->system_monitor_grayscale,
            "Control Panel position, size, monitor visibility and appearance round trip");
        require(win::publish_node_one_enabled(root, sid, false).succeeded() &&
                !win::read_node_one_enabled(root) &&
                win::publish_node_one_enabled(root, sid, true).succeeded() &&
                win::read_node_one_enabled(root),
            "Node 1 enabled state persists explicitly");
        {
            const HANDLE corrupt = CreateFileW((root / L"Config" / L"SFXNode1.state").c_str(),
                GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            require(corrupt != INVALID_HANDLE_VALUE, "open Node 1 state for corruption test");
            constexpr char invalid[] = "invalid-node-state";
            DWORD written = 0;
            const bool write_succeeded = WriteFile(corrupt, invalid,
                static_cast<DWORD>(sizeof(invalid) - 1U), &written, nullptr) != FALSE &&
                written == static_cast<DWORD>(sizeof(invalid) - 1U);
            CloseHandle(corrupt);
            require(write_succeeded && !win::read_node_one_enabled(root),
                "malformed persisted Node 1 state fails closed to disabled");
            require(win::publish_node_one_enabled(root, sid, true).succeeded(),
                "restore explicit enabled state after corruption test");
        }
        require(storage_policy.assess(root / L"Config" / L"SFXControlPanel.ini",
                win::StorageDaclObjectKind::file).status.succeeded() &&
                storage_policy.assess(root / L"Config" / L"SFXNode1.state",
                win::StorageDaclObjectKind::file).status.succeeded() &&
                storage_policy.assess(root / L"Config" / L"SFXScheduler.conf",
                win::StorageDaclObjectKind::file).status.succeeded() &&
                storage_policy.assess(root / L"Data" / L"SFXScheduler.state",
                win::StorageDaclObjectKind::file).status.succeeded(),
            "new Control Panel and scheduler files retain exact protected storage DACLs");

        win::WindowsControlPanelMetricSampler metrics(root);
        (void)metrics.sample();
        Sleep(1100);
        const auto metric_snapshot = metrics.sample();
        require(metric_snapshot.memory_available && metric_snapshot.memory_total_bytes != 0 &&
                metric_snapshot.storage_activity_available &&
                metric_snapshot.storage_activity_percent >= 0.0 &&
                metric_snapshot.storage_read_bytes_per_second >= 0.0 &&
                metric_snapshot.storage_write_bytes_per_second >= 0.0,
            "unprivileged Control Panel sampler reports real memory and aggregate disk activity");

        {
            win::RuntimeInstanceGuard first_guard;
            win::RuntimeInstanceGuard duplicate_guard;
            require(first_guard.acquire(root, sid).succeeded() && first_guard.acquired(),
                "first authoritative runtime acquires both guards");
            require(!duplicate_guard.acquire(root, sid).succeeded(),
                "duplicate authoritative runtime is rejected");
        }
        {
            win::RuntimeInstanceGuard restarted_guard;
            require(restarted_guard.acquire(root, sid).succeeded(),
                "runtime guard is released for orderly restart");
        }

        const auto lower_names = win::installation_object_names(root);
        std::wstring upper_root = root.native();
        for (auto& character : upper_root) character = static_cast<wchar_t>(towupper(character));
        const auto upper_names = win::installation_object_names(upper_root);
        require(lower_names.pipe_name == upper_names.pipe_name &&
                lower_names.mutex_name == upper_names.mutex_name,
            "installation object identity is path-case stable");
        const auto accept_started = std::chrono::steady_clock::now();
        require(!win::accept_local_control_client(lower_names.pipe_name, sid,
                GetCurrentProcessId(), 100).valid(),
            "unconnected local-control server returns without an orphan wait");
        require(std::chrono::steady_clock::now() - accept_started < std::chrono::seconds(2),
            "local-control accept timeout is bounded");

        std::string accept_diagnostic;
        std::atomic<bool> server_authenticated{false};
        std::atomic<bool> server_exchanged{false};
        std::thread server_thread([&] {
            auto server = win::accept_local_control_client(lower_names.pipe_name, sid,
                GetCurrentProcessId(), 2000, &accept_diagnostic);
            server_authenticated.store(server.valid());
            app::LocalControlFrame received_frame;
            if (!server.read_frame(received_frame) ||
                received_frame.type != app::LocalControlMessageType::status_request ||
                received_frame.request_id != 1) return;
            app::LocalControlFrame response_frame;
            response_frame.type = app::LocalControlMessageType::status_response;
            response_frame.request_id = received_frame.request_id;
            response_frame.payload = {1U, 2U, 3U};
            server_exchanged.store(server.write_frame(response_frame));
        });
        Sleep(50);
        auto client = win::connect_local_control_server(lower_names.pipe_name,
            GetCurrentProcessId(), 2000);
        if (!client.valid()) {
            server_thread.join();
            throw std::runtime_error("local-control authentication: " + accept_diagnostic);
        }
        app::LocalControlFrame request_frame;
        request_frame.type = app::LocalControlMessageType::status_request;
        request_frame.request_id = 1;
        require(client.write_frame(request_frame), "client writes one bounded protocol frame");
        app::LocalControlFrame received_response;
        require(client.read_frame(received_response) &&
                received_response.request_id == request_frame.request_id &&
                received_response.payload == std::vector<std::uint8_t>({1U, 2U, 3U}),
            "client receives the exact matching response frame");
        client.disconnect();
        server_thread.join();
        require(server_authenticated.load() && server_exchanged.load(),
            "server authenticates the exact peer and completes the response");

        // Same installation user is insufficient: a replacement process must
        // never acquire the original Desktop owner's control authority.
        std::string replacement_diagnostic;
        std::atomic<bool> replacement_accepted{false};
        std::thread replacement_server([&] {
            auto server = win::accept_local_control_client(lower_names.pipe_name, sid,
                GetCurrentProcessId() ^ 1U, 2000, &replacement_diagnostic);
            replacement_accepted.store(server.valid());
        });
        Sleep(50);
        auto replacement_client = win::connect_local_control_server(lower_names.pipe_name,
            GetCurrentProcessId(), 2000);
        replacement_server.join();
        replacement_client.disconnect();
        require(!replacement_accepted.load() && replacement_diagnostic == "pipe_client_process_mismatch",
            "same-user replacement Control Panel process is actually rejected, not skipped");

        const std::filesystem::path insecure = output / "pre-existing-insecure";
        std::filesystem::create_directory(insecure);
        require(!win::provision_alpha_installation_tree(insecure, sid).succeeded(),
            "insecure pre-existing installation root fails closed");
        fixture.finish();
        std::cout << "SFX Alpha Win32 runtime-platform tests PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SFX Alpha Win32 runtime-platform tests FAIL: " << error.what() << '\n';
        return 1;
    }
}
