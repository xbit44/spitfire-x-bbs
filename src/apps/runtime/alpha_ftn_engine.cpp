#include "alpha_ftn_engine.hpp"
#include "sfx/platform/win32/portable_file_bindings.hpp"
#include "bink_log.hpp"
#include "sfx/application/ftn_nodelist.hpp"
#include "sfx/platform/win32/ftn_stored_scan.hpp"
#include "alpha_rlogin_resolver.hpp"
#include "sfx/platform/win32/ftn_native_toss.hpp"
#include "sfx/platform/win32/ftn_native_tic.hpp"
#include "sfx/platform/win32/ftn_processed_expiry.hpp"
#include "ftn_external_files.hpp"
#include "sfx/platform/win32/ftn_native_custody.hpp"
#include <sodium.h>
#include <algorithm>
#include <map>

namespace sfx::apps::runtime {
namespace {
namespace app=application;
namespace native=platform::win32;
namespace ins=native::installation;
using Clock=std::chrono::steady_clock;
std::string diagnostic(const std::exception& error) {
    const std::string text=error.what();
    if(text.size()>96 || (text.rfind("ftn_",0)!=0 && text.rfind("installation_",0)!=0))return "ftn_local_operation_failed";
    if(!std::all_of(text.begin(),text.end(),[](char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';}))return "ftn_local_operation_failed";
    return text;
}
bool enabled(const app::FtnConfiguration& c,const app::FtnLink& link) {
    return link.enabled&&app::ftn_outbound(link)&&std::any_of(c.networks.begin(),c.networks.end(),[&](const auto& n){return n.enabled&&n.code==link.network;});
}
std::string key(std::string_view network,std::string_view link){return std::string(network)+"/"+std::string(link);}
#include "ftn_external_runtime.inc"
app::FtnLink dial_endpoint(const app::FtnConfiguration& c,const app::FtnLink& original,const ins::Receipt& receipt,const std::function<bool()>& running){
    if(!original.host.empty()&&original.port)return original;
    const auto n=std::find_if(c.networks.begin(),c.networks.end(),[&](const auto& net){return net.code==original.network;});
    ins::require(n!=c.networks.end()&&!n->nodelist_file.empty(),"ftn_nodelist_not_configured");
    auto path=native::resolve_portable_owned_path(receipt.root,std::filesystem::u8path(n->nodelist_file));
    ins::Handle input(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
    ins::require(input&&GetFileInformationByHandle(input.get(),&info)&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&GetFileSizeEx(input.get(),&size)&&size.QuadPart>=0&&size.QuadPart<=32*1024*1024,"ftn_nodelist_unavailable");
    std::string text(static_cast<std::size_t>(size.QuadPart),'\0');std::size_t done{};while(done<text.size()){ins::require(running(),"ftn_session_cancelled");DWORD read{};const auto count=static_cast<DWORD>(std::min<std::size_t>(65536,text.size()-done));ins::require(ReadFile(input.get(),text.data()+done,count,&read,nullptr)&&read==count,"ftn_nodelist_read_failed");done+=read;}
    const auto address=app::parse_ftn_address(original.address);ins::require(address.has_value(),"ftn_nodelist_address_invalid");auto own=app::parse_ftn_address(n->address);ins::require(!own||own->domain.empty()||address->domain.empty()||own->domain==address->domain,"ftn_nodelist_domain_mismatch");const auto endpoint=app::ftn_nodelist_endpoint(text,*address);ins::require(endpoint.has_value(),"ftn_nodelist_endpoint_missing_or_ambiguous");auto result=original;if(result.host.empty())result.host=endpoint->host;if(!result.port)result.port=endpoint->port;return result;
}

}
AlphaFtnEngine::AlphaFtnEngine(ins::Receipt receipt,app::IFtnMailStorage& storage,app::IFileStorage& files,
    app::Identifier128 generation,std::shared_ptr<app::EventSoundStream> sounds)
    :receipt_(std::move(receipt)),storage_(storage),files_(files),generation_(generation),sounds_(std::move(sounds)) {
    view_.capable=receipt_.ftn_layout;
    thread_=std::thread([this]{run();});
}
AlphaFtnEngine::~AlphaFtnEngine(){stop();}
void AlphaFtnEngine::set_bbs_time_zone(std::string_view zone) {
    auto next = std::make_shared<native::WindowsBbsLogTimestamp>();
    (void)next->configure(zone);
    std::lock_guard<std::mutex> lock(mutex_);
    timezone_ = std::move(next);
    ++change_;
    cancel_ = true;
    *retention_cancel_ = true;
    wake_.notify_all();
}
void AlphaFtnEngine::configure(app::FtnConfiguration value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(stopping_)return;
    // Saved revisions are monotonic for this Runtime lifetime; an offline
    // restore starts a new owner. A delayed AreaFix refresh cannot undo a newer
    // already-published owner configuration.
    if(wanted_&&value.revision<wanted_->revision)return;
    wanted_=std::move(value);++change_;cancel_=true;*retention_cancel_=true;polls_.clear();
    view_.saved_revision=wanted_->revision;view_.mode=wanted_->settings.mode;
    wake_.notify_all();
}
AlphaFtnSnapshot AlphaFtnEngine::snapshot() const {std::lock_guard<std::mutex> lock(mutex_);return view_;}
void AlphaFtnEngine::activity_locked(std::uint64_t session,std::string_view network,std::string_view link,std::string message){
    const auto utc=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if(view_.activity.size()>=32)view_.activity.erase(view_.activity.begin());
    view_.activity.push_back({static_cast<std::uint64_t>(utc),session,std::string(network),std::string(link),message});
    std::string network_name="Unidentified network",link_name="Unconfigured peer";
    if(wanted_){for(const auto& n:wanted_->networks)if(n.code==network)network_name=n.name;for(const auto& l:wanted_->links)if(l.network==network&&l.code==link)link_name=l.name;}
    try {append_bink_log(receipt_.root,receipt_.owner,utc,session,network_name,link_name,message);}
    catch(...) {if(view_.activity.size()>=32)view_.activity.erase(view_.activity.begin());view_.activity.push_back({static_cast<std::uint64_t>(utc),session,std::string(network),std::string(link),"Warning: could not write Logs/bink.log. Check log folder access or free space."});}
}
bool AlphaFtnEngine::poll(std::string_view network,std::string_view link,bool hold) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(stopping_||backup_hold_||view_.serial_recovery_required||!wanted_||!receipt_.ftn_layout||wanted_->settings.mode!=app::FtnMailerMode::native||polls_.size()>=64)return false;
    const auto found=std::find_if(wanted_->links.begin(),wanted_->links.end(),[&](const auto& l){return l.network==network&&l.code==link&&enabled(*wanted_,l);});
    if(found==wanted_->links.end())return false;
    const auto pending=std::find_if(polls_.begin(),polls_.end(),[&](const auto& p){return p.network==network&&p.link==link;});
    if(pending!=polls_.end())pending->hold=pending->hold||hold;
    else polls_.push_back({std::string(network),std::string(link),hold});
    scan_requested_=true;activity_locked(0,network,link,"Poll requested; waiting for a connection slot.");
    wake_.notify_all();return true;
}
bool AlphaFtnEngine::pause_for_backup() {
    std::unique_lock<std::mutex> lock(mutex_);
    if(stopping_||backup_hold_)return false;
    backup_hold_=true;cancel_=true;*retention_cancel_=true;wake_.notify_all();
    if(wake_.wait_for(lock,std::chrono::seconds(30),[this]{return quiescent_||stopping_;})&&!stopping_)return true;
    backup_hold_=false;++change_;wake_.notify_all();return false;
}
bool AlphaFtnEngine::scan(){
    std::lock_guard<std::mutex> lock(mutex_);
    if(stopping_||backup_hold_||view_.serial_recovery_required||!wanted_||!receipt_.ftn_layout||wanted_->settings.mode==app::FtnMailerMode::disabled||view_.active_revision!=wanted_->revision)return false;
    scan_requested_=true;wake_.notify_all();return true;
}
void AlphaFtnEngine::resume_after_backup() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);backup_hold_=false;++change_;wake_.notify_all();
}
void AlphaFtnEngine::stop() noexcept {
    stopping_=true;cancel_=true;*retention_cancel_=true;wake_.notify_all();
    if(thread_.joinable())thread_.join();
}
void AlphaFtnEngine::run() noexcept {
    struct Worker {std::thread thread;std::shared_ptr<std::atomic_bool> done;std::string outbound;};
    std::vector<Worker> workers;
    std::thread processor;
    std::atomic_bool processor_done{true},processor_work{true};
    unsigned quiet_passes{};
    ExternalFtnProgress external_progress;
    app::Identifier128 cursor,control_cursor,rescan_cursor,reprocess_cursor,tic_retry_cursor,retention_cursor;
    std::map<std::string,std::int64_t> scan_cursors;
    std::map<std::string,std::string> processing_problems;
    const auto report_processing_locked=[&](std::string key,const std::string& why,std::string_view network,std::string_view link){
        const auto old=processing_problems.find(key);
        if(why.empty()){if(old!=processing_problems.end()){activity_locked(0,network,link,key+": processing resumed.");processing_problems.erase(old);}return;}
        if(old==processing_problems.end()||old->second!=why){
            activity_locked(0,network,link,key+": "+(why=="ftn_scan_real_name_required"?"Set a valid Real Name for this author; the echo requires it. Message retained.":why));
            if(processing_problems.size()<4096||old!=processing_problems.end())processing_problems[key]=why;
        }
    };
    std::size_t scan_network{};std::string stored_after,stored_error;
    auto next_processing=Clock::now();
    std::unique_ptr<native::FtnNativeListener> classic,tls;
    const auto retire=[&] {
        cancel_=true;*retention_cancel_=true;classic.reset();tls.reset();
        for(auto& worker:workers)if(worker.thread.joinable())worker.thread.join();
        workers.clear();
        if(processor.joinable())processor.join();
        std::lock_guard<std::mutex> lock(mutex_);
        view_.classic_listening=view_.tls_listening=false;view_.classic_port=view_.tls_port=0;view_.active_revision=0;
        for(auto& session:view_.sessions)session.active=false;
        view_.processing=false;
        quiescent_=true;wake_.notify_all();
    };
    try {
        std::uint64_t applied{},serial{};
        std::optional<app::FtnConfiguration> active;
        std::shared_ptr<const native::WindowsBbsLogTimestamp> timezone;
        std::optional<native::FilePayloadDirectory> inbound,outbound;
        using Directories=std::map<std::string,std::pair<native::FilePayloadDirectory,native::FilePayloadDirectory>>;
        Directories directories;
        std::map<std::string,Clock::time_point> due;
        std::map<std::string,Clock::time_point> mail_due,mail_probe;
        bool recovered{};
        while(!stopping_) {
            std::optional<app::FtnConfiguration> next;std::uint64_t revision{};bool hold{};
            {std::lock_guard<std::mutex> lock(mutex_);revision=change_;hold=backup_hold_;if(wanted_&&(hold||applied!=revision||!active))next=*wanted_;}
            if(hold||applied!=revision||!active) {
                retire();inbound.reset();outbound.reset();directories.clear();active.reset();
                if(stopping_)break;
                {std::unique_lock<std::mutex> lock(mutex_);
                    if(backup_hold_||!wanted_){view_.paused=backup_hold_;view_.diagnostic=backup_hold_?"ftn_paused_for_backup":"ftn_waiting_configuration";
                        wake_.wait_for(lock,std::chrono::milliseconds(100));continue;}
                    next=*wanted_;timezone=timezone_;revision=change_;cancel_=false;*retention_cancel_=false;quiescent_=false;view_.paused=false;
                    view_.classic_error.clear();view_.tls_error.clear();
                }
                active=std::move(next);applied=revision;due.clear();cursor={};next_processing=Clock::now();
                const auto id_history=storage_.ftn_serial_state({true,{}});
                {std::lock_guard<std::mutex> lock(mutex_);view_.serial_recovery_required=!id_history||!id_history.value||id_history.value->recovery_required;}
                const auto inactive=[&](const char* reason){std::lock_guard<std::mutex> lock(mutex_);view_.diagnostic=reason;quiescent_=true;wake_.notify_all();};
                if(!receipt_.ftn_layout){inactive("ftn_needs_normal_setup_update");}
                else if(active->settings.mode==app::FtnMailerMode::disabled){inactive("ftn_disabled_mail_retained");}
                else if(!id_history||!id_history.value||id_history.value->recovery_required){inactive("ftn_message_id_history_needs_recovery");}
                else if(!std::any_of(active->networks.begin(),active->networks.end(),[](const auto& n){return n.enabled;})){inactive("ftn_no_enabled_network");}
                else {
                    try {
                        ins::verify_ftn_directory(receipt_);
                        // Pins the verified root and ancestors against replacement
                        // before any ordinary child creation or file custody.
                        auto root=native::FilePayloadDirectory::private_store(receipt_.root/L"FTN",receipt_.owner);
                        const auto sd=ins::mutable_descriptor(receipt_.owner);
                        const auto child=[&](const std::filesystem::path& path) {
                            const auto attrs=GetFileAttributesW(path.c_str());
                            if(attrs==INVALID_FILE_ATTRIBUTES){ins::require(GetLastError()==ERROR_FILE_NOT_FOUND,"ftn_directory_unavailable");ins::create_directory(path,sd.get());}
                            return native::FilePayloadDirectory::private_store(path,receipt_.owner);
                        };
                        for(const auto* leaf:{L"Inbound",L"Outbound",L"Nonsecure",L"Bad",L"Processed"})(void)child(root.path()/leaf);
                        native::migrate_ftn_inbound(storage_,receipt_,[&]{return !cancel_.load();});
                        (void)child(receipt_.root/L"FTN"/L"Partial");
                        for(const auto& network:active->networks)if(network.enabled) {
                            ins::require(app::valid_ftn_code(network.code),"ftn_network_directory_invalid");
                            const auto code=std::filesystem::u8path(network.code);
                            auto incoming=child(receipt_.root/L"FTN"/L"Partial"/code),outgoing=child(root.path()/L"Outbound"/code);
                            (void)child(root.path()/L"Bad"/code);(void)child(root.path()/L"Processed"/code);
                            directories.emplace(network.code,std::make_pair(std::move(incoming),std::move(outgoing)));
                            if(active->settings.mode==app::FtnMailerMode::external_binkd)external::prepare_profile(receipt_.root,network.code);
                        }
                        inbound=native::FilePayloadDirectory::private_store(root.path()/L"Nonsecure",receipt_.owner);
                        outbound=native::FilePayloadDirectory::private_store(root.path()/L"Outbound",receipt_.owner);
                        if(!recovered){ins::require(storage_.recover_ftn_claims({true,{}},generation_).succeeded(),"ftn_claim_recovery_failed");recovered=true;}
                        const auto listen=[&](bool encrypted) {
                            try {
                                auto listener=native::FtnNativeListener::open(active->settings.bind_address,encrypted?active->settings.tls_port:active->settings.port,encrypted,active->settings,receipt_.owner);
                                std::lock_guard<std::mutex> lock(mutex_);
                                if(encrypted){view_.tls_port=listener->port();view_.tls_listening=true;tls=std::move(listener);}
                                else{view_.classic_port=listener->port();view_.classic_listening=true;classic=std::move(listener);}
                            }catch(const std::exception& error){std::lock_guard<std::mutex> lock(mutex_);(encrypted?view_.tls_error:view_.classic_error)=diagnostic(error);activity_locked(0,{},{},std::string(encrypted?"TLS listener failed: ":"Classic listener failed: ")+diagnostic(error));}
                        };
                        if(active->settings.mode==app::FtnMailerMode::native&&active->settings.listener_enabled)listen(false);
                        if(active->settings.mode==app::FtnMailerMode::native&&active->settings.tls_listener_enabled)listen(true);
                        {std::lock_guard<std::mutex> lock(mutex_);view_.active_revision=active->revision;view_.diagnostic=active->settings.mode==app::FtnMailerMode::external_binkd?"ftn_external_processing_active":(view_.classic_error.empty()&&view_.tls_error.empty())?"ftn_native_active":"ftn_native_listener_failure";activity_locked(0,{},{} ,active->settings.mode==app::FtnMailerMode::external_binkd?"External BinkD selected. SFX processes mail; BinkD owns transport.":"BinkP configuration activated; inbound migration verified.");}
                    }catch(const std::exception& error){inbound.reset();outbound.reset();std::lock_guard<std::mutex> lock(mutex_);view_.diagnostic=diagnostic(error);activity_locked(0,{},{} ,"BinkP startup stopped: "+diagnostic(error));quiescent_=true;wake_.notify_all();}
                }
            }
            for(auto i=workers.begin();i!=workers.end();) {
                if(i->done->load()){i->thread.join();i=workers.erase(i);}else ++i;
            }
            if(inbound&&outbound&&!cancel_&&!stopping_) {
                if(processor_done.load()&&(Clock::now()>=next_processing||scan_requested_.load())) {
                    const bool requested=scan_requested_.exchange(false);if(requested){scan_cursors.clear();scan_network=0;}
                    if(processor.joinable())processor.join();
                    quiet_passes=requested||processor_work.load()?0:std::min(quiet_passes+1,3U);
                    const auto delay=std::min(5U,1U<<quiet_passes);
                    processor_done=false;processor_work=false;next_processing=Clock::now()+std::chrono::seconds(delay);
                    processor=std::thread([this,&processor_work,&processor_done,&cursor,&control_cursor,&rescan_cursor,&reprocess_cursor,&tic_retry_cursor,&retention_cursor,&scan_cursors,&scan_network,&stored_after,&stored_error,&processing_problems,&report_processing_locked,&external_progress,scoped=directories,config=*active,timezone] {
                        std::string background_error;
                        const auto running=[this]{return !stopping_.load()&&!cancel_.load();};
                        {std::lock_guard<std::mutex> lock(mutex_);view_.processing=true;}
                        const ExternalReport external_report=[&](std::string label,const std::string& why,std::string_view network,std::string_view link){std::lock_guard<std::mutex> lock(mutex_);report_processing_locked(std::move(label),why,network,link);};
                        try {
                            if(config.settings.mode==app::FtnMailerMode::external_binkd&&running()){
                                auto completed=native::FilePayloadDirectory::private_store(receipt_.root/L"FTN"/L"Inbound",receipt_.owner);
                                auto nonsecure=native::FilePayloadDirectory::private_store(receipt_.root/L"FTN"/L"Nonsecure",receipt_.owner);
                                if(receive_external_page(receipt_.root,storage_,config,scoped,completed,nonsecure,external_progress,running,external_report))processor_work=true;
                                if(running()&&receive_external_page(receipt_.root,storage_,config,scoped,completed,nonsecure,external_progress,running,external_report,true))processor_work=true;
                            }
                            const auto page=storage_.pending_ftn_ingress_after({true,{}},16,cursor);
                            ins::require(page&&page.value,"ftn_pending_receipts_unavailable");
                            
                            for(const auto& receipt:*page.value) {
                                if(!running())break;
                                auto suffix=receipt.file.name.size()>4?receipt.file.name.substr(receipt.file.name.size()-4):std::string{};
                                for(auto& c:suffix)if(c>='A'&&c<='Z')c=static_cast<char>(c+32);
                                const auto result=suffix==".tic"
                                    ?native::publish_ftn_native_tic(storage_,files_,config,receipt_,receipt,running)
                                    :native::toss_ftn_native_receipt(storage_,config,receipt_,receipt,running);
                                if(result.processed||result.held||result.imported||result.replayed||result.configuration_changed)processor_work=true;
                                cursor=receipt.id;
                                {std::lock_guard<std::mutex> lock(mutex_);
                                view_.processed_receipts+=result.processed?1:0;view_.held_receipts+=result.held?1:0;
                                view_.imported_messages+=result.imported;view_.replayed_messages+=result.replayed;
                                report_processing_locked("Inbound "+receipt.file.name,result.reason,receipt.network,receipt.link);
                                for(const auto& note:result.diagnostics)activity_locked(0,receipt.network,receipt.link,note);
                                if(result.processed||result.held)activity_locked(0,receipt.network,receipt.link,"Processed "+receipt.file.name+": "+std::to_string(result.imported)+" imported, "+std::to_string(result.replayed)+" already committed; "+(result.held?(result.reason=="ftn_inbound_file_missing_or_changed"?"source unavailable; automatic retries stopped.":"original retained for held messages."):"completed."));}
                                if(result.configuration_changed){const auto current=storage_.ftn_configuration({true,{}});ins::require(current&&current.value,"ftn_areafix_reload_failed");configure(*current.value);break;}
                            }
                            if(page.value->size()<16)cursor={};
                            if(running()){
                                const auto retries=storage_.pending_ftn_tic_retries({true,{}},tic_retry_cursor,4);ins::require(retries&&retries.value,"ftn_tic_retry_queue_unavailable");
                                for(const auto& retry:*retries.value){if(!running())break;const auto result=native::reprocess_ftn_native_tic(storage_,files_,config,receipt_,retry,running);if(result.processed||result.held||result.configuration_changed)processor_work=true;tic_retry_cursor=retry.request.operation;{std::lock_guard<std::mutex> lock(mutex_);view_.processed_receipts+=(result.processed&&!result.completion_reconciled)?1:0;view_.held_receipts+=result.held?1:0;background_error=result.reason;}if(result.configuration_changed){const auto current=storage_.ftn_configuration({true,{}});ins::require(current&&current.value,"ftn_tic_retry_reload_failed");configure(*current.value);break;}}
                                if(retries.value->size()<4)tic_retry_cursor={};
                            }
                            if(running()){
                                const auto retries=storage_.pending_ftn_reprocess({true,{}},reprocess_cursor,4);ins::require(retries&&retries.value,"ftn_reprocess_queue_unavailable");
                                for(const auto& retry:*retries.value){if(!running())break;const auto result=native::reprocess_ftn_native_receipt(storage_,config,receipt_,retry.request.operation,running);if(result.processed||result.held||result.imported||result.replayed||result.configuration_changed)processor_work=true;reprocess_cursor=retry.request.operation;
                                    const auto original=storage_.ftn_ingress_receipt({true,{}},retry.request.ingress);
                                    {std::lock_guard<std::mutex> lock(mutex_);view_.processed_receipts+=(result.processed&&!result.completion_reconciled)?1:0;view_.held_receipts+=result.held?1:0;view_.imported_messages+=result.imported;view_.replayed_messages+=result.replayed;background_error=result.reason;
                                    if(original&&original.value)report_processing_locked("Inbound "+original.value->file.name,result.reason,original.value->network,original.value->link);
                                    for(const auto& note:result.diagnostics)activity_locked(0,original&&original.value?original.value->network:std::string{},original&&original.value?original.value->link:std::string{},note);
                                    if(result.processed||result.held)activity_locked(0,original&&original.value?original.value->network:std::string{},original&&original.value?original.value->link:std::string{},"Reviewed retry: "+std::to_string(result.imported)+" imported, "+std::to_string(result.replayed)+" already committed; "+(result.held?"original retained for held messages.":"completed."));}
                                    if(result.configuration_changed){const auto current=storage_.ftn_configuration({true,{}});ins::require(current&&current.value,"ftn_reprocess_reload_failed");configure(*current.value);break;}
                                }
                                if(retries.value->size()<4)reprocess_cursor={};
                            }
                            if(running()){
                                const auto controls=storage_.pending_ftn_control({true,{}},control_cursor,4);ins::require(controls&&controls.value,"ftn_control_intents_unavailable");
                                for(const auto& control:*controls.value){if(!running())break;const auto result=native::scan_ftn_native_control(storage_,config,receipt_,control,running,timezone.get());if(result.processed)processor_work=true;control_cursor=control.text.operation;std::lock_guard<std::mutex> lock(mutex_);view_.scanned_messages+=result.processed?1:0;if(!result.reason.empty())background_error=result.reason;}
                                if(controls.value->size()<4)control_cursor={};
                            }
                            if(running()){
                                const auto pending=storage_.next_ftn_rescan({true,{}},rescan_cursor);ins::require(pending&&pending.value,"ftn_rescan_read_unavailable");
                                if(pending.value->has_value()){const auto& item=**pending.value;const auto result=native::scan_ftn_native_rescan(storage_,config,receipt_,item,running,timezone.get());if(result.processed)processor_work=true;rescan_cursor=item.request;std::lock_guard<std::mutex> lock(mutex_);view_.scanned_messages+=result.processed&&item.arrival&&item.text.reason.empty()?1:0;if(!result.reason.empty())background_error=result.reason;}else rescan_cursor={};
                            }
                            if(running()&&(config.settings.msg_netmail_enabled||std::any_of(config.networks.begin(),config.networks.end(),[](const auto& n){return n.enabled&&n.msg_netmail_enabled.value_or(false);}))) {
                                const auto result=native::scan_ftn_stored_messages(storage_,config,receipt_,stored_after,running);if(result.queued||result.completed)processor_work=true;
                                std::lock_guard<std::mutex> lock(mutex_);
                                if(result.queued)activity_locked(0,{}, {},"Queued "+std::to_string(result.queued)+" stored .MSG file-attach request(s).");
                                if(result.completed)activity_locked(0,{}, {},"Delivery acknowledged; completed "+std::to_string(result.completed)+" stored .MSG request(s).");
                                if(result.failed)background_error=result.reason;if(result.failed&&result.reason!=stored_error){activity_locked(0,{}, {},"Stored .MSG request retained: "+result.reason);background_error=result.reason;}
                                stored_error=result.reason;
                            }
                            if(running()&&!config.networks.empty()) {
                                const auto& network=config.networks[scan_network++%config.networks.size()];
                                if(network.enabled) {
                                    auto& after=scan_cursors[network.code];
                                    const auto scan=storage_.pending_ftn_scan({true,{}},network.code,after,4);
                                    ins::require(scan&&scan.value,"ftn_scan_intents_unavailable");
                                    for(const auto& intent:*scan.value) {
                                        if(!running())break;
                                        const auto result=native::scan_ftn_native_intent(storage_,config,receipt_,intent,running,timezone.get());
                                        if(result.processed)processor_work=true;after=intent.message.value;
                                        std::lock_guard<std::mutex> lock(mutex_);view_.scanned_messages+=result.processed?1:0;
                                        report_processing_locked("Outbound message #"+std::to_string(intent.message.value),result.reason,network.code,{});
                                    }
                                    if(scan.value->size()<4)after=0;
                                }
                            }
                            if(config.settings.mode==app::FtnMailerMode::external_binkd&&running()){
                                if(publish_external_page(receipt_.root,storage_,config,scoped,generation_,external_progress,running,external_report))processor_work=true;
                            }
                            if(running()){
                                const auto retained=native::process_ftn_retention_page(storage_,receipt_,retention_cancel_,retention_cursor);if(retained.examined)processor_work=true;
                                if(!retained.reason.empty()){std::lock_guard<std::mutex> lock(mutex_);background_error=retained.reason;}
                            }
                        }catch(const std::exception& error){std::lock_guard<std::mutex> lock(mutex_);background_error=diagnostic(error);}
                        {std::lock_guard<std::mutex> lock(mutex_);report_processing_locked("Background processing",background_error,{},{});if(!processing_problems.empty())view_.processing_error=std::to_string(processing_problems.size())+" processing issue(s). See the activity log for affected files/messages.";else view_.processing_error.clear();view_.processing=false;}
                        processor_done=true;wake_.notify_all();
                    });
                }
                if(active->settings.mode==app::FtnMailerMode::native){
                const auto launch=[&](std::unique_ptr<native::FtnNativeChannel> channel,std::optional<app::FtnLink> link,bool include_hold) {
                    const auto number=++serial;auto done=std::make_shared<std::atomic_bool>(false);
                    {std::lock_guard<std::mutex> lock(mutex_);
                        while(view_.sessions.size()>=16){const auto old=std::find_if(view_.sessions.begin(),view_.sessions.end(),[](const auto& s){return !s.active;});if(old==view_.sessions.end())break;view_.sessions.erase(old);}
                        AlphaFtnSessionView row;row.serial=number;row.active=true;if(link){row.transfer.outbound=true;row.transfer.network=link->network;row.transfer.link=link->code;}view_.sessions.push_back(std::move(row));
                    }
                    Worker worker;worker.done=done;worker.outbound=link?key(link->network,link->code):std::string{};
                    // Reserve the vector slot first; an allocation failure must
                    // never destroy a joinable thread and terminate Runtime.
                    workers.push_back(std::move(worker));
                    workers.back().thread=std::thread([this,number,done,config=*active,in=*inbound,out=*outbound,scoped=directories,channel=std::move(channel),link=std::move(link),include_hold]() mutable {
                        auto running=[this]{return !stopping_.load()&&!cancel_.load();};
                        native::FtnNativeSessionResult result;bool announced{};
                        const auto activity=[&](std::string message){std::lock_guard<std::mutex> lock(mutex_);activity_locked(number,result.network,result.link,std::move(message));};
                        const auto observe=[&](const auto& state){
                            if(!announced&&state.handshake_complete&&!state.outbound&&!state.link.empty()){announced=true;if(sounds_)sounds_->publish(app::SoundEvent::inbound_binkp,GetTickCount64());}
                            std::lock_guard<std::mutex> lock(mutex_);for(auto& row:view_.sessions)if(row.serial==number){
                                const auto& previous=row.transfer;
                                const auto note=[&](std::string message){activity_locked(number,state.network,state.link,std::move(message));};
                                if(!state.link.empty()&&previous.link.empty()&&!state.handshake_complete)note("Address matched configured connection; authentication pending.");
                                if(state.handshake_complete&&!previous.handshake_complete)note("BinkP handshake accepted; exchanging mail.");
                                if(state.peer_verified&&!previous.peer_verified)note("Peer identity verified.");
                                if(state.send.active&&(!previous.send.active||previous.send.name!=state.send.name))note("Sending "+state.send.name+" ("+std::to_string(state.send.size)+" bytes).");
                                if(state.send.awaiting_ack&&!previous.send.awaiting_ack)note("File sent; waiting for peer acknowledgement.");
                                if(state.sent_files>previous.sent_files)note("Peer acknowledged delivery; "+std::to_string(state.sent_files)+" file(s) sent.");
                                if(state.receive.active&&(!previous.receive.active||previous.receive.name!=state.receive.name))note("Receiving "+state.receive.name+" ("+std::to_string(state.receive.size)+" bytes).");
                                if(state.received_files>previous.received_files)note("Received "+std::to_string(state.received_files)+" file(s).");
                                if(state.reason=="ftn_inbound_name_collision"&&state.reason!=previous.reason)note("Inbound filename already exists; conflicting arrival skipped and retained by sender.");
                                row.transfer=state;
                            }
                        };
                        try {
                            std::optional<app::FtnNodelistEndpoint> endpoint_binding;
                            if(link) {
                                result.outbound=true;result.network=link->network;result.link=link->code;
                                activity("Resolving hub address.");
                                const auto endpoint=dial_endpoint(config,*link,receipt_,running);
                                endpoint_binding=app::FtnNodelistEndpoint{endpoint.host,endpoint.port};
                                const auto addresses=resolve_alpha_rlogin_ipv4(endpoint.host,running);
                                ins::require(!addresses.empty(),"ftn_dns_resolution_failed");
                                activity("Connecting to "+(endpoint.host.size()<=128?endpoint.host:std::string("configured host"))+":"+std::to_string(endpoint.port)+".");
                                for(const auto& address:addresses){try{channel=native::FtnNativeChannel::connect_ipv4(address,endpoint.port,endpoint,receipt_.owner,running);break;}catch(const std::exception&){if(!running())throw;}}
                                ins::require(bool(channel),"ftn_connection_failed");
                            }
                            activity(link?"Connected to peer.":"Inbound connection accepted.");
                            if(link&&link->tls)activity("Negotiating TLS.");
                            channel->handshake(running);
                            activity("Starting BinkP handshake and authentication.");
                            native::FtnNativeSessionPorts ports{storage_,config,in,out,receipt_.root.u8string(),generation_,{},include_hold,running,observe};
                            // Bind only the resolved endpoint in this session's
                            // owner-held snapshot. Peer addresses, credentials,
                            // TLS policy and authoritative revision stay exact;
                            // the native channel still must match this peer.
                            if(link&&endpoint_binding)for(auto& peer:ports.configuration.links)if(peer.network==link->network&&peer.code==link->code){peer.host=endpoint_binding->host;peer.port=endpoint_binding->port;}

                            ports.completed_inbound=native::FilePayloadDirectory::private_store(receipt_.root/L"FTN"/L"Inbound",receipt_.owner);
                            ports.directory=[scoped,in](std::string_view network,bool sending){
                                if(network.empty()&&!sending)return in;
                                const auto found=scoped.find(std::string(network));
                                ins::require(found!=scoped.end(),"ftn_network_directory_unavailable");
                                return sending?found->second.second:found->second.first;
                            };
                            if(link)ports.dialled_link=app::FtnPeerMatch{link->network,link->code};
                            result=native::run_ftn_native_session(*channel,std::move(ports));
                        }catch(const std::exception& error){result.reason=diagnostic(error);}
                        if(link){result.outbound=true;result.network=link->network;result.link=link->code;}
                        observe(result);
                        if(result.received_files||result.sent_files)scan_requested_=true;
                        if(result.completed)activity("Session completed: "+std::to_string(result.sent_files)+" sent, "+std::to_string(result.received_files)+" received.");
                        else{
                            auto reason=result.reason;
                            if(reason=="ftn_connection_failed")reason="Could not connect to the configured host and port";
                            else if(reason=="ftn_dns_resolution_failed")reason="Hub address could not be resolved";
                            else if(reason=="ftn_session_peer_closed")reason="Peer closed the connection";
                            else if(reason=="ftn_session_handshake_timeout")reason="BinkP handshake timed out";
                            else if(reason=="ftn_session_cancelled")reason="Session cancelled";
                            else if(reason=="ftn_session_cram_response_length")reason="CRAM response has an unexpected length";
                            else if(reason=="ftn_session_cram_trailing_nul")reason="CRAM response contains an extra trailing NUL byte";
                            else if(reason=="ftn_session_cram_response_encoding")reason="CRAM response is not a valid hexadecimal digest";
                            else if(reason=="ftn_session_cram_digest_mismatch")reason="CRAM format valid; digest does not match this connection's session password";
                            else if(reason=="ftn_session_cram_crypto_unavailable")reason="Local CRAM calculation unavailable";
                            else{if(reason.rfind("ftn_",0)==0)reason.erase(0,4);std::replace(reason.begin(),reason.end(),'_',' ');if(reason.empty())reason="Connection ended without completion";}
                            activity("Session failed: "+reason+". Unacknowledged mail remains queued.");
                        }
                        {std::lock_guard<std::mutex> lock(mutex_);for(auto& row:view_.sessions)if(row.serial==number)row.active=false;}
                        done->store(true);wake_.notify_all();
                    });
                };
                for(auto* listener:{classic.get(),tls.get()}) {
                    if(!listener||workers.size()>=4||cancel_)continue;
                    try{auto channel=listener->accept(std::chrono::milliseconds(5),[this]{return !stopping_.load()&&!cancel_.load();});if(channel)launch(std::move(channel),{},false);}
                    catch(const std::exception& error){if(!cancel_){std::lock_guard<std::mutex> lock(mutex_);view_.diagnostic=diagnostic(error);}}
                }
                for(const auto& link:active->links) {
                    if(workers.size()>=4||cancel_)break;
                    if(!enabled(*active,link))continue;
                    const auto identity=key(link.network,link.code);
                    if(std::any_of(workers.begin(),workers.end(),[&](const auto& w){return w.outbound==identity;}))continue;
                    bool requested{},include_hold{};
                    {std::lock_guard<std::mutex> lock(mutex_);const auto p=std::find_if(polls_.begin(),polls_.end(),[&](const auto& p){return p.network==link.network&&p.link==link.code;});if(p!=polls_.end()){if(!processor_done.load())continue;requested=true;include_hold=p->hold;polls_.erase(p);}}
                    const auto now=Clock::now();auto found=due.find(identity);
                    if(found==due.end()){due[identity]=now+std::chrono::minutes(link.poll_minutes);found=due.find(identity);}
                    bool queued{};
                    if(!requested&&link.contact_on_mail&&processor_done.load()&&now>=mail_due[identity]&&now>=mail_probe[identity]){
                        mail_probe[identity]=now+std::chrono::seconds(5);
                        const auto ready=storage_.ftn_outbound_ready({true,{}},link.network,link.code);
                        queued=ready&&ready.value&&*ready.value;
                    }
                    if(!requested&&!queued&&(!link.poll_minutes||now<found->second))continue;
                    mail_due[identity]=now+std::chrono::minutes(1);

                    due[identity]=now+std::chrono::minutes(std::max<std::uint16_t>(1,link.poll_minutes));
                    launch({},link,include_hold);
                }
                }
            }
            std::unique_lock<std::mutex> lock(mutex_);wake_.wait_for(lock,workers.empty()?std::chrono::milliseconds(inbound?250:1000):std::chrono::milliseconds(50));
        }
        retire();
        std::lock_guard<std::mutex> lock(mutex_);view_.diagnostic="ftn_stopped_mail_retained";
    }catch(const std::exception& error){retire();std::lock_guard<std::mutex> lock(mutex_);view_.diagnostic=diagnostic(error);}
    catch(...){retire();std::lock_guard<std::mutex> lock(mutex_);view_.diagnostic="ftn_engine_failed";}
}
}
