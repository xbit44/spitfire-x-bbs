#include "sfx/platform/win32/storage_dacl_policy.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using sfx::platform::win32::StorageDaclObjectKind;
using sfx::platform::win32::WindowsStorageDaclPolicy;

void require(const bool condition, const char* const message) {
    if (!condition) throw std::runtime_error(message);
}

void create_file(const std::filesystem::path& path) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    require(stream.good(), "create DACL qualification artifact");
    stream << "SFX Stage 5A DACL qualification\n";
    require(stream.good(), "write DACL qualification artifact");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "DACL tests require an output directory");
        const std::filesystem::path root = std::filesystem::absolute(argv[1]);
        std::error_code error;
        std::filesystem::remove_all(root, error);
        require(!error, "remove previous DACL qualification directory");
        std::filesystem::create_directories(root);

        const std::wstring runtime_sid =
            sfx::platform::win32::current_process_user_sid_string();
        require(!runtime_sid.empty(), "resolve current Desktop qualification identity SID");
        const WindowsStorageDaclPolicy policy(runtime_sid);
        require(policy.apply(root, StorageDaclObjectKind::directory).succeeded(),
                "replace inherited storage-directory DACL");
        const auto root_assessment = policy.assess(root, StorageDaclObjectKind::directory);
        require(root_assessment.status && root_assessment.private_access &&
                    root_assessment.effective_access,
                "storage-directory owner and DACL are exact and protected");

        const std::filesystem::path backup_directory = root / "backups";
        std::filesystem::create_directories(backup_directory);
        require(policy.apply(backup_directory, StorageDaclObjectKind::directory).succeeded(),
                "protect backup directory");
        const auto backup_assessment =
            policy.assess(backup_directory, StorageDaclObjectKind::directory);
        require(backup_assessment.status && backup_assessment.private_access &&
                    backup_assessment.effective_access,
                "backup-directory owner and DACL are exact and protected");

        const std::filesystem::path artifacts[] = {
            root / "SFX.DB",
            root / "SFX.DB",
            root / "SFX.DB-wal",
            root / "SFX.DB-shm",
            backup_directory / "SFX-pre-migration.sqlite3",
            backup_directory / "SFX-r5-cold-capture.manifest",
        };
        for (const auto& artifact : artifacts) {
            create_file(artifact);
            require(policy.apply(artifact, StorageDaclObjectKind::file).succeeded(),
                    "protect database, sidecar, or backup artifact");
            const auto assessment = policy.assess(artifact, StorageDaclObjectKind::file);
            require(assessment.status && assessment.private_access &&
                        assessment.effective_access,
                    "artifact owner and DACL are exact and protected");
        }

        require(
            !WindowsStorageDaclPolicy(L"S-1-5-19")
                 .apply(artifacts[0], StorageDaclObjectKind::file)
                 .succeeded(),
            "LocalService cannot be selected as runtime identity");
        require(!WindowsStorageDaclPolicy(L"S-1-1-0")
                     .apply(artifacts[0], StorageDaclObjectKind::file)
                     .succeeded(),
                "Everyone cannot be selected as runtime identity");
        require(!WindowsStorageDaclPolicy(L"S-1-5-11")
                     .apply(artifacts[0], StorageDaclObjectKind::file)
                     .succeeded(),
                "Authenticated Users cannot be selected as runtime identity");
        require(!WindowsStorageDaclPolicy(L"S-1-5-32-544")
                     .apply(artifacts[0], StorageDaclObjectKind::file)
                     .succeeded(),
                "Administrators group cannot be selected as runtime identity");
        require(!WindowsStorageDaclPolicy(L"not-a-sid")
                     .apply(artifacts[0], StorageDaclObjectKind::file)
                     .succeeded(),
                "malformed runtime SID fails closed");

        std::filesystem::remove_all(root, error);
        require(!error, "remove DACL qualification directory");
        std::cout << "SFX Stage 5A Win32 identity-aware DACL qualification passed\n";
        std::wcout << L"qualified_runtime_sid=" << runtime_sid
                   << L" principals=runtime,SYSTEM,BUILTIN\\Administrators"
                   << L" runtime_rights=modify system_admin_rights=full"
                   << L" protected=yes ordinary_users=absent\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "SFX Stage 5A Win32 DACL qualification failure: "
                  << exception.what() << '\n';
        return 1;
    }
}
