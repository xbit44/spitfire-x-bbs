#pragma once
#include "linux_web_sessions.hpp"
#include "sfx/application/chat_notice.hpp"

namespace sfx::apps::runtime::web {
struct WebNoticeReview {
    std::string handle;
    application::ChatNoticeRequest request;
    application::ChatResult result{application::ChatResult::unavailable};
    bool submitted{},completed{};
};
struct WebOperatorView {
    std::string handle;
    application::ChatOperatorSnapshot snapshot;
    std::uint64_t next_send{1};
};
// Browser-session ownership wraps the existing Runtime chat operations. Notice
// receipts prevent a lost HTTP response from sending a notice a second time.
// Conversation text is memory-only; the Runtime owns the ten-second chat lease.
class WebCallerActions final {
public:
    using Operator=std::function<application::ChatOperatorSnapshot(const application::ChatOperatorRequest&)>;
    using Notice=std::function<application::ChatResult(const application::ChatNoticeRequest&)>;
    WebCallerActions(application::IClock&,Operator,Notice);
    ~WebCallerActions();
    std::optional<WebNoticeReview> prepare_notice(std::string owner,const application::ChatNoticeRequest&);
    std::optional<WebNoticeReview> notice(std::string_view owner,std::string_view handle,bool send);
    std::optional<WebNoticeReview> current_notice(std::string_view owner);
    bool cancel_notice(std::string_view owner,std::string_view handle);
    std::optional<WebOperatorView> start(std::string owner,application::ChatOperatorRequest);
    std::optional<WebOperatorView> poll(std::string_view owner,std::uint64_t acknowledged);
    application::ChatResult send(std::string_view owner,std::string_view handle,std::uint64_t sequence,std::string_view text);
    bool end(std::string_view owner,std::string_view handle);
    void clear();
private:
    struct NoticeState {WebNoticeReview review;std::uint64_t touched{};};
    struct OperatorState {WebOperatorView view;application::Identifier128 identity;std::uint64_t touched{};};
    application::IClock& clock_;
    Operator operator_;Notice notice_;
    std::mutex mutex_;
    std::map<std::string,NoticeState> notices_;
    std::map<std::string,OperatorState> operators_;
    void expire();
    void end_view(OperatorState&) noexcept;
};
}
