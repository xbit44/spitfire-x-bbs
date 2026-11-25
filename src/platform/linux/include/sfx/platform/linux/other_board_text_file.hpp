#pragma once
#include "sfx/application/classic_information_storage.hpp"
#include <filesystem>
#include <functional>
#include <mutex>
namespace sfx::platform::linux_host {
void initialize_other_board_text(const std::filesystem::path&);
class LinuxOtherBoardTextStorage final:public application::IOtherBoardTextStorage {
public:
    using Log=std::function<bool(const application::OtherBoardLogRecord&)>;
    LinuxOtherBoardTextStorage(std::filesystem::path root,Log log):root_(std::move(root)),log_(std::move(log)){}
    application::StorageResult<application::OtherBoardPage> read(std::int64_t)override;
    application::StorageResult<application::OtherBoardLogRecord> change(const application::ClassicInformationChange&,
        const application::ClassicInformationActor&,std::string_view)override;
    void set_log(Log log){std::lock_guard<std::mutex> lock(log_mutex_);log_=std::move(log);}
    bool publish_log(const application::OtherBoardLogRecord& record)noexcept override{
        try{Log log;{std::lock_guard<std::mutex> lock(log_mutex_);log=log_;}return log&&log(record);}catch(...){return false;}}
private:
    std::filesystem::path root_;Log log_;std::mutex mutex_,log_mutex_;
};
}
