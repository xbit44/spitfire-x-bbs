#pragma once

#include "sfx/archive/archive_engine.hpp"

#include <filesystem>
#include <functional>
#include <string>

namespace sfx::archive {

enum class BackupKind {
    setup_upgrade,
    manual,
    scheduled,
    pre_restore,
};

struct ManagedBackupRequest final {
    std::filesystem::path installation_root;
    // The caller must validate/pin this local private collection before use.
    // Empty selects Upgrades for Setup/pre-restore, Scheduled for ordinary jobs.
    // The selected destination must already exist and be safe; no fallback.
    std::filesystem::path destination_directory;
    BackupKind kind{BackupKind::manual};
    std::string transaction_id;
    std::string sfx_version{"1.0.0-alpha.1"};
    std::string source_generation{"unknown"};
    Timestamp created;
    std::filesystem::path database_snapshot;
    // Trusted host sets this only from the verified installation capability,
    // while FTN writers are quiescent through DB capture and archive completion.
    bool ftn_layout{};
    // Trusted host-owned, leased optional payload/index inputs only. Core data
    // is always full; these members are never core-restore output.
    std::vector<CreateBackupRequest::SourceOverride> file_contents;
    // Quiescent host-generated namespace proof for the same database/content
    // capture. A separate field cannot override another Config or core member.
    std::filesystem::path portable_identity_snapshot;
    // Native Linux uses a distinct authenticated identity map. This explicit
    // host-only override cannot replace Windows credentials or another member.
    std::filesystem::path linux_portable_identity_snapshot;
#if defined(__linux__)
    // Native host's immutable point-in-time operational log capture only.
    // This cannot replace another log, configuration or core member.
    std::filesystem::path operational_log_snapshot;
    // Complete writer-owned journal/SQLite cut. An empty vector is not an
    // instruction to omit live logs. Only the three closed native journal
    // families may be replaced, including their captured retained rotations.
    bool linux_journal_snapshot_set{};
    std::vector<CreateBackupRequest::SourceOverride> linux_journal_snapshots;
#endif
    Limits limits{};
    CancelCheck cancelled;
    Progress progress;
    PhysicalLayout physical_layout{PhysicalLayout::canonical};
};

struct ManagedBackupResult final {
    Result result;
    std::filesystem::path archive_path;
};

using SecureDirectory = std::function<Result(const std::filesystem::path&)>;
using SecureFile = std::function<Result(const std::filesystem::path&)>;

struct RestoreRequest final {
    std::filesystem::path installation_root;
    std::filesystem::path archive_path;
    std::string transaction_id;
    std::string sfx_version{"1.0.0-alpha.1"};
    std::string source_generation{"unknown"};
    bool ftn_layout{}; // Verified receipt; never inferred from archive/path.
    Timestamp pre_restore_created;
    Limits limits{};
    CancelCheck cancelled;
    Progress progress;
    SecureDirectory secure_live_directory;
    SecureFile secure_live_file;
    // Called under the host's continuously held exclusive guard after protected
    // live publication, before final commit. A failed prerequisite rolls the
    // restored roles back through the existing recovery path, not a repair.
    std::function<Result()> verify_live_publication;
    PhysicalLayout physical_layout{PhysicalLayout::canonical};
};

struct RestoreResult final {
    Result result;
    std::filesystem::path pre_restore_archive;
    std::filesystem::path retained_previous_state;
    std::filesystem::path retained_failed_state;
    bool live_state_changed{false};
    bool rollback_attempted{false};
    bool rollback_succeeded{false};
    // A returned candidate path alone is not creation provenance. In
    // particular, create-new failures may report an existing destination.
    bool pre_restore_archive_created{false};
    bool retained_previous_state_created{false};
    bool retained_failed_state_created{false};
};

[[nodiscard]] std::filesystem::path managed_backup_filename(
    BackupKind kind, const Timestamp& timestamp,
    PhysicalLayout physical_layout = PhysicalLayout::canonical);
[[nodiscard]] ManagedBackupResult create_managed_backup(
    const ManagedBackupRequest& request);
// CORE ONLY: Files and durable Data/FileIntake/FileRemoved contents are never
// copied, moved, re-permissioned or removed, even for legacy full-role media.
// Explicit selected-area content restoration is a separate authorized workflow.
[[nodiscard]] RestoreResult restore_backup(const RestoreRequest& request);

} // namespace sfx::archive
