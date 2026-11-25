#include "sfx/application/local_control_protocol.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/offline_account_administration.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace app = sfx::application;
namespace win32 = sfx::platform::win32;

std::size_t assertions = 0;

void require(const bool condition, const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}

void object_identity_and_offline_guard(const std::filesystem::path& root) {
    const auto names = win32::installation_object_names(root);
    std::wstring upper = root.native();
    for (auto& value : upper) value = static_cast<wchar_t>(towupper(value));
    const auto upper_names = win32::installation_object_names(upper);
    const auto independent = win32::installation_object_names(
        std::filesystem::path(root.native() + L"-independent"));
    require(names.pipe_name == upper_names.pipe_name &&
            names.mutex_name == upper_names.mutex_name &&
            names.configuration_pipe_name == upper_names.configuration_pipe_name &&
            names.configuration_mutex_name == upper_names.configuration_mutex_name,
        "all installation object names are path-case stable");
    require(names.pipe_name != names.configuration_pipe_name &&
            names.mutex_name != names.configuration_mutex_name,
        "Runtime ownership and Configuration editor objects are distinct");
    require(names.configuration_pipe_name != independent.configuration_pipe_name &&
            names.configuration_mutex_name != independent.configuration_mutex_name,
        "different installation roots remain independent");

    const auto owner = win32::current_process_user_sid_string();
    require(!owner.empty(), "current Windows owner SID is available");
    {
        win32::OfflineInstallationGuard offline;
        win32::OfflineInstallationGuard duplicate;
        require(offline.acquire(root, owner).succeeded() && offline.acquired(),
            "first offline configuration transaction acquires the shared Runtime mutex");
        require(!duplicate.acquire(root, owner).succeeded(),
            "a second offline configuration transaction is refused");
        win32::RuntimeInstanceGuard runtime;
        require(!runtime.acquire(root, owner).succeeded(),
            "Runtime startup is refused while an offline transaction owns the installation");
    }
    win32::OfflineInstallationGuard reopened;
    require(reopened.acquire(root, owner).succeeded(),
        "the offline transaction guard is released for a later operation");
}

void same_owner_configuration_pipe(const std::filesystem::path& root) {
    const auto names = win32::installation_object_names(root);
    const auto owner = win32::current_process_user_sid_string();
    std::atomic<bool> authenticated{false};
    std::atomic<bool> exchanged{false};
    std::string diagnostic;
    std::thread server([&] {
        auto connection = win32::accept_local_control_client(
            names.configuration_pipe_name, owner, 0, 2000, &diagnostic);
        authenticated.store(connection.valid());
        app::LocalControlFrame request;
        if (!connection.read_frame(request) ||
            request.type != app::LocalControlMessageType::configuration_snapshot_request ||
            request.request_id != 77 || !request.payload.empty()) return;
        app::LocalControlFrame response;
        response.type = app::LocalControlMessageType::configuration_snapshot_response;
        response.request_id = request.request_id;
        response.payload = {1U};
        exchanged.store(connection.write_frame(response));
    });
    Sleep(250);
    auto client = win32::connect_local_control_server(
        names.configuration_pipe_name, GetCurrentProcessId(), 2000);
    const bool client_valid = client.valid();
    app::LocalControlFrame request;
    request.type = app::LocalControlMessageType::configuration_snapshot_request;
    request.request_id = 77;
    const bool request_written = client_valid && client.write_frame(request);
    app::LocalControlFrame response;
    const bool response_read = request_written && client.read_frame(response) &&
            response.type == app::LocalControlMessageType::configuration_snapshot_response &&
            response.request_id == request.request_id &&
            response.payload == std::vector<std::uint8_t>{1U};
    client.disconnect();
    server.join();
    if (!client_valid)
        throw std::runtime_error("same-owner configuration client authentication failed: " +
            diagnostic);
    ++assertions;
    require(request_written,
        "configuration client writes one bounded framed request");
    require(response_read,
        "configuration client receives only its exact correlated framed response");
    require(authenticated.load() && exchanged.load() &&
            diagnostic == "pipe_client_authenticated",
        "configuration server authenticates the owner SID before exchange");
}

void account_admission_refusals(const std::filesystem::path& root) {
    const auto owner = win32::current_process_user_sid_string();
    const auto snapshot = win32::offline_account_snapshot(root,owner,{});
    require(snapshot.code != app::LocalAccountResultCode::ok && snapshot.accounts.empty(),
        "unverified non-installation cannot expose accounts or create a database");
    require(!std::filesystem::exists(root / L"Data" / L"SFX.DB"), "refused inspection creates no database");
    app::LocalAccountRequest command;
    command.action = app::LocalAccountAction::replace_system_password; command.expected_revision = 1;
    command.password = "fixture-secret"; command.confirmation = command.password;
    require(win32::offline_administer_account(root,L"S-1-5-18",command).code != app::LocalAccountResultCode::ok &&
        command.password.empty() && command.confirmation.empty(), "wrong installation owner refused and secrets erased");
    command.action = app::LocalAccountAction::issue_temporary_password;
    command.password = "fixture-secret"; command.confirmation = command.password;
    require(win32::elevated_replace_system_password(root,command).code == app::LocalAccountResultCode::unauthorized &&
        command.password.empty() && command.confirmation.empty(), "elevated recovery can never become account-reset authority");
    const auto recovery = win32::elevated_system_password_snapshot(root);
    require(recovery.code != app::LocalAccountResultCode::ok && recovery.accounts.empty(),
        "unverified or ordinary recovery entry discloses no security/account state");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 2 || (argc == 3 && std::wstring_view(argv[2]) == L"--inspect-scheduler"),
            "usage: cr3_configuration_platform_tests <fixture-root> [--inspect-scheduler]");
        const std::filesystem::path root(argv[1]);
        if (argc == 3) {
            const auto owner = win32::current_process_user_sid_string();
            const auto configuration = win32::read_scheduler_configuration_file(root, owner);
            const auto state = win32::read_scheduler_state_file(root, owner);
            std::cout << "scheduler_configuration=" << configuration.diagnostic_code
                      << ";scheduler_state=" << state.diagnostic_code;
            if (configuration.value) {
                std::cout << ";revision=" << configuration.value->revision
                          << ";time=" << static_cast<unsigned>(configuration.value->local_hour)
                          << ':' << static_cast<unsigned>(configuration.value->local_minute);
            }
            if (state.value) {
                std::cout << ";active=" << state.value->active
                          << ";last_result=" << static_cast<unsigned>(state.value->last_result)
                          << ";history=" << state.value->history.size();
            }
            std::cout << '\n';
            return configuration.value && state.value ? 0 : 3;
        }
        object_identity_and_offline_guard(root);
        same_owner_configuration_pipe(root);
        account_admission_refusals(root);
        std::cout << "PASS CR-3 configuration platform: object identity, offline lock, owner pipe; assertions="
                  << assertions << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CR-3 configuration platform failure: " << error.what() << '\n';
        return 1;
    }
}
