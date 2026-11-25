#pragma once
#include "file_transfer_completion.hpp"
#include "alpha_message_receive.hpp"
#include "sfx/platform/win32/file_payload_store.hpp"
namespace sfx::apps::runtime {
// Native-only capability. Not an IPC/caller-supplied handle, path or callback.
// The caller retains the original payload lease through synchronous run().
struct NativeFileTransferItem {
    std::string wire_name;
    std::uint64_t maximum{};
    application::FileObjectIdentity object;
    std::function<platform::win32::installation::Handle()> duplicate;
};
struct DecodedFileTransferChannel {
    std::function<bool()> acquire,authorized;
    std::function<void(bool synchronized)> release;
    std::function<application::InteractiveStreamReadResult(std::size_t)> read;
    std::function<application::InteractiveStreamWriteResult(std::string_view)> write;
    std::function<std::uint64_t()> now,absolute_deadline;
    // Reset only the binary transport's idle window for one bounded abort.
    // Must retain calling-time, shutdown, authority and total-transfer bounds.
    std::function<bool()> begin_recovery;
};
enum class NativeFileTransferCode {completed,unavailable,failed,disconnected};
struct NativeFileTransferResult {
    NativeFileTransferCode code{NativeFileTransferCode::failed};
    std::vector<file_transfer_detail::SlotCompletion> files;
    std::uint32_t refused{};
    std::function<void()> confirm_completed; // trusted host after durable receipt/requery
};
class AlphaFileTransfer final {
public:
    AlphaFileTransfer(std::filesystem::path,std::wstring,DecodedFileTransferChannel,MessageReceiveObserver={},WorkProblemObserver work_problem={});
    ~AlphaFileTransfer();
    bool available();
    NativeFileTransferResult run(file_transfer_detail::Direction,const std::vector<NativeFileTransferItem>&,
        application::TransferProtocolPreference=application::TransferProtocolPreference::zmodem);
private:struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace sfx::apps::runtime
