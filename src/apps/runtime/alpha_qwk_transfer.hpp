#pragma once
#include "sfx/application/qwk_transfer.hpp"
#include "alpha_message_receive.hpp"
namespace sfx::apps::runtime {
// Separate external GPL program. Old message helper and host stay unchanged.
class AlphaQwkPacketTransfer final:public application::IQwkPacketTransfer {
public:
    AlphaQwkPacketTransfer(std::filesystem::path,std::wstring,
        application::DecodedQwkTransferChannel,MessageReceiveObserver={},WorkProblemObserver work_problem={});
    ~AlphaQwkPacketTransfer()override;
    bool available(const application::QwkTransferRequest&)override;
    std::string unavailable_reason()const;
    application::QwkReceiveResult receive(const application::QwkTransferRequest&)override;
    application::QwkSendResult send(const application::QwkTransferRequest&,std::string_view)override;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace sfx::apps::runtime
