#pragma once
#include "sfx/application/file_management.hpp"
#include "sfx/application/ftn_work.hpp"
#include "sfx/platform/linux/file_manager.hpp"
#include <memory>

namespace sfx::platform::linux_host {
// Exactly one worker per authenticated owner host/exclusively held offline
// session. The host must stop it BEFORE closing storage/releasing ownership.
// No caller transport or UI supplies authority/native-success assertions.
class LinuxFileAdministration final {
public:
    struct HatchOutcome {bool queued{},journaled{};std::string reason;};
    using Hatch=std::function<HatchOutcome(const application::FtnHatchRequest&,const std::function<bool()>&,const LinuxFileManager::Progress&)>;
    using IdleClock=std::function<std::uint64_t()>;
    static constexpr std::uint64_t view_idle_millis=5*60*1000;
    LinuxFileAdministration(application::IFileStorage&,std::filesystem::path,std::shared_ptr<IFilePayloadBindings>,
        application::LocalAccountAuthority,LinuxFileManager::Boundary test_boundary={},IdleClock qualification_clock={},application::IFtnMailStorage* ftn_storage=nullptr,Hatch hatch={});
    // Runtime-bound session only; the storage actor remains a caller. This
    // overload exposes the Section14A task allowlist, never local-owner APIs.
    LinuxFileAdministration(application::IFileStorage&,std::filesystem::path,std::shared_ptr<IFilePayloadBindings>,
        application::MessageActor,std::function<std::optional<application::MessageActor>()> revalidate);
    ~LinuxFileAdministration();
    LinuxFileAdministration(const LinuxFileAdministration&)=delete;
    application::FileManagementJobStatus start(const application::FileManagementTask&);
    application::FileManagementJobStatus status(application::Identifier128);
    application::FileManagementJobStatus cancel(application::Identifier128);
    // Host housekeeping; never interrupts an active job. An abandoned online
    // client cannot retain an immutable library/discovery lease indefinitely.
    void expire_idle_views();
    bool active();
    void stop() noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace sfx::platform::linux_host
