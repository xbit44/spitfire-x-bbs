#pragma once
#include "sfx/archive/backup_service.hpp"
#include "sfx/archive/file_contents.hpp"
#include "sfx/platform/linux/file_payload_store.hpp"

namespace sfx::platform::linux_host {
// Descriptor-anchored private collection. Operations revalidate native name,
// object and mutation evidence; no Windows mandatory sharing is assumed.
struct BackupCollection {
    std::filesystem::path directory;
    archive::BackupKind kind{archive::BackupKind::manual};
    std::optional<FilePayloadDirectory> custody;
};
struct VerifiedBackupMember {
    std::filesystem::path path;
    std::uint64_t sequence{};
    std::string sha256;
    archive::BackupInspection inspection;
    archive::ContentIndex contents;
    std::vector<FilePayloadLease> pins;
};
BackupCollection open_backup_collection(const std::filesystem::path&,std::shared_ptr<IFilePayloadBindings>,
    archive::BackupKind,const std::string& destination={},bool create=true);
// Only after this operation's successful create-new archive publication into
// the held private collection. Never a repair API for existing backup media.
void secure_new_backup_archive(const BackupCollection&,const std::filesystem::path&);
void secure_new_backup_snapshot(const FilePayloadDirectory&,std::string_view name);
std::vector<VerifiedBackupMember> verified_backup_members(const std::filesystem::path&,
    std::shared_ptr<IFilePayloadBindings>,const BackupCollection&,archive::BackupKind,const archive::CancelCheck& = {});
std::vector<std::size_t> content_dependency_closure(const std::vector<VerifiedBackupMember>&,
    const std::vector<std::size_t>&);
void verify_content_dependencies(const std::vector<VerifiedBackupMember>&,const std::vector<std::size_t>& = {});
std::size_t finish_backup_collection(const std::filesystem::path&,std::shared_ptr<IFilePayloadBindings>,
    const BackupCollection&,const std::filesystem::path&,archive::BackupKind,std::uint32_t,
    archive::CancelCheck = {},std::function<void(std::size_t,std::size_t)> = {});
} // namespace sfx::platform::linux_host
