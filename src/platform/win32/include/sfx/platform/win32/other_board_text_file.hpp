#pragma once
#include "sfx/application/classic_information_storage.hpp"
#include <filesystem>
#include <functional>
#include <mutex>

namespace sfx::platform::win32 {
void initialize_other_board_text(const std::filesystem::path&,const std::wstring& owner);
class WindowsOtherBoardTextStorage final:public application::IOtherBoardTextStorage {
public:
    using Log=std::function<bool(const application::OtherBoardLogRecord&)>;
    WindowsOtherBoardTextStorage(std::filesystem::path root,std::wstring owner,Log log)
        :root_(std::move(root)),owner_(std::move(owner)),log_(std::move(log)){}
    application::StorageResult<application::OtherBoardPage> read(std::int64_t after) override;
    application::StorageResult<application::OtherBoardLogRecord> change(
        const application::ClassicInformationChange&,const application::ClassicInformationActor&,std::string_view actor_name) override;
    bool publish_log(const application::OtherBoardLogRecord& record) noexcept override {
        try{return log_ && log_(record);}catch(...){return false;}
    }
private:
    std::filesystem::path root_;std::wstring owner_;Log log_;std::mutex mutex_;
};
}
