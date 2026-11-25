#pragma once
#include "sfx/archive/file_contents.hpp"
#include "sfx/archive/backup_service.hpp"
#include "sfx/platform/linux/file_payload_store.hpp"
#include "sfx/platform/linux/backup_collections.hpp"

namespace sfx::platform::linux_host {
// Trusted host's newly completed Work snapshot only. Pin the exact object for
// readers. Only explicit confirmed-success cleanup removes it by held identity.
// Stack unwinding and uncertain outcomes preserve all new scratch evidence.
class FileBackupSnapshot final {
public:
    FileBackupSnapshot(const FilePayloadDirectory&, std::string);
    ~FileBackupSnapshot();
    FileBackupSnapshot(const FileBackupSnapshot&)=delete;
    bool cleanup() noexcept;
private:
    FilePayloadDirectory directory_;
    std::string name_;
    std::optional<FilePayloadLease> pin_;
    application::FileObjectIdentity identity_;
    bool attempted_{}, removed_{};
};
// Stopped/guarded Setup backup only: read the existing schema without starting
// Runtime, migrating a predecessor, creating tables or changing its settings.
// A genuine pre-file-system database returns an empty area list (core-only).
application::FileConfiguration read_backup_file_configuration(const std::filesystem::path&,
    std::shared_ptr<IFilePayloadBindings>);
// Owns exact immutable source leases and the verified predecessor set until
// publication finishes. A failure never falls back to a core-only backup when
// an explicitly selected area could not be captured.
class FileBackupCapture final {
public:
    static std::unique_ptr<FileBackupCapture> begin(const std::filesystem::path&,
        std::shared_ptr<IFilePayloadBindings>, const application::FileConfiguration&, const BackupCollection&,
        archive::BackupKind, const archive::CancelCheck& = {});
    ~FileBackupCapture();
    FileBackupCapture(const FileBackupCapture&)=delete;
    // Invoke after the database snapshot: checks membership/configuration and
    // binds this content index/payloads into the always-full core request.
    void bind(archive::ManagedBackupRequest&, const application::FileConfiguration&);
    bool cleanup() noexcept;
private:
    struct Impl;explicit FileBackupCapture(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

struct FileContentRestoreItem {
    std::int64_t area{};
    std::string name;
    enum class State { missing, identical, conflict } state{State::missing};
};
struct FileContentRestoreResult {
    std::size_t restored{}, identical{}, conflicts{};
    bool completed{};
    std::string diagnostic;
};
// Caller retains the genuine stopped-installation guard through preview,
// explicit confirmation and execution. No core/catalog changes, overwrites or
// library deletion. Native identities are not forged after restoring bytes.
class FileContentRestore final {
public:
    static std::unique_ptr<FileContentRestore> preview(const std::filesystem::path&,
        std::shared_ptr<IFilePayloadBindings>, const application::FileConfiguration&, const BackupCollection&,
        const std::filesystem::path&, const std::vector<std::int64_t>& selected_areas,
        const archive::CancelCheck& = {});
    ~FileContentRestore();
    const std::vector<FileContentRestoreItem>& items() const noexcept;
    const std::vector<std::pair<std::int64_t,std::filesystem::path>>& destinations() const noexcept;
    FileContentRestoreResult execute(const application::FileConfiguration&,const archive::CancelCheck& = {});
private:
    struct Impl;explicit FileContentRestore(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
} // namespace sfx::platform::linux_host
