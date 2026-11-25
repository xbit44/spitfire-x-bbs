#pragma once

#include "sfx/archive/backup_service.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/archive/file_contents.hpp"

namespace sfx::platform::win32 {

// Keeps every existing ancestor and the private destination pinned against
// rename/reparse substitution until publication/retention has finished.
struct BackupCollection final {
    std::filesystem::path directory;
    std::vector<installation::Handle> anchors;
};

struct VerifiedBackupMember {
    std::filesystem::path path;
    std::uint64_t sequence{};
    std::string sha256;
    archive::BackupInspection inspection;
    archive::ContentIndex contents;
    std::vector<installation::Handle> pins;
};
// Content predicate only; never an authorization without the native managed
// collection verifier below. Unknown/configured state remains persistent.
bool pre_setup_backup_contents(const archive::BackupInspection&, std::uint64_t current_generation);
// Read-only, bounded verification of the entire Upgrades directory. Every
// archive and sidecar must match this installation and contain only pre-setup
// metadata/display files. Successful leases remain held through classification.
bool verified_pre_setup_upgrade_backups(const std::filesystem::path& root,
    const std::wstring& owner, std::vector<installation::Handle>& leases);
// Exact same-installation/pool archives only, ordered by authenticated sequence.
// Native archive+marker leases remain alive with the returned inventory.
std::vector<VerifiedBackupMember> verified_backup_members(
    const std::filesystem::path&, const std::wstring&, const BackupCollection&,
    archive::BackupKind, const archive::CancelCheck& = {});
// Every referenced member must exist in an older exact archive with the same
// manifest size/hash. Empty selected means verify all restore points.
void verify_content_dependencies(const std::vector<VerifiedBackupMember>&,
    const std::vector<std::size_t>& selected = {});
std::vector<std::size_t> content_dependency_closure(const std::vector<VerifiedBackupMember>&,
    const std::vector<std::size_t>& selected);

// Only the final private directory is created. External parent directories
// must already exist; no permission repair or fallback occurs.
BackupCollection open_backup_collection(const std::filesystem::path& root,
    const std::wstring& owner, archive::BackupKind kind,
    const std::string& saved_destination = {}, bool create = true);

// Records a verified publication for this installation, then removes only
// verified, hash-bound, same-installation managed members of this exact pool.
// Setup calls this only AFTER its verified installation commit. Pre-restore,
// legacy/unknown material and recovery trees never participate.
std::size_t finish_backup_collection(const std::filesystem::path& root,
    const std::wstring& owner, const BackupCollection& collection,
    const std::filesystem::path& published, archive::BackupKind kind,
    std::uint32_t retained, archive::CancelCheck cancelled = {},
    std::function<void(std::size_t inspected,std::size_t total)> progress = {});
enum class BackupFinalizationStage { existing_archives, published_archive, ownership, dependencies, retirement };
std::size_t finish_backup_collection(const std::filesystem::path& root,
    const std::wstring& owner, const BackupCollection& collection,
    const std::filesystem::path& published, archive::BackupKind kind,
    std::uint32_t retained, archive::CancelCheck cancelled,
    std::function<void(std::size_t inspected,std::size_t total)> progress,
    std::function<void(BackupFinalizationStage)> stage);

} // namespace sfx::platform::win32
