#pragma once
#include "file_transfer_completion.hpp"
#include "sfx/application/message_receive.hpp"
#include "sfx/platform/linux/file_payload_store.hpp"
#include <functional>

namespace sfx::apps::runtime {
// Trusted native composition only. FDs are already bounded receive capabilities
// or read-only send capabilities; never supplied by a caller/browser request.
// Owner retains each original lease/descriptor through synchronous run(), then
// closes its descriptor and returns receive custody only after run() has reaped.
struct LinuxFileTransferItem {
    std::string wire_name;
    std::uint64_t maximum{};
    application::FileObjectIdentity native_object;
    int descriptor{-1};
};
struct LinuxFileTransferChannel {
    std::function<bool()> acquire,authorized;
    std::function<void(bool synchronized)> release;
    std::function<application::InteractiveStreamReadResult(std::size_t)> read;
    std::function<application::InteractiveStreamWriteResult(std::string_view)> write;
    std::function<std::uint64_t()> now,absolute_deadline;
    std::function<bool()> begin_recovery;
};
enum class LinuxFileTransferCode {completed,unavailable,failed,disconnected};
struct LinuxFileTransferResult {
    LinuxFileTransferCode code{LinuxFileTransferCode::failed};
    std::vector<file_transfer_detail::SlotCompletion> files;
    std::uint32_t refused{};
    // Call only after the host durably records/requeries the business outcome.
    // Failed/uncertain transfer work remains available for recovery review.
    std::function<void()> confirm_completed;
};
class LinuxFileTransfer final {
public:
    LinuxFileTransfer(std::filesystem::path,LinuxFileTransferChannel);
    bool available();
    LinuxFileTransferResult run(file_transfer_detail::Direction,const std::vector<LinuxFileTransferItem>&,
        application::TransferProtocolPreference=application::TransferProtocolPreference::zmodem);
private:
    std::filesystem::path root_;
    LinuxFileTransferChannel channel_;
};
}
