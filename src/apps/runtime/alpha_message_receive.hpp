#pragma once
#include "sfx/application/message_receive.hpp"
#include <filesystem>
#include <memory>
#include "alpha_job_work.hpp"
namespace sfx::apps::runtime {
// Fixed host-phase diagnostics only: never file contents, names or commands.
using MessageReceiveObserver=std::function<void(std::string_view,std::uint32_t,std::uint32_t,std::size_t)>;
// Separate executable adapter. No GPL helper implementation is linked into SFX.
class AlphaMessageSourceReceiver final : public application::IMessageSourceReceiver {
public:
    AlphaMessageSourceReceiver(std::filesystem::path root,std::wstring owner,
        application::DecodedMessageReceiveChannel channel,MessageReceiveObserver observer={},WorkProblemObserver work_problem={});
    ~AlphaMessageSourceReceiver() override;
    bool available(const application::MessageReceiveRequest&) override;
    application::MessageReceiveResult receive(const application::MessageReceiveRequest&) override;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace sfx::apps::runtime
