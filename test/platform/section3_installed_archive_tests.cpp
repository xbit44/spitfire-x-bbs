// Bounded installed-data backend qualification, NOT GUI automation or a shipping tool.
// The exact disposable identity/images and production offline guard are mandatory.
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/archive/backup_service.hpp"
#include <iostream>

namespace install = sfx::platform::win32::installation;
namespace win32 = sfx::platform::win32;
namespace archive = sfx::archive;
std::string hex_ascii(const std::wstring& text) {
    std::string result;
    for (const wchar_t ch : text) {
        install::require((ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'F'), "invalid hex");
        result += static_cast<char>(ch);
    }
    return result;
}
int wmain(int argc, wchar_t** argv) {
    try {
        const install::Path root = L"C:\\xtest-sfx-v002-installer-cr3-20260914";
        const auto owner = install::operator_sid();
        install::verify_runtime_installation(root, owner);
        const auto receipt = install::read_receipt(root, owner);
        install::require(receipt.id == L"42DFE5EEBF865D9C6B75FF83BEBAB6FA" &&
            receipt.program_count == 5, "wrong disposable receipt");
        const std::array<std::string, 5> expected{{
            "05919BB7BDA61CC7ABB73E3628A27B4A656BC000744C6F2574E6CCD890D37336",
            "E7E78EC7E06EAB420B28B8C17042225D084D9C5A1F3CA7B23450B48E91411CC9",
            "F06FF984FDAA806FB4D4C0C9B371AC88FA420FF5BF89778548E31A9D7A771A08",
            "4381F19CCFF177EDA4DE4AD12C015E0CA73B573E19CA2DCB7D2A4750C341DFB7",
            "B2A463D5DCA4B5BF3706C18E34DF349DCA664BC8D5773648CEAD4145CF508FA0"}};
        install::require(receipt.program_hashes == expected, "installed candidate mismatch");
        win32::OfflineInstallationGuard guard;
        install::require(guard.acquire(root, owner).succeeded(), "offline transaction unavailable");
        win32::WindowsStorageDaclPolicy policy(owner);
        const auto id = install::unique_id();
        const auto transaction = hex_ascii(id);
        if (argc == 2 && std::wstring(argv[1]) == L"backup") {
            archive::ManagedBackupRequest request;
            request.installation_root = root;
            request.transaction_id = transaction;
            request.source_generation = std::to_string(receipt.generation);
            request.created = archive::current_timestamp();
            const auto saved = archive::create_managed_backup(request);
            install::require(saved.result.ok(), saved.result.detail.c_str());
            install::require(policy.apply(saved.archive_path, win32::StorageDaclObjectKind::file).succeeded(),
                "backup security failed");
            install::require(archive::inspect_backup(saved.archive_path).result.ok(), "backup verification failed");
            std::wcout << L"BACKUP PASS " << saved.archive_path.native() << L'\n';
            std::cout << "SHA256 " << install::file_sha256(saved.archive_path) << '\n';
        } else if (argc == 4 && std::wstring(argv[1]) == L"restore") {
            const install::Path leaf(argv[2]);
            install::require(leaf == leaf.filename() && leaf.extension() == L".zip" &&
                leaf.native().rfind(L"SFX-", 0) == 0, "restore leaf invalid");
            archive::RestoreRequest request;
            request.installation_root = root;
            request.archive_path = root / L"Backup" / leaf;
            const std::wstring digest(argv[3]);
            install::require(digest.size() == 64 && install::file_sha256(request.archive_path) ==
                hex_ascii(digest), "restore archive hash mismatch");
            request.transaction_id = transaction;
            request.source_generation = std::to_string(receipt.generation);
            request.pre_restore_created = archive::current_timestamp();
            request.secure_live_directory = [&](const auto& path) {
                const auto secured = policy.apply(path, win32::StorageDaclObjectKind::directory);
                return secured ? archive::Result{} : archive::Result{archive::Error::publication_failed, secured.operation};
            };
            request.secure_live_file = [&](const auto& path) {
                const auto secured = policy.apply(path, win32::StorageDaclObjectKind::file);
                return secured ? archive::Result{} : archive::Result{archive::Error::publication_failed, secured.operation};
            };
            const auto restored = archive::restore_backup(request);
            if (!restored.result.ok()) {
                std::cerr << "restore error=" << restored.result.detail << " rollback="
                    << restored.rollback_attempted << '/' << restored.rollback_succeeded << '\n';
                return 1;
            }
            install::verify_runtime_installation(root, owner);
            install::require(!std::filesystem::exists(root / L"Displays"), "legacy active tree remains");
            std::wcout << L"RESTORE PASS preserved=" << restored.pre_restore_archive.native()
                << L" previous=" << restored.retained_previous_state.native() << L'\n';
        } else throw std::runtime_error("expected backup or restore <verified leaf> <sha256>");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "INSTALLED ARCHIVE FAIL " << error.what() << '\n';
        return 1;
    }
}
