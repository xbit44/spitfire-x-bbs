#pragma once
#include "linux_web_sessions.hpp"
#include "sfx/application/ftn_runtime.hpp"
#include <nlohmann/json.hpp>

namespace sfx::apps::runtime::web {
nlohmann::json web_binkp_snapshot(const application::FtnRuntimeResponse&);
struct WebBinkpReview {
    std::string handle;
    application::Identifier128 epoch;
    std::int64_t revision{};
    application::FtnRuntimeRequest request;
    bool submitted{},completed{};
    application::FtnRuntimeResponse result;
};
// A reviewed request is consumed before dispatch. Reading or resubmitting its
// receipt cannot dispatch it again, including after an uncertain exception.
class WebBinkpActions final {
public:
    using Execute=std::function<application::FtnRuntimeResponse(const application::FtnRuntimeRequest&,application::Identifier128,std::int64_t)>;
    WebBinkpActions(application::IClock&,Execute);
    std::optional<WebBinkpReview> prepare(std::string,const application::FtnRuntimeRequest&,application::Identifier128,std::int64_t);
    std::optional<WebBinkpReview> read(std::string_view,std::string_view,bool submit=false);
    std::optional<WebBinkpReview> current(std::string_view);
    bool cancel(std::string_view,std::string_view);
private:
    struct State {WebBinkpReview review;std::uint64_t touched{};};
    application::IClock& clock_;Execute execute_;std::mutex mutex_;std::map<std::string,State> states_;
    void expire();
};
}
