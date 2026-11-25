#pragma once
#include "linux_web_sessions.hpp"
#include "sfx/application/file_management.hpp"
#include <nlohmann/json.hpp>

namespace sfx::apps::runtime::web {
application::FileManagementReadRequest web_file_query(const nlohmann::json&);
application::FileManagementTask web_file_task(const nlohmann::json&,application::Identifier128 job,application::Identifier128 operation);
nlohmann::json web_file_snapshot(const application::FileManagementReadSnapshot&);
nlohmann::json web_file_status(const application::FileManagementJobStatus&);
nlohmann::json web_hatch_catalog(const application::FtnHatchCatalog&);
struct WebFileReview {
    std::string handle;
    application::FileManagementTask task;
    nlohmann::json choices;
    bool submitted{},known{};
    application::FileManagementJobStatus status;
};
// A browser owns one immutable reviewed task. An uncertain start can only be
// retried with that same task/job, never silently replaced by a new mutation.
class WebFileActions final {
public:
    using Start=std::function<application::FileManagementJobStatus(const application::FileManagementTask&)>;
    using Read=std::function<application::FileManagementJobStatus(application::Identifier128)>;
    WebFileActions(application::IClock&,Start,Read,Read);
    std::optional<WebFileReview> prepare(std::string,application::FileManagementTask,nlohmann::json);
    std::optional<WebFileReview> current(std::string_view);
    std::optional<WebFileReview> act(std::string_view,std::string_view,std::string_view);
    void clear();
private:
    struct State {WebFileReview review;std::uint64_t touched{};};
    application::IClock& clock_;Start start_;Read status_,cancel_;std::mutex mutex_;std::map<std::string,State> states_;
    void expire();
    void refresh(State&);
};
}
