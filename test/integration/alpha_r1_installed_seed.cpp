// One-use preparation for the owner-approved B91E disposable installation only.
// Normal-user mutable content operations; no UI, elevation, program/receipt or
// structural permission changes. Never accepts an installation target argument.
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/storage_path_policy.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>

namespace win = sfx::platform::win32;
namespace install = win::installation;
namespace app = sfx::application;
namespace fs = std::filesystem;
int main() {
    try {
        const fs::path root = L"C:\\xtest-sfx-v002-r1-20260909-B91E";
        const fs::path source = (fs::path(SFX_R1_JOURNAL_SCRATCH) / L"r1-journal-1788996300486168600").make_preferred();
        install::require(!install::elevated(), "ordinary_fixture_only");
        const auto owner = install::operator_sid();
        install::verify_runtime_installation(root, owner);
        win::RuntimeInstanceGuard guard;
        install::require(guard.acquire(root, owner).succeeded(), "fixture_must_be_stopped");
        install::require(!fs::exists(root / L"Data/SFX.DB") && !fs::exists(root / L"Config/SFX.conf"),
            "fresh_test_content_only");
        for (const auto& entry : fs::directory_iterator(root / L"Data"))
            install::require(entry.path().filename() == L"SFXRuntime.lock", "empty_target_data_except_owned_guard");
        install::require(fs::is_empty(root / L"Logs"), "empty_target_logs");
        const auto empty_joker = install::read_file((root / L"Config/JOKER.DAT").make_preferred(), 65536);
        install::require(empty_joker.empty(), "fresh_installer_empty_joker");
        install::require((!fs::exists(source / L"Data/SFX.DB-wal") || fs::file_size(source / L"Data/SFX.DB-wal") == 0) &&
            !fs::exists(source / L"Data/SFX.DB-journal"),
            "source_must_be_cleanly_closed");
        const auto marker = install::read_file(source / L"fixture.marker", 256);
        install::require(std::string(marker.begin(), marker.end()) == "R1 journal loopback fixture only\r\n", "synthetic_source_only");
        auto descriptor = install::mutable_descriptor(owner);
        win::WindowsStorageDaclPolicy policy(owner);
        const auto copy = [&](const wchar_t* from, const wchar_t* to) {
            const auto destination = (root / to).make_preferred();
            const auto bytes = install::read_file((source / from).make_preferred(), 8U * 1024U * 1024U);
            install::create_file(destination, bytes, descriptor.get());
            install::require(policy.apply(destination, win::StorageDaclObjectKind::file).succeeded(), "private_fixture_content");
            install::require(install::read_file(destination, 8U * 1024U * 1024U) == bytes, "exact_synthetic_copy");
        };
        copy(L"Data/SFX.DB", L"Data/SFX.DB");
        copy(L"Config/ssh_host_ed25519.pem", L"Config/ssh_host_ed25519.pem");
        copy(L"Logs/CALLERS.LOG", L"Logs/CALLERS.LOG");
        copy(L"Logs/HEYSYSOP.LOG", L"Logs/HEYSYSOP.LOG");
        copy(L"Logs/CALLERS.LOG", L"Logs/CALLERS-20260909T000000000Z-11111111111111111111111111111111.LOG");
        copy(L"Logs/HEYSYSOP.LOG", L"Logs/HEYSYSOP-20260909T000000000Z-22222222222222222222222222222222.LOG");
        // Modify the initially empty *mutable file*, without replacing its ACL.
        auto joker = install::open_object((root / L"Config/JOKER.DAT").make_preferred(), false, GENERIC_READ | GENERIC_WRITE | READ_CONTROL);
        const std::string names = "; Disposable R1 maintenance preservation fixture\r\n  Blocked Caller  \r\nCaller One\r\n";
        DWORD written{};
        install::require(WriteFile(joker.get(), names.data(), static_cast<DWORD>(names.size()), &written, nullptr) &&
            written == names.size() && FlushFileBuffers(joker.get()), "fixture_joker_write");
        joker.reset();
        win::WindowsLocalStoragePathPolicy paths(owner);
        sfx::adapters::storage::SqliteStorageSettings settings; settings.data_directory = root / L"Data";
        settings.utc_now = [] { return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count(); };
        sfx::adapters::storage::SqliteStorage storage(settings, paths);
        install::require(storage.start().succeeded(), "validate_copied_database");
        const auto security = storage.security_policy(); const auto first = storage.first_run_state();
        install::require(security && security.value && first && first.value && first.value->complete, "complete_synthetic_setup");
        app::OperationalConfiguration config;
        config.bbs_name = "SFX R1 Journal QA"; config.bbs_time_zone = "Pacific Standard Time";
        config.hostname_lookup = false;
        config.telnet = {true, "127.0.0.1", 54323, false}; config.ssh = {true, "127.0.0.1", 54222};
        config.security_policy_revision = security.value->revision; config.security_policy_hash = security.value->policy_hash;
        install::require(win::publish_operational_configuration_file(root, owner, config).succeeded(), "publish_valid_loopback_configuration");
        install::require(storage.shutdown().succeeded(), "closed_prepared_database");
        install::verify_runtime_installation(root, owner);
        std::cout << "PASS: exact B91E disposable content seeded; fresh empty JOKER verified; "
            "synthetic closed database/key/logs/archives; loopback 54323/54222; no structural change or elevation\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
