// Adapted from SFX own Windows caller orchestration; original source preserved.
#pragma once
#include "linux_file_transfer.hpp"
#include "sfx/application/file_session.hpp"
#include "sfx/platform/linux/file_intake.hpp"

namespace sfx::apps::runtime {
using application::CallerFileReport;
using application::CallerFileBatchResult;
using application::CallerUploadReview;
struct LinuxCallerFileTransferPorts {
    application::IFileStorage& storage;
    std::filesystem::path installation;
    std::shared_ptr<platform::linux_host::IFilePayloadBindings> bindings;
    std::function<std::optional<application::FileActor>()> actor;
    std::function<application::Identifier128()> operation;
    std::function<std::string()> local_date;
    LinuxFileTransferChannel channel;
    platform::linux_host::LinuxFileIntake::Scan scan;
    // Called only AFTER the binary stream has ended and validation/scan passed.
    // Empty means no metadata acceptance, not deletion of staged content.
    std::function<std::optional<CallerUploadReview>(const application::FileUploadItem&,
        const platform::linux_host::NativeFileIntakeOutcome&,std::uint64_t)> review;
    std::function<void(std::string_view,std::uint32_t,std::uint32_t,std::size_t)> observer;
    // Trusted finalized intake only: detection disposition is known here,
    // never inferred from the scanner's exit code or before remove_exact.
    std::function<void(const application::FileUploadItem&,const platform::linux_host::NativeFileIntakeOutcome&)> scan_notice;
    std::function<void(std::string,std::string)> work_problem;
};
// Trusted native Runtime coordinator, not IPC success/path assertions. Queues
// are immutable inputs; caller session removes ONLY reported completed items.
class LinuxCallerFileTransfers final {
public:
    explicit LinuxCallerFileTransfers(LinuxCallerFileTransferPorts);
    CallerFileBatchResult upload(const std::vector<application::FileUploadItem>&,bool private_submission,
        application::TransferProtocolPreference=application::TransferProtocolPreference::zmodem);
    CallerFileBatchResult download(const std::vector<application::FileId>&,
        application::TransferProtocolPreference=application::TransferProtocolPreference::zmodem);
private:
    LinuxCallerFileTransferPorts ports_;
    platform::linux_host::FilePayloadDirectory area(const application::FileConfiguration&,application::FileAreaId)const;
    bool live(const application::FileActor&,std::int64_t catalog_revision)const;
};
} // namespace sfx::apps::runtime
