#include "sfx/platform/linux/scheduler_files.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include "sfx/application/external_events.hpp"
#include "sfx/application/external_event_files.hpp"
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>

namespace sfx::platform::linux_host {
namespace {
namespace app=application;
const char* filename(std::string_view job,bool configuration) {
    if(job==app::kBackupJobId)return configuration?"sfxscheduler.conf":"sfxscheduler.state";
    if(job==app::kMessageRetentionJobId)return configuration?"sfxmessageretention.conf":"sfxmessageretention.state";
    if(job==app::kMessageIntegrityJobId)return configuration?"sfxmessageintegrity.conf":"sfxmessageintegrity.state";
    return nullptr;
}
template<class T,class Parser> SchedulerFileReadResult<T> read(const std::filesystem::path& root,
    std::string_view job,bool configuration,Parser parser) {
    const auto* name=filename(job,configuration);
    if(!name)return {SchedulerFileState::invalid,{},"scheduler_job_unknown"};
    const std::string label=configuration?"scheduler_configuration":"scheduler_state";
    try {
        auto directory=open_owned_directory(root/(configuration?"config":"data"));
        struct stat info{};
        if(fstatat(directory.get(),name,&info,AT_SYMLINK_NOFOLLOW)!=0)
            return {errno==ENOENT?SchedulerFileState::missing:SchedulerFileState::io_failure,{},label+(errno==ENOENT?"_missing":"_open_failed")};
        const auto raw=read_private_file(directory.get(),name,app::kSchedulerDocumentMaximumBytes);
        auto value=parser(std::string(raw.begin(),raw.end()));
        if(!value)return {SchedulerFileState::invalid,{},label+"_invalid"};
        if(value->job_id!=job)return {SchedulerFileState::invalid,{},"scheduler_job_binding_invalid"};
        return {SchedulerFileState::loaded,std::move(value),label+"_loaded"};
    }catch(const std::exception&){return {SchedulerFileState::invalid,{},label+"_unreadable_or_insecure"};}
}
template<class T,class Serialize,class Reader> app::StorageStatus publish(const std::filesystem::path& root,
    const T& value,bool configuration,Serialize serialize,Reader reader) {
    const auto* name=filename(value.job_id,configuration);const auto text=serialize(value);
    if(!name||text.empty()||text.size()>app::kSchedulerDocumentMaximumBytes)
        return {app::StorageErrorCode::invalid_argument,"scheduler_publication_invalid"};
    try {
        auto directory=open_owned_directory(root/(configuration?"config":"data"));
        publish_private_file(directory.get(),name,{text.begin(),text.end()},true);
        const auto saved=reader(root,value.job_id);
        if(saved.state!=SchedulerFileState::loaded||!saved.value||serialize(*saved.value)!=text)
            return {app::StorageErrorCode::io,"scheduler_publication_readback_failed"};
        return {};
    }catch(const std::exception&){return {app::StorageErrorCode::io,"scheduler_publication_failed_preserved"};}
}
}
SchedulerConfigurationFileReadResult read_scheduler_configuration_file(const std::filesystem::path& root,std::string_view job){
    return read<application::SchedulerConfiguration>(root,job,true,application::parse_scheduler_configuration);
}
SchedulerStateFileReadResult read_scheduler_state_file(const std::filesystem::path& root,std::string_view job){
    return read<application::SchedulerState>(root,job,false,application::parse_scheduler_state);
}
application::StorageStatus publish_scheduler_configuration_file(const std::filesystem::path& root,const application::SchedulerConfiguration& value){
    return publish(root,value,true,application::serialize_scheduler_configuration,read_scheduler_configuration_file);
}
application::StorageStatus publish_scheduler_state_file(const std::filesystem::path& root,const application::SchedulerState& value){
    return publish(root,value,false,application::serialize_scheduler_state,read_scheduler_state_file);
}
} // namespace sfx::platform::linux_host

namespace sfx::application {
namespace {
auto open_external_event_directory(const std::filesystem::path& root, ExternalEventFile file) {
    return platform::linux_host::open_owned_directory(root / (file == ExternalEventFile::catalog ? "config" : "data"));
}
const char* external_event_leaf(ExternalEventFile file) {
    return file == ExternalEventFile::catalog ? "sfxexternalevents.conf" : "sfxexternalevents.state";
}
}
std::optional<std::string> read_external_event_file(const std::filesystem::path& root,
    const std::wstring&, ExternalEventFile file) {
    try {
        auto directory = open_external_event_directory(root, file);
        const auto name = external_event_leaf(file);
        struct stat status{};
        if (fstatat(directory.get(), name, &status, AT_SYMLINK_NOFOLLOW) != 0)
            return errno == ENOENT ? std::optional<std::string>{""} : std::nullopt;
        const auto raw = platform::linux_host::read_private_file(directory.get(), name, kExternalEventFileLimit);
        return raw.empty() ? std::nullopt : std::optional<std::string>{{raw.begin(), raw.end()}};
    } catch (...) {
        return {};
    }
}
bool publish_external_event_file(const std::filesystem::path& root, const std::wstring& owner,
    ExternalEventFile file, std::string_view document) {
    if (document.empty() || document.size() > kExternalEventFileLimit || !read_external_event_file(root, owner, file))
        return false;
    try {
        auto directory = open_external_event_directory(root, file);
        platform::linux_host::publish_private_file(directory.get(), external_event_leaf(file),
            {document.begin(), document.end()}, true);
        return read_external_event_file(root, owner, file) == document;
    } catch (...) {
        return false;
    }
}
} // namespace sfx::application
