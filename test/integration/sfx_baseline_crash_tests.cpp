#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/test/shipping_schema.hpp"
#include "sfx/test/storage_fakes.hpp"
#include "sqlite_internal.hpp"
#include "sqlite_sfx_baseline.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {

namespace app = sfx::application;
namespace internal = sfx::adapters::storage::internal;
namespace storage = sfx::adapters::storage;
namespace test = sfx::test;

std::size_t assertions = 0;

void require(const bool value, const std::string& message) {
    ++assertions;
    if (!value) throw std::runtime_error(message);
}

void remove_tree(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    require(!error, "remove crash-test tree");
}

storage::SqliteStorageSettings settings_for(
    const std::filesystem::path& directory,
    const std::string_view crash_boundary = {}) {
    storage::SqliteStorageSettings settings;
    settings.data_directory = directory;
    settings.utc_now = [] { return std::int64_t{1'788'115'200}; };
    if (!crash_boundary.empty()) {
        settings.bootstrap_boundary_hook =
            [expected = std::string(crash_boundary)](const std::string_view actual) {
                if (actual == expected) std::_Exit(86);
                return app::StorageStatus::success();
            };
    }
    return settings;
}

std::uintmax_t size_or_zero(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) return 0;
    const std::uintmax_t bytes = std::filesystem::file_size(path, error);
    return error ? 0 : bytes;
}

std::string quoted(const std::filesystem::path& value) {
    const std::string text = value.string();
    if (text.find('"') != std::string::npos) {
        throw std::runtime_error("crash-test path contains a quote");
    }
    return '"' + text + '"';
}

std::vector<unsigned char> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("read incompatible database probe");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

int child_main(const std::string_view boundary, const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    test::FakeStoragePathPolicy policy;
    storage::SqliteStorage database(settings_for(directory, boundary), policy);
    if (!database.start().succeeded()) return 11;
    if (!database.shutdown().succeeded()) return 12;
    return 13;
}

void verify_exact_completed_state(const std::filesystem::path& directory) {
    const std::filesystem::path database_file = directory / "SFX.DB";
    internal::SqliteConnection verification(
        database_file, SQLITE_OPEN_READONLY, std::chrono::milliseconds{100}, false);
    require(internal::validate_sfx_shipping_database(verification.get(), true),
        "retry produces the exact complete shipping SFX schema and ledger");
    require(!internal::validate_sfx_baseline_database(verification.get(), true),
        "frozen schema-one validator still rejects an upgraded shipping schema");
    require(verification.scalar_int64(
                "SELECT count(*) FROM sfx_schema_migrations",
                "count crash-retry migration ledger") == sfx::test::shipping_migration_count,
        "retry produces exactly all accepted shipping migration ledger rows");
    require(verification.scalar_int64(
                "PRAGMA application_id", "read crash-retry application ID") ==
            internal::sfx_baseline_application_id &&
            verification.scalar_int64(
                "PRAGMA user_version", "read crash-retry schema version") ==
            sfx::test::shipping_schema_version,
        "retry publishes SFXB and the shipping schema version");
}

void verify_only_sfx_database_remains(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const std::string leaf = entry.path().filename().string();
        if (leaf == "SFX.DB") continue;
        if ((leaf == "SFX.DB-wal" || leaf == "SFX.DB-shm" ||
                leaf == "SFX.DB-journal") && size_or_zero(entry.path()) == 0) {
            continue;
        }
        require(false, "retry leaves no receipt, compatibility, or nonempty sidecar artifact");
    }
}

void run_crash_scenario(
    const std::filesystem::path& executable,
    const std::filesystem::path& root,
    const std::string_view boundary) {
    const std::filesystem::path directory = root / std::string(boundary);
    std::filesystem::create_directories(directory);
    std::string command = quoted(executable) + " --crash-child \"" +
        std::string(boundary) + "\" " + quoted(directory);
#ifdef _WIN32
    command = '"' + command + '"';
#endif
    const int child_result = std::system(command.c_str());
#ifdef _WIN32
    require(child_result == 86,
        "child terminates with the exact injected abrupt-exit code");
#else
    require(child_result != -1 && WIFEXITED(child_result) &&
            WEXITSTATUS(child_result) == 86,
        "child terminates with the exact injected abrupt-exit code");
#endif

    const std::filesystem::path database_file = directory / "SFX.DB";
    const std::uintmax_t database_bytes = size_or_zero(database_file);
    const std::uintmax_t wal_bytes = size_or_zero(database_file.string() + "-wal");
    const std::uintmax_t shm_bytes = size_or_zero(database_file.string() + "-shm");
    const std::uintmax_t journal_bytes = size_or_zero(database_file.string() + "-journal");
    std::cout << "sfx_baseline_crash boundary=" << boundary
              << " database_bytes=" << database_bytes
              << " wal_bytes=" << wal_bytes
              << " shm_bytes=" << shm_bytes
              << " journal_bytes=" << journal_bytes << '\n';

    if (boundary == "before-sfx-database-create") {
        require(!std::filesystem::exists(database_file),
            "pre-create crash leaves the database absent");
    } else {
        require(std::filesystem::exists(database_file),
            "post-create crash retains the exact SFX database leaf");
    }
    if (boundary == "after-sfx-database-create" ||
        boundary == "after-sfx-database-protect") {
        require(database_bytes == 0,
            "pre-SQLite crash retains the exact zero-byte prepublication state");
    }

    test::FakeStoragePathPolicy policy;
    storage::SqliteStorage retry(settings_for(directory), policy);
    require(retry.start().succeeded(),
        "pinned SQLite resolves the abrupt crash state on retry");
    verify_exact_completed_state(directory);
    require(retry.shutdown().succeeded(), "crash retry shuts down cleanly");
    verify_only_sfx_database_remains(directory);
}

void test_unknown_nonempty_database_is_untouched(const std::filesystem::path& root) {
    const std::filesystem::path directory = root / "unknown-nonempty";
    std::filesystem::create_directories(directory);
    const std::filesystem::path database_file = directory / "SFX.DB";
    {
        std::ofstream output(database_file, std::ios::binary | std::ios::trunc);
        const std::array<unsigned char, 17> bytes{
            0x53, 0x46, 0x58, 0x2d, 0x55, 0x4e, 0x4b, 0x4e, 0x4f,
            0x57, 0x4e, 0x2d, 0x53, 0x54, 0x41, 0x54, 0x45};
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        require(output.good(), "write incompatible nonempty SFX database probe");
    }
    const std::vector<unsigned char> before = read_bytes(database_file);
    test::FakeStoragePathPolicy policy;
    storage::SqliteStorage database(settings_for(directory), policy);
    require(!database.start().succeeded(),
        "unknown nonempty SFX database fails closed");
    require(read_bytes(database_file) == before,
        "unknown nonempty SFX database remains byte-for-byte untouched");
    require(database.health().state == app::StorageRuntimeState::recovery_required,
        "unknown nonempty state reports recovery required");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string_view(argv[1]) == "--crash-child") {
            return child_main(argv[2], std::filesystem::absolute(argv[3]));
        }
        require(argc == 2, "SFX baseline crash tests require an output directory");
        const std::filesystem::path root = std::filesystem::absolute(argv[1]);
        remove_tree(root);
        std::filesystem::create_directories(root);
        const std::filesystem::path executable = std::filesystem::absolute(argv[0]);
        const std::array<std::string_view, 9> boundaries{
            "before-sfx-database-create",
            "after-sfx-database-create",
            "after-sfx-database-protect",
            "after-sqlite-initialization",
            "after-baseline-transaction-begin",
            "after-baseline-schema",
            "before-baseline-commit",
            "after-baseline-commit",
            "before-ordinary-connection-shutdown"};
        for (const std::string_view boundary : boundaries) {
            run_crash_scenario(executable, root, boundary);
        }
        test_unknown_nonempty_database_is_untouched(root);
        remove_tree(root);
        std::cout << "all SFX clean-baseline subprocess crash tests passed assertions="
                  << assertions << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SFX clean-baseline crash test failure: " << error.what() << '\n';
        return 1;
    }
}
