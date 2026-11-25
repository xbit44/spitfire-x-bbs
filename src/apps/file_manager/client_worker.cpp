#include "client_worker.hpp"
#include <algorithm>
namespace sfx::file_manager {
ClientWorker::ClientWorker(std::filesystem::path root,std::wstring owner,Factory factory):root_(std::move(root)),owner_(std::move(owner)),factory_(std::move(factory)),thread_([this]{run();}){}
ClientWorker::~ClientWorker(){stop();}
bool ClientWorker::submit(ClientCommand command){std::lock_guard<std::mutex> lock(mutex_);if(stopping_||active_||command_||reply_||!command.serial)return false;command_=std::move(command);wake_.notify_one();return true;}
std::optional<ClientReply> ClientWorker::poll(){std::lock_guard<std::mutex> lock(mutex_);if(!reply_)return {};auto result=std::move(reply_);reply_.reset();return result;}
bool ClientWorker::busy()const{std::lock_guard<std::mutex> lock(mutex_);return active_||command_.has_value()||reply_.has_value();}
void ClientWorker::stop(){
    {std::lock_guard<std::mutex> lock(mutex_);stopping_=true;command_.reset();wake_.notify_one();}
    if(thread_.joinable())thread_.join();
}
void ClientWorker::run(){
    for(;;){ClientCommand command;
        {std::unique_lock<std::mutex> lock(mutex_);wake_.wait(lock,[this]{return stopping_||command_.has_value();});if(stopping_)break;command=std::move(*command_);command_.reset();active_=true;}
        auto reply=perform(command);
        {std::lock_guard<std::mutex> lock(mutex_);active_=false;reply_=std::move(reply);}
    }
    // Offline ownership/storage and its native worker close on this same thread
    // only after the current bounded request has finished, never force-closed.
    if(client_)(void)client_->finish();client_.reset();
}
ClientReply ClientWorker::perform(const ClientCommand& command){
    ClientReply result;result.serial=command.serial;result.kind=command.kind;
    try{
        if(command.kind==ClientCommandKind::disconnect){if(client_)result.diagnostic=client_->finish();client_.reset();return result;}
        if(command.kind==ClientCommandKind::connect){client_.reset();client_=factory_(root_,owner_,result.diagnostic);}
        if(!client_){if(result.diagnostic.empty())result.diagnostic="Owner connection unavailable. Reconnect explicitly; no offline fallback attempted.";return result;}
        result.connected=true;result.online=client_->online();
        switch(command.kind){
        case ClientCommandKind::hatch_catalog:result.hatch_catalog=client_->ftn_hatch_catalog();if(!result.hatch_catalog)result.diagnostic="FTN hatch destinations are unavailable. No file was queued.";break;
        case ClientCommandKind::connect:
        case ClientCommandKind::configuration:result.configuration=client_->configuration();if(!result.configuration)result.diagnostic="Current file-area configuration is unavailable. No edits were attempted.";break;
        case ClientCommandKind::read:{
            if(!app::valid_file_management_read_request(command.query)){result.read_code=app::LocalAccountResultCode::invalid_request;break;}
            auto query=command.query;
            for(unsigned part=0;part<16;++part){const auto page=client_->read(query);result.read_code=page.code;if(!app::valid_file_management_read_snapshot(page)||page.code!=app::LocalAccountResultCode::ok||page.kind!=query.kind||(query.kind==app::FileManagementQueryKind::detail&&(page.records.size()!=1||!(page.records[0].file.id==query.selection)))){result.records.clear();result.pending.clear();result.next={};result.pending_next={};if(page.code==app::LocalAccountResultCode::ok)result.read_code=app::LocalAccountResultCode::unavailable;break;}
                if(query.kind!=app::FileManagementQueryKind::records&&query.kind!=app::FileManagementQueryKind::detail){result.pending=page.pending;result.pending_next=page.pending_next;break;}
                if(!page.records.empty()&&page.records.front().file.id.value<=query.after.value){result.records.clear();result.next={};result.read_code=app::LocalAccountResultCode::unavailable;break;}
                result.records.insert(result.records.end(),page.records.begin(),page.records.end());result.next=page.next;if(!page.next.value||query.kind==app::FileManagementQueryKind::detail)break;query.after=page.next;
            }
            break;
        }
        case ClientCommandKind::start:
            result.status=app::valid_file_management_task(command.task)?client_->start(command.task):app::FileManagementJobStatus{app::LocalAccountResultCode::invalid_request};break;
        case ClientCommandKind::status:result.status=client_->status(command.job);break;
        case ClientCommandKind::cancel:result.status=client_->cancel(command.job);break;
        case ClientCommandKind::disconnect:break; // handled before client availability
        }
        if((command.kind==ClientCommandKind::start||command.kind==ClientCommandKind::status||command.kind==ClientCommandKind::cancel)&&!app::valid_file_management_status(result.status)){result.status={};result.diagnostic="Owner job response is invalid. Keep the exact job for reconciliation.";}
    }catch(...){result.status={};result.read_code=app::LocalAccountResultCode::unavailable;result.configuration.reset();result.records.clear();result.pending.clear();result.next={};result.pending_next={};result.diagnostic="Owner link unavailable. An operation may have started; reconcile the same job before any new action.";}
    return result;
}
bool JobTracker::begin(const app::FileManagementTask& task){if(blocks_edits()||!app::valid_file_management_task(task))return false;task_=task;state_=JobTracking::unknown;return true;}
void JobTracker::link_failed()noexcept{if(task_&&state_!=JobTracking::idle)state_=JobTracking::unknown;}
bool JobTracker::observe(const app::FileManagementJobStatus& status,bool start_response){
    if(!task_||state_==JobTracking::idle)return false;
    if(!app::valid_file_management_status(status)||status.code==app::LocalAccountResultCode::unavailable){state_=JobTracking::unknown;return false;}
    if(status.code!=app::LocalAccountResultCode::ok){
        const bool refused=status.code==app::LocalAccountResultCode::unauthorized||status.code==app::LocalAccountResultCode::invalid_request||status.code==app::LocalAccountResultCode::conflict;
        if(start_response&&refused){state_=JobTracking::idle;return true;}state_=JobTracking::unknown;return false;
    }
    if(!(status.job==task_->job)||status.kind!=task_->kind){state_=JobTracking::unknown;return false;}
    state_=status.state==app::FileManagementJobState::running?JobTracking::running:JobTracking::idle;return true;
}
} // namespace sfx::file_manager
