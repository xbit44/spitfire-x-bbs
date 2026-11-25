#pragma once
#include "alpha_file_transfer.hpp"
#include "sfx/application/file_session.hpp"
#include "sfx/platform/win32/file_intake.hpp"

namespace sfx::apps::runtime {
using application::CallerFileReport;
using application::CallerFileBatchResult;
using application::CallerUploadReview;
struct CallerFileTransferPorts {
    application::IFileStorage& storage;
    std::filesystem::path installation;
    std::wstring owner;
    std::function<std::optional<application::FileActor>()> actor;
    std::function<application::Identifier128()> operation;
    std::function<std::string()> local_date;
    DecodedFileTransferChannel channel;
    platform::win32::WindowsFileIntake::Scan scan;
    // Called only AFTER the binary stream has ended and validation/scan passed.
    // Empty means no metadata acceptance, not deletion of staged content.
    std::function<std::optional<CallerUploadReview>(const application::FileUploadItem&,
        const platform::win32::NativeFileIntakeOutcome&,std::uint64_t)> review;
    MessageReceiveObserver observer;
    // Trusted finalized intake only: detection disposition is known here,
    // never inferred from the scanner's exit code or before remove_exact.
    std::function<void(const application::FileUploadItem&,const platform::win32::NativeFileIntakeOutcome&)> scan_notice;
    WorkProblemObserver work_problem;
};
// Trusted native Runtime coordinator, not IPC success/path assertions. Queues
// are immutable inputs; caller session removes ONLY reported completed items.
class AlphaCallerFileTransfers final {
public:
    explicit AlphaCallerFileTransfers(CallerFileTransferPorts);
    CallerFileBatchResult upload(const std::vector<application::FileUploadItem>&,bool private_submission,
        application::TransferProtocolPreference=application::TransferProtocolPreference::zmodem);
    CallerFileBatchResult download(const std::vector<application::FileId>&,
        application::TransferProtocolPreference=application::TransferProtocolPreference::zmodem);
private:
    CallerFileTransferPorts ports_;
    platform::win32::FilePayloadDirectory area(const application::FileConfiguration&,application::FileAreaId)const;
    bool live(const application::FileActor&,std::int64_t catalog_revision)const;
};
} // namespace sfx::apps::runtime
