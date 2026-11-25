#pragma once
#include "sfx/application/qwk_transfer.hpp"
#include <filesystem>

namespace sfx::apps::runtime {
class LinuxQwkPacketTransfer final:public application::IQwkPacketTransfer {
public:
    LinuxQwkPacketTransfer(std::filesystem::path,application::DecodedQwkTransferChannel,
        std::function<void(std::string_view)> observer={},
        std::function<void(std::string resource,std::string diagnostic)> work_problem={});
    ~LinuxQwkPacketTransfer()override;
    bool available(const application::QwkTransferRequest&)override;
    std::string unavailable_reason()const;
    application::QwkReceiveResult receive(const application::QwkTransferRequest&)override;
    application::QwkSendResult send(const application::QwkTransferRequest&,std::string_view)override;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
