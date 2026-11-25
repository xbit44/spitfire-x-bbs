#pragma once
#include "sfx/application/file_management.hpp"
#include "sfx/application/ftn_work.hpp"
#include "sfx/platform/win32/file_manager.hpp"
#include <memory>

namespace sfx::platform::win32 {
// Exactly one worker per authenticated owner host/exclusively held offline
// session. The host must stop it BEFORE closing storage/releasing ownership.
// No caller transport or UI supplies authority/native-success assertions.
class WindowsFileAdministration final {
public:
    using IdleClock=std::function<std::uint64_t()>;
    static constexpr std::uint64_t view_idle_millis=5*60*1000;
    WindowsFileAdministration(application::IFileStorage&,std::filesystem::path,std::wstring,
        application::LocalAccountAuthority,WindowsFileManager::Boundary test_boundary={},IdleClock qualification_clock={},application::IFtnMailStorage* ftn_storage=nullptr);
    // Runtime-bound session only; the storage actor remains a caller. This
    // overload exposes the Section14A task allowlist, never local-owner APIs.
    WindowsFileAdministration(application::IFileStorage&,std::filesystem::path,std::wstring,
        application::MessageActor,std::function<std::optional<application::MessageActor>()> revalidate);
    ~WindowsFileAdministration();
    WindowsFileAdministration(const WindowsFileAdministration&)=delete;
    application::FileManagementJobStatus start(const application::FileManagementTask&);
    application::FileManagementJobStatus status(application::Identifier128);
    application::FileManagementJobStatus cancel(application::Identifier128);
    // Host housekeeping; never interrupts an active job. An abandoned online
    // client cannot retain an immutable library/discovery lease indefinitely.
    void expire_idle_views();
    void stop() noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace sfx::platform::win32
