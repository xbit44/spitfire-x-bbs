#pragma once

#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>

namespace sfx::platform::win32 {

// Only trusted, owner-controlled Windows objects supply these entries. ZIP
// metadata and transient logon identities are never an authorization source.
using BackupReadGrants = std::vector<std::vector<unsigned char>>;
struct BackupPublicationAccess final {
    BackupReadGrants grants;
    bool inherit_parent{true};
};
BackupPublicationAccess capture_backup_publication_access(const std::filesystem::path& previous,
    const std::wstring& owner, bool directory);
void apply_backup_read_grants(const std::filesystem::path& destination,
    const BackupReadGrants& grants, bool directory = false);
BackupReadGrants capture_backup_read_grants(HANDLE object,
    const std::wstring& owner, bool directory, bool inherit_to_child = false);
BackupReadGrants capture_backup_read_grants(const std::filesystem::path& path,
    const std::wstring& owner, bool directory, bool inherit_to_child = false);
std::vector<unsigned char> merge_backup_read_grants(PACL base,
    const BackupReadGrants& grants);
void carry_backup_read_grants(const std::filesystem::path& trusted_previous,
    const std::filesystem::path& destination, const std::wstring& owner);

// DACL-only transaction snapshot: ownership, SACLs and contents are unchanged.
struct BackupDaclSnapshot final {
    std::vector<unsigned char> descriptor;
    explicit BackupDaclSnapshot(HANDLE object);
    std::wstring sddl() const;
    void restore(HANDLE object) const;
};
void ensure_backup_owner_read(HANDLE object, const std::wstring& owner,
    bool directory, const BackupDaclSnapshot* original = nullptr);
bool backup_owner_has_read(HANDLE object, const std::wstring& owner, bool directory);
bool backup_boundary_with_readers(HANDLE object, void* expected, const std::wstring& owner);

} // namespace sfx::platform::win32
