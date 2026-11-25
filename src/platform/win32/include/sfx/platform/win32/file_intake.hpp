#pragma once
#include "sfx/platform/win32/file_payload_store.hpp"
#include <functional>

namespace sfx::platform::win32 {
enum class FileIntakeBoundary { before_delete, before_placement, after_placement, before_commit, staging_bound, destination_bound, before_staging_cleanup };
struct NativeFileIntakeOutcome {
    application::FileIntakeState state{application::FileIntakeState::held};
    application::FileScanResult scan;
    std::optional<application::FileDescriptions> diz;
    std::optional<application::FileEntry> accepted;
    bool deletion_confirmed{}, journal_confirmed{};
    std::string detail; // fixed safe text, no physical paths or exception output
};
// A native Runtime-owned receive capability, not a caller-provided receipt.
// Journal first, CREATE_NEW second; destructor NEVER deletes staged payloads.
// The scanner callback is supplied by the trusted host using WindowsFileScanner,
// never by IPC/client data. Tests explicitly distinguish injected scan outcomes.
class WindowsFileIntake final {
public:
    using Scan = std::function<application::FileScanResult(FilePayloadLease&,std::uint64_t)>;
    using Boundary = std::function<void(FileIntakeBoundary)>;
    WindowsFileIntake(application::IFileStorage&, application::FileConfigurationActor,
        application::FileActor, application::FileIntakeStart, FilePayloadDirectory durable_intake,
        Boundary qualification_boundary = {});
    void append(const std::uint8_t*,std::size_t);
    // Only the trusted native transfer host receives this write-only duplicate.
    // The durable staging identity is already committed before child launch.
    // Host must reap the child/close duplicate before finish (which hashes/seals).
    installation::Handle receive_handle();
    NativeFileIntakeOutcome finish(std::string_view filename,bool transfer_complete,const Scan& = {});
    // The host derives destination from the current protected area configuration,
    // holding its capability. This layer verifies that exact configured path.
    NativeFileIntakeOutcome accept(const std::filesystem::path& installation,
        const FilePayloadDirectory& destination,const application::FileDescriptions&,bool sysop_only);
    const application::FileIntake& record() const noexcept {return record_;}
private:
    NativeFileIntakeOutcome hold(std::string_view detail);
    void boundary(FileIntakeBoundary);
    application::IFileStorage& storage_;
    application::FileConfigurationActor owner_;
    application::FileActor actor_;
    FilePayloadDirectory staging_;
    application::FileIntake record_;
    std::optional<FilePayloadLease> payload_;
    NativeFileIntakeOutcome outcome_;
    Boundary boundary_;
    bool receive_failed_{},finished_{};
};
// Explicit host/Sysop reconciliation of an ACCEPTED upload's redundant staging
// only. A surviving current final object is verified/held across removal of an
// actual redundant original. Missing/replaced final retains recovery storage.
// Never publishes held content or removes an existing library file.
bool cleanup_accepted_file_intake(application::IFileStorage&,const application::FileConfigurationActor&,
    application::Identifier128,const FilePayloadDirectory& durable_intake,
    const std::filesystem::path& installation,const std::wstring& owner,
    const std::function<bool(std::uint64_t,std::uint64_t)>& progress = {});
} // namespace sfx::platform::win32
