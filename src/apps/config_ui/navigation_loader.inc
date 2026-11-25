// Read-only worker protocol. No HWND control is read/written from this worker;
// only an epoch notification is posted. Results are owned in a bounded mailbox.
constexpr UINT kConfigNavigationMessage=WM_APP+42;
using NavigationKey=std::pair<app::LocalControlMessageType,std::vector<std::uint8_t>>;
struct NavigationCachedRead {bool ready{},ok{};app::LocalControlFrame response;};
struct NavigationContext {
    bool ok{},online{};
    std::string diagnostic;
    app::AlphaConfigurationSnapshot configuration;
    app::AlphaSchedulerStatus scheduler;
    std::optional<app::SchedulerConfiguration> backup;
};
struct NavigationResult {
    std::uint64_t epoch{};bool context{},finished{},ok{};
    NavigationContext state;NavigationKey key;app::LocalControlFrame response;
    std::string diagnostic;
};
class ConfigNavigationWorker final {
    struct Task {std::uint64_t epoch{};int page{};bool context{},finish{},release_for_service{};app::LocalControlFrame request;app::LocalControlMessageType expected{};};
    HWND window_;std::filesystem::path root_;std::wstring owner_;install::Receipt receipt_;
    std::mutex mutex_;std::condition_variable wake_;std::deque<Task> tasks_;std::deque<NavigationResult> results_;
    std::atomic<bool> stopping_{false};std::atomic<std::uint64_t> wanted_{0};std::thread worker_;
    std::unique_ptr<install::InstallationVerificationLease> verification_;
    std::unique_ptr<win32::OfflineConfigurationReadSession> offline_;
    std::uint64_t epoch_{};DWORD panel_{};bool context_ok_{};
    bool exchange(const app::LocalControlFrame& request,app::LocalControlMessageType expected,app::LocalControlFrame& response){
        if(stopping_||wanted_!=epoch_||!panel_)return false;
        verification_->verify_process(install::read_receipt(root_,owner_),panel_,install::control_panel_program_index);
        const auto names=win32::installation_object_names(root_);
        auto connection=win32::connect_local_control_server(names.configuration_pipe_name,panel_,1500);
        return connection.valid()&&connection.write_frame(request)&&connection.read_frame(response)&&response.type==expected&&response.request_id==request.request_id;
    }
    NavigationContext context(int page,bool release_for_service){
        NavigationContext out;offline_.reset();panel_=0;context_ok_=false;
        const auto current=install::read_receipt(root_,owner_);
        if(!verification_)verification_=std::make_unique<install::InstallationVerificationLease>(receipt_);
        verification_->verify_process(current,GetCurrentProcessId(),install::configuration_program_index);
        const auto panel=installed_window(current,kControlPanelClass,install::control_panel_program_index,verification_.get());panel_=panel.process_id;
        const bool stopped_service=[&]{try{const auto selected=install::read_service_selection(root_);return selected&&selected->enabled&&!selected->pending&&install::query_owned_service(*selected).state==SERVICE_STOPPED;}catch(...){return false;}}();
        if(stopped_service)panel_=0;
        if(panel_){
            app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::configuration_snapshot_request;request.request_id=1;
            const bool exchanged=exchange(request,app::LocalControlMessageType::configuration_snapshot_response,response);
            const auto value=exchanged?app::decode_configuration_snapshot(response.payload):std::nullopt;
            // Read status on the scheduler page or to distinguish first-run
            // and recovery from a failed configuration read.
            if(!value||page==4){
                std::uint32_t id=2;const auto status=app::read_status_snapshot(0,[&](const app::AlphaStatusRequest& q)->std::optional<std::vector<std::uint8_t>>{
                    app::LocalControlFrame r,reply;r.type=app::LocalControlMessageType::status_request;r.request_id=id++;r.payload=app::encode_status_request(q);
                    if(!exchange(r,app::LocalControlMessageType::status_response,reply))return {};return reply.payload;});
                if(!value){
                    if(status&&status->state==app::AlphaRuntimePresentationState::setup_required&&!status->bootstrap_complete)
                        out.diagnostic="setup_required";
                    else if(status&&!status->diagnostic_code.empty()&&status->state!=app::AlphaRuntimePresentationState::ready)
                        out.diagnostic=status->diagnostic_code;
                    else out.diagnostic=exchanged?"configuration_snapshot_unreadable":"control_panel_configuration_channel_unavailable";
                    return out;
                }
                if(!status){out.diagnostic="control_panel_scheduler_status_unavailable";return out;}
                out.scheduler=status->scheduler;
            }
            out.configuration=*value;out.online=true;
        }else{
            if(release_for_service){out.diagnostic="hosting_action_in_progress_retry_settings";return out;}
            win32::OfflineInstallationGuard guard;
            const auto locked=guard.acquire(root_,owner_);
            if(!locked){out.diagnostic=locked.operation;return out;}
            const auto config=win32::read_operational_configuration_file(root_);
            if(config.state!=win32::ConfigurationFileState::loaded||!config.configuration){
                out.diagnostic=config.diagnostic_code.empty()?"saved_configuration_unreadable":config.diagnostic_code;
                if(config.state==win32::ConfigurationFileState::missing){
                    const auto present=win32::alpha_installation_has_persistent_state(root_,owner_,[&](const auto& path){
                        return win32::stock_docs_bootstrap_metadata(root_,path,owner_)||
                            win32::clamav_bootstrap_metadata(root_,path,owner_);
                    });
                    if(present&&present.value&&!*present.value)out.diagnostic="setup_required";
                    else if(!present&&!present.status.operation.empty())out.diagnostic=present.status.operation;
                }
                return out;
            }
            out.configuration.active=out.configuration.saved=*config.configuration;out.configuration.activation=app::AlphaConfigurationActivation::saved_next_start;out.configuration.detail="Saved - applies on next Runtime start";
            if(page==4){const auto c=win32::read_scheduler_configuration_file(root_,owner_);const auto s=win32::read_scheduler_state_file(root_,owner_);
                if(c.value&&s.value){out.scheduler.configuration_loaded=true;out.scheduler.enabled=c.value->enabled;out.scheduler.configuration_revision=c.value->revision;out.scheduler.local_hour=c.value->local_hour;out.scheduler.local_minute=c.value->local_minute;out.scheduler.next_run_utc=s.value->next_run_utc;out.scheduler.active=s.value->active;out.scheduler.active_scheduled=s.value->active_scheduled;out.scheduler.active_run_id=s.value->active_run_id;out.scheduler.active_started_utc=s.value->active_started_utc;out.scheduler.last_run_utc=s.value->last_run_utc;out.scheduler.last_result=s.value->last_result;out.scheduler.last_detail=s.value->last_detail;out.scheduler.history=s.value->history;for(auto& item:out.scheduler.history)item.detail=app::bounded_scheduler_status_history_detail(item.detail);out.scheduler.diagnostic_code="scheduler_offline";}
            }
        }
        if(page==4)out.backup=win32::read_scheduler_configuration_file(root_,owner_).value;
        out.ok=context_ok_=true;out.diagnostic.clear();return out;
    }
    bool whole_ftn(app::LocalControlFrame& response){
        app::FtnConfigurationRequest q;win32::WindowsSecureIdentifierGenerator ids;q.token=ids.generate().bytes;
        std::vector<std::uint8_t> bytes;std::int64_t revision{};std::uint32_t total{},id=1;
        const auto request=[&](const app::FtnConfigurationRequest& value)->std::optional<app::FtnConfigurationResponse>{app::LocalControlFrame a,b;a.type=app::LocalControlMessageType::ftn_configuration_request;a.request_id=id++;a.payload=app::encode_ftn_configuration_request(value);if(!exchange(a,app::LocalControlMessageType::ftn_configuration_response,b))return {};return app::decode_ftn_configuration_response(b.payload);};
        const auto cancel=[&]{auto stop=q;stop.operation=app::FtnConfigurationOperation::cancel;stop.offset=0;(void)request(stop);};
        for(;;){
            const auto page=request(q);
            if(!page||page->code!=app::FtnConfigurationCode::ok||page->revision<1||page->offset!=bytes.size()||page->bytes.empty()||!page->total||(revision&&(page->revision!=revision||page->total!=total))){cancel();return false;}
            if(!revision){revision=page->revision;total=page->total;if(total>16U*1024U*1024U){cancel();return false;}bytes.reserve(total);}
            if(page->bytes.size()>total-bytes.size()){cancel();return false;}bytes.insert(bytes.end(),page->bytes.begin(),page->bytes.end());
            if(bytes.size()==total)break;q.offset=static_cast<std::uint32_t>(bytes.size());q.revision=revision;
        }
        cancel();const auto value=app::decode_ftn_configuration(bytes);if(!value||value->revision!=revision)return false;
        response.type=app::LocalControlMessageType::ftn_configuration_response;response.payload=std::move(bytes);return true;
    }
    bool read(const Task& task,app::LocalControlFrame& response){
        if(!context_ok_)return false;
        verification_->verify_process(install::read_receipt(root_,owner_),GetCurrentProcessId(),install::configuration_program_index);
        using T=app::LocalControlMessageType;
        switch(task.request.type){
        case T::account_query_request:case T::security_query_request:case T::message_configuration_query_request:case T::file_configuration_query_request:case T::classic_information_query_request:case T::qwk_policy_query_request:case T::chat_configuration_query_request:case T::door_configuration_query_request:break;
        case T::botgate_feed_status_request:if(!panel_)return false;break;
        case T::ftn_held_request:{const auto q=app::decode_ftn_held_request(task.request.payload);if(!q||q->index()>1)return false;break;}
        case T::ftn_configuration_request:if(!task.request.payload.empty())return false;break;
        default:return false; // Never run or retry an administrative write.
        }
        if(!panel_){if(!offline_)offline_=std::make_unique<win32::OfflineConfigurationReadSession>(root_,owner_);const auto result=offline_->query(task.request);if(!result||result->type!=task.expected)return false;response=*result;return true;}
        if(task.request.type==T::ftn_configuration_request&&task.request.payload.empty())return whole_ftn(response);
        return exchange(task.request,task.expected,response);
    }
    void run(){
        while(!stopping_){Task task;{std::unique_lock<std::mutex> lock(mutex_);wake_.wait(lock,[&]{return stopping_||!tasks_.empty();});if(stopping_)break;task=std::move(tasks_.front());tasks_.pop_front();}
            if(task.epoch!=wanted_)continue;
            NavigationResult result;result.epoch=task.epoch;result.context=task.context;result.finished=task.finish;result.key={task.request.type,task.request.payload};
            try{
                if(task.context){epoch_=task.epoch;result.state=context(task.page,task.release_for_service);result.ok=result.state.ok;}
                else if(task.finish){offline_.reset();result.ok=true;}
                else if(task.epoch==epoch_)result.ok=read(task,result.response);
            }catch(const std::exception& error){result.ok=false;result.diagnostic=std::string(error.what()).substr(0,160);result.state.diagnostic=result.diagnostic;offline_.reset();}
            catch(...){result.ok=false;result.diagnostic="navigation_read_failed";result.state.diagnostic=result.diagnostic;offline_.reset();}
            if(task.epoch!=wanted_)continue;
            {std::lock_guard<std::mutex> lock(mutex_);results_.push_back(std::move(result));}
            PostMessageW(window_,kConfigNavigationMessage,0,0);
        }
        offline_.reset();verification_.reset();
    }
public:
    ConfigNavigationWorker(HWND window,const std::filesystem::path& root,const std::wstring& owner,const install::Receipt& receipt):window_(window),root_(root),owner_(owner),receipt_(receipt),worker_([this]{run();}){}
    ~ConfigNavigationWorker(){stopping_=true;wake_.notify_all();if(worker_.joinable()){CancelSynchronousIo(worker_.native_handle());worker_.join();}}
    void begin(std::uint64_t epoch,int page,bool release_for_service=false){wanted_=epoch;std::lock_guard<std::mutex> lock(mutex_);tasks_.clear();results_.clear();Task task;task.epoch=epoch;task.page=page;task.context=true;task.release_for_service=release_for_service;tasks_.push_back(std::move(task));wake_.notify_one();}
    void submit(std::uint64_t epoch,const app::LocalControlFrame& request,app::LocalControlMessageType expected){std::lock_guard<std::mutex> lock(mutex_);if(tasks_.size()<1024){Task task;task.epoch=epoch;task.request=request;task.expected=expected;tasks_.push_back(std::move(task));wake_.notify_one();}}
    void finish(std::uint64_t epoch){std::lock_guard<std::mutex> lock(mutex_);Task task;task.epoch=epoch;task.finish=true;tasks_.push_back(std::move(task));wake_.notify_one();}
    // Invalidate queued/in-flight results and release an ordinary read context
    // without starting a new one for the report-only Health page.
    void suspend(std::uint64_t epoch){wanted_=epoch;std::lock_guard<std::mutex> lock(mutex_);tasks_.clear();results_.clear();Task task;task.epoch=epoch;task.finish=true;tasks_.push_back(std::move(task));wake_.notify_one();}
    std::deque<NavigationResult> take(){std::lock_guard<std::mutex> lock(mutex_);std::deque<NavigationResult> result;result.swap(results_);return result;}
};
