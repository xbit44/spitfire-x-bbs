#pragma once
#include "sfx/archive/backup_service.hpp"
#include "sfx/application/ftn_history_recovery.hpp"
#include <memory>
#include <string_view>

namespace sfx::platform::linux_host {
std::unique_ptr<application::IFtnHistoryRecovery> preview_ftn_history_recovery(const std::filesystem::path&);
struct LinuxCoreRestoreResult {
    archive::RestoreResult restore;
    bool file_prepare_required{};
    bool ftn_recovery_required{};
};
// Stopped-installation capability retained across preview, explicit token
// confirmation, publication and rollback. Never runs a BBS or modifies Files.
class LinuxCoreRestore final {
public:
    static std::unique_ptr<LinuxCoreRestore> preview(const std::filesystem::path& root,
        const std::filesystem::path& archive,const archive::CancelCheck& = {},
        // Trusted host build identity, captured with this preview. Never read
        // from a browser restore request or from the selected old archive.
        std::string_view sfx_version = "unknown");
    ~LinuxCoreRestore();
    const archive::BackupInspection& inspection() const noexcept;
    const std::string& confirmation_token() const noexcept;
    LinuxCoreRestoreResult execute(std::string_view confirmation,const archive::CancelCheck& = {},
        // Trusted native qualification seam, never selected by a web request.
        std::function<archive::Result()> verify_publication = {},
        archive::Timestamp preservation_time = {});
private:
    struct Impl;
    explicit LinuxCoreRestore(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
} // namespace sfx::platform::linux_host
