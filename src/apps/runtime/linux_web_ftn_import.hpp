#pragma once
#include "linux_web_ftn_source.hpp"
#include "linux_web_ftn_transfer.hpp"
#include "sfx/application/ftn_echo_import.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace sfx::apps::runtime::web {
using FtnListReader=std::function<FtnEchoListDownload(std::string,bool,const std::atomic<bool>&)>;
struct WebFtnListSnapshot {
    application::FtnTransferToken token{};
    std::string network,source,diagnostic,selected_source;
    bool active{true},ready{},url{};
    application::FtnEchoList list;
};
class WebFtnListJob final {
public:
    WebFtnListJob(application::FtnTransferToken,std::string network,std::string source,bool url,bool cp437,FtnListReader,std::string selected_source={});
    ~WebFtnListJob();
    WebFtnListSnapshot snapshot() const;
    void cancel() noexcept;
private:
    mutable std::mutex mutex_;
    WebFtnListSnapshot current_;
    std::atomic<bool> cancelled_{false};
    std::thread thread_;
};
struct WebFtnImportCatalog {
    std::int64_t revision{};
    std::vector<std::pair<std::int64_t,std::string>> groups;
    std::vector<std::pair<std::int64_t,std::int64_t>> echoes;
};
application::FtnEchoImportPlan web_ftn_import_plan(const application::FtnConfiguration&,const WebFtnListSnapshot&,const WebFtnImportCatalog&,const std::vector<std::string>& selected,application::FtnEchoImportRequirements,bool update_text);
}
