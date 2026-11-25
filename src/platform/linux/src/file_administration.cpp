#include "sfx/platform/linux/file_administration.hpp"
#include "sfx/platform/linux/file_owner_view.hpp"
#include "sfx/platform/linux/file_intake.hpp"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <stdexcept>
namespace sfx::platform::linux_host {
namespace app=application;
namespace {
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
app::FileManagementJobStatus rejected(app::LocalAccountResultCode code){app::FileManagementJobStatus result;result.code=code;return result;}
std::uint64_t monotonic_millis(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
}

struct LinuxFileAdministration::Impl {
    struct Job {
        app::FileManagementTask task;
        std::vector<std::uint8_t> canonical;
        app::FileManagementJobStatus state;
        std::atomic_bool cancel{};
        std::uint64_t started{};
    };
    app::IFileStorage& storage;
    app::IFtnMailStorage* ftn{};
    Hatch hatch;
    std::filesystem::path root;
    std::shared_ptr<IFilePayloadBindings> bindings;
    app::LocalAccountAuthority authority;
    std::optional<app::MessageActor> caller;
    std::function<std::optional<app::MessageActor>()> revalidate;
    std::optional<app::MessageActor> live_actor() const {
        const auto current=revalidate?revalidate():std::optional<app::MessageActor>{};
        return current&&caller&&current->account==caller->account&&current->session==caller->session?current:std::nullopt;
    }
    app::FileConfigurationActor actor() const {return {authority==app::LocalAccountAuthority::installation_owner,live_actor()};}
    bool authorized() const {return authority==app::LocalAccountAuthority::installation_owner || live_actor().has_value();}
    bool allowed(const app::FileManagementTask& task) {
        if(!caller)return true;
        using K=app::FileManagementTaskKind;
        switch(task.kind){
        case K::operation:{
            if(task.operation.action!=app::FileOperationAction::rename&&task.operation.action!=app::FileOperationAction::move&&task.operation.action!=app::FileOperationAction::purge)return false;
            // The terminal counterpart cannot become a legacy retained-file
            // purge screen through an invented record identifier.
            const auto record=storage.managed_file(actor(),task.operation.file);
            return record&&record.value&&record.value->file.state==app::FileCatalogState::available&&record.value->file.revision==task.operation.expected_revision;
        }
        case K::reconcile_operation:case K::cancel_operation:{
            std::lock_guard<std::mutex> lock(data);
            return std::any_of(jobs.begin(),jobs.end(),[&](const auto& job){return
                (job->task.kind==K::operation&&job->task.operation.operation==task.target)||
                ((job->task.kind==K::reconcile_operation||job->task.kind==K::cancel_operation)&&job->task.target==task.target);});
        }
        case K::scan_pending:case K::scan_offline:case K::scan_report:
        case K::open_view:case K::page_view:case K::read_view_member:case K::view_archive_index:case K::close_view:return true;
        default:return false;
        }
    }
    LinuxFileManager::Boundary boundary;
    std::mutex serial,data;
    std::thread worker;
    bool stopping{};
    std::vector<std::shared_ptr<Job>> jobs; // bounded completed-result/replay cache
    std::unique_ptr<FileUnregisteredScan> discovery;
    std::unique_ptr<FilePayloadEnumeration> folder_discovery;
    std::unique_ptr<LinuxFileOwnerView> view;
    std::vector<std::string> scan_pages; // one bounded report, at most512 x14000 bytes
    app::Identifier128 scan_cursor;
    std::uint64_t scan_activity{};
    app::Identifier128 view_cursor;
    app::Identifier128 discovery_cursor;
    app::FileAreaId discovery_area;
    std::int64_t discovery_area_revision{};
    std::string discovery_folder;
    LinuxFileAdministration::IdleClock idle_clock;
    std::uint64_t view_activity{},discovery_activity{};
    Impl(app::IFileStorage& s,std::filesystem::path r,std::shared_ptr<IFilePayloadBindings> binding_context,app::LocalAccountAuthority a,LinuxFileManager::Boundary b,LinuxFileAdministration::IdleClock c,app::IFtnMailStorage* f,Hatch h={}):storage(s),ftn(f),hatch(std::move(h)),root(std::move(r)),bindings(std::move(binding_context)),authority(a),boundary(std::move(b)),idle_clock(c?std::move(c):LinuxFileAdministration::IdleClock{[]{return monotonic_millis();}}){}
    void expire_joined_view(){const auto now=idle_clock();if(!scan_pages.empty()&&now>=scan_activity&&now-scan_activity>=LinuxFileAdministration::view_idle_millis){scan_pages.clear();scan_cursor={};}if(view&&now>=view_activity&&now-view_activity>=LinuxFileAdministration::view_idle_millis){view.reset();view_cursor={};}if((discovery||folder_discovery)&&now>=discovery_activity&&now-discovery_activity>=LinuxFileAdministration::view_idle_millis){discovery.reset();folder_discovery.reset();discovery_cursor={};}}
    app::FileManagementJobStatus snapshot(const std::shared_ptr<Job>& job){
        auto result=job->state;if(result.state==app::FileManagementJobState::running)result.elapsed_millis=monotonic_millis()-job->started;return result;
    }
    void finish(const std::shared_ptr<Job>& job,app::FileManagementJobStatus result){
        const bool bulk=result.kind==app::FileManagementTaskKind::scan_pending||result.kind==app::FileManagementTaskKind::scan_offline||result.kind==app::FileManagementTaskKind::scan_report;
        result.elapsed_millis=monotonic_millis()-job->started;result.detail=app::sanitize_file_text(result.detail,bulk?16384:512,bulk);
        std::lock_guard<std::mutex> lock(data);result.bytes_done=job->state.bytes_done;result.bytes_total=job->state.bytes_total;result.cancellation_requested=job->cancel.load();job->state=std::move(result);
    }
    void run(const std::shared_ptr<Job>& job) noexcept {
        app::FileManagementJobStatus result;{std::lock_guard<std::mutex> lock(data);result=job->state;}const auto& task=job->task;
        try{
            if(job->cancel.load()||!authorized()){result.state=app::FileManagementJobState::cancelled;result.detail="Cancelled before native operation started.";finish(job,std::move(result));return;}
            if(task.kind==app::FileManagementTaskKind::cancel_unregistered){
                if((discovery||folder_discovery)&&task.target==discovery_cursor){discovery.reset();folder_discovery.reset();discovery_cursor={};result.state=app::FileManagementJobState::completed;result.detail="Read-only discovery closed. Payloads and catalog unchanged.";}
                else{result.state=app::FileManagementJobState::refused;result.detail="Discovery cursor is no longer active.";}finish(job,std::move(result));return;
            }
            const auto progress=[&](std::uint64_t done,std::uint64_t total){
                if(job->cancel.load()||done>total||!authorized())return false;
                std::lock_guard<std::mutex> lock(data);job->state.bytes_done=done;job->state.bytes_total=total;return !job->cancel.load();
            };
            if(task.kind==app::FileManagementTaskKind::scan_report){
                if(!(task.target==scan_cursor)||task.view_page>=scan_pages.size()){result.state=app::FileManagementJobState::refused;result.detail="Scan results are no longer available. No files were changed by this request.";}
                else{result.state=app::FileManagementJobState::completed;result.detail=scan_pages[task.view_page];result.report=app::FileManagementJobStatus::Report{task.view_page,static_cast<std::uint16_t>(scan_pages.size())};scan_activity=idle_clock();}
                finish(job,std::move(result));return;
            }
            const bool view_task=task.kind>=app::FileManagementTaskKind::open_view&&task.kind<=app::FileManagementTaskKind::close_view;
            if(view_task){
                if(task.kind!=app::FileManagementTaskKind::open_view&&(!view||!(task.target==view_cursor))){result.state=app::FileManagementJobState::refused;result.detail="Preview cursor expired or changed. Open the selected file again.";finish(job,std::move(result));return;}
                if(task.kind==app::FileManagementTaskKind::close_view){view.reset();view_cursor={};result.state=app::FileManagementJobState::completed;result.detail="Read-only preview closed. Payload and catalog unchanged.";finish(job,std::move(result));return;}
                if(task.kind==app::FileManagementTaskKind::open_view){view.reset();view_cursor={};view=std::make_unique<LinuxFileOwnerView>(storage,root,bindings,task.view_file,task.view_archive,progress,actor());result.view=view->page(0);}
                else if(task.kind==app::FileManagementTaskKind::read_view_member)result.view=view->member(task.view_member,progress);
                else if(task.kind==app::FileManagementTaskKind::view_archive_index)result.view=view->archive_index(task.view_page);
                else result.view=view->page(task.view_page);
                require(!job->cancel.load(),"file_view_cancelled");result.view->cursor=task.job;require(app::valid_file_management_view(*result.view),"file_view_snapshot_invalid");view_cursor=task.job;view_activity=idle_clock();result.state=app::FileManagementJobState::completed;result.detail="Read-only inert preview. No original terminal controls execute; nothing extracted or changed.";finish(job,std::move(result));return;
            }
            // Do not strand a native mutation behind this host's own preview
            // handle. A later view requires a fresh current-context open.
            view.reset();view_cursor={};
            if(task.kind==app::FileManagementTaskKind::hatch_file||task.kind==app::FileManagementTaskKind::reconcile_hatch){
                if(!ftn||!hatch){result.state=app::FileManagementJobState::refused;result.detail="FTN hatch is unavailable in this owner host. No file was queued.";finish(job,std::move(result));return;}
                auto request=task.hatch;if(task.kind==app::FileManagementTaskKind::reconcile_hatch){const auto original=ftn->ftn_hatch({true,{}},task.target);if(!original||!original.value){result.state=app::FileManagementJobState::refused;result.detail="The original hatch is unavailable. No new hatch was created.";finish(job,std::move(result));return;}request=original.value->request;}
                const auto hatched=hatch(request,[&]{return !job->cancel.load();},progress);
                result.journal_confirmed=hatched.journaled;result.state=hatched.queued?app::FileManagementJobState::completed:hatched.journaled?app::FileManagementJobState::held:job->cancel.load()?app::FileManagementJobState::cancelled:app::FileManagementJobState::refused;
                if(hatched.queued){result.result=request.file;result.detail="File queued for the confirmed FTN destinations. Queueing is not delivery or acceptance.";}
                else result.detail=(hatched.journaled?"Hatch held; its original request and recovery files are retained. ":"No file was queued. ")+hatched.reason;
                finish(job,std::move(result));return;
            }
            LinuxFileManager manager(storage,actor(),root,bindings,boundary);
            NativeFileOperationOutcome outcome;
            switch(task.kind){
            case app::FileManagementTaskKind::hatch_file:
            case app::FileManagementTaskKind::reconcile_hatch:throw std::logic_error("hatch_dispatch_inconsistent");
            case app::FileManagementTaskKind::scan_pending:
                scan_cursor={};scan_pages.clear();outcome=manager.scan_files(true,progress,&scan_pages);
                if(!scan_pages.empty()){scan_cursor=task.job;scan_activity=idle_clock();result.report=app::FileManagementJobStatus::Report{0,static_cast<std::uint16_t>(scan_pages.size())};}break;
            case app::FileManagementTaskKind::scan_report:throw std::logic_error("scan_report_dispatch_inconsistent");
            case app::FileManagementTaskKind::scan_offline:outcome=manager.scan_files(false,progress);break;
            case app::FileManagementTaskKind::discover_unregistered:
            case app::FileManagementTaskKind::discover_import_folder:
            case app::FileManagementTaskKind::continue_unregistered:{
                if(task.kind!=app::FileManagementTaskKind::continue_unregistered){
                    discovery.reset();folder_discovery.reset();discovery_cursor={};discovery_area=task.discovery_area;discovery_folder.clear();
                    const auto config=storage.file_configuration({true,{}});require(config&&config.value,"file_discovery_configuration_unavailable");const auto selected=std::find_if(config.value->areas.begin(),config.value->areas.end(),[&](const auto& a){return a.area.id==discovery_area;});require(selected!=config.value->areas.end(),"file_discovery_area_unavailable");discovery_area_revision=selected->area.revision;
                    std::vector<std::filesystem::path> others;for(const auto& a:config.value->areas)if(!(a.area.id==discovery_area))others.push_back(std::filesystem::u8path(a.storage_path));
                    auto destination=FilePayloadDirectory::area(root,std::filesystem::u8path(selected->storage_path),others,{"bin","config","data","display","backup","logs","work","doors","botgate"},bindings);
                    auto chosen=task.discovery_folder.empty()?destination.path():bindings->resolve_path(std::filesystem::u8path(task.discovery_folder));chosen.make_preferred();
                    if(task.kind==app::FileManagementTaskKind::discover_unregistered||chosen==destination.path()){discovery=std::make_unique<FileUnregisteredScan>(manager.find_unregistered(discovery_area));discovery_folder=destination.path().u8string();}
                    else{std::vector<std::filesystem::path> protected_paths{root};for(const auto& a:config.value->areas){auto path=std::filesystem::u8path(a.storage_path);protected_paths.push_back(bindings->resolve_path(path));}auto source=FilePayloadDirectory::import_source(chosen,protected_paths,bindings);folder_discovery=std::make_unique<FilePayloadEnumeration>(source.enumerate());discovery_folder=source.path().u8string();}
                }else if((!discovery&&!folder_discovery)||!(task.target==discovery_cursor)){result.state=app::FileManagementJobState::refused;result.detail="Discovery continuation expired or changed. Start a new read-only scan.";finish(job,std::move(result));return;}
                if(folder_discovery){const auto config=storage.file_configuration({true,{}});require(config&&config.value,"file_discovery_configuration_unavailable");const auto selected=std::find_if(config.value->areas.begin(),config.value->areas.end(),[&](const auto& a){return a.area.id==discovery_area;});require(selected!=config.value->areas.end()&&selected->area.revision==discovery_area_revision,"file_discovery_destination_changed");}
                auto page=discovery?discovery->next(100,[&]{return !job->cancel.load();}):folder_discovery->next(100,[&]{return !job->cancel.load();});
                result.discovery=app::FileManagementDiscoveryPage{task.job,discovery_area,std::move(page.names),page.finished,discovery_folder};result.state=app::FileManagementJobState::completed;result.detail="Read-only candidates only; each selected native file is rechecked before import.";
                discovery_cursor=task.job;discovery_activity=idle_clock();if(page.finished){discovery.reset();folder_discovery.reset();discovery_cursor={};}finish(job,std::move(result));return;
            }
            case app::FileManagementTaskKind::cancel_unregistered:throw std::logic_error("discovery_close_dispatch_inconsistent");
            case app::FileManagementTaskKind::open_view:
            case app::FileManagementTaskKind::page_view:
            case app::FileManagementTaskKind::read_view_member:
            case app::FileManagementTaskKind::view_archive_index:
            case app::FileManagementTaskKind::close_view:throw std::logic_error("view_dispatch_inconsistent");
            case app::FileManagementTaskKind::inspect_files:{
                const auto& query=task.inspection_request;const auto page=manager.inspect_files(query.filter,query.after,app::kFileManagementRecordPage,progress);app::FileManagementInspectionPage inspected;inspected.catalog.code=app::LocalAccountResultCode::ok;inspected.catalog.next=page.next;
                for(const auto& row:page.entries){auto compact=row.record;compact.file.descriptions.long_text.clear();inspected.catalog.records.push_back(std::move(compact));inspected.presence.push_back(row.presence);}result.inspection=std::move(inspected);result.state=app::FileManagementJobState::completed;result.detail="Read-only native identity/hash inspection. No missing flags or payload changes were committed.";finish(job,std::move(result));return;
            }
            case app::FileManagementTaskKind::operation:outcome=manager.execute(task.operation,progress);break;
            case app::FileManagementTaskKind::import_file:outcome=manager.import_file(task.import,progress);break;
            case app::FileManagementTaskKind::reconcile_operation:outcome=manager.reconcile(task.target,progress);break;
            case app::FileManagementTaskKind::cancel_operation:outcome=manager.cancel_unchanged(task.target,progress);break;
            case app::FileManagementTaskKind::reconcile_import:outcome=manager.reconcile_import(task.target,progress);break;
            case app::FileManagementTaskKind::cancel_import:outcome=manager.cancel_import_unchanged(task.target,progress);break;
            case app::FileManagementTaskKind::reconcile_intake:{
                const auto staging=FilePayloadDirectory::private_store(root/"data"/"fileintake",bindings);
                const bool cleaned=cleanup_accepted_file_intake(storage,{true,{}},task.target,staging,root,bindings,progress);
                outcome={cleaned,false,cleaned,{},cleaned?"Accepted-upload staging reconciled against verified native storage. No new upload credit.":"Staging retained or receipt unconfirmed. The surviving final must be verified; review current pending intake before retrying."};break;
            }
            case app::FileManagementTaskKind::purge_intake:outcome=manager.purge_held_intake(task.purge,progress);break;
            case app::FileManagementTaskKind::preview_import:{
                const auto& c=task.choice;NativeFileImportChoice choice;choice.operation=c.operation;choice.area=c.area;choice.source=bindings->resolve_path(std::filesystem::u8path(c.source_path));choice.source.make_preferred();choice.mode=c.mode;choice.filename=c.filename;choice.public_uploader=c.public_uploader;choice.descriptions=c.descriptions;choice.use_diz=c.use_diz;choice.allow_duplicate_content=c.allow_duplicate_content;choice.sysop_only=c.sysop_only;
                const auto config=storage.file_configuration({true,{}});
                if(!config||!config.value)throw std::runtime_error("file_import_configuration_unavailable");
                const auto selected=std::find_if(config.value->areas.begin(),config.value->areas.end(),[&](const auto& a){return a.area.id==c.area;});
                if(selected==config.value->areas.end())throw std::runtime_error("file_import_area_unavailable");
                std::vector<std::filesystem::path> others;for(const auto& a:config.value->areas)if(!(a.area.id==c.area))others.push_back(std::filesystem::u8path(a.storage_path));
                auto directory=FilePayloadDirectory::area(root,std::filesystem::u8path(selected->storage_path),others,{"bin","config","data","display","backup","logs","work","doors","botgate"},bindings);
                if(choice.source.is_absolute()&&choice.source.parent_path()==directory.path())choice.mode=app::FileImportMode::in_place;
                result.preview=manager.preview_import(choice,progress);
                if(result.preview){const auto duplicates=storage.file_content_duplicate_count({true,{}},result.preview->registration.payload);if(!duplicates||!duplicates.value)throw std::runtime_error("file_duplicate_preview_unavailable");result.duplicate_contents=*duplicates.value;result.state=app::FileManagementJobState::completed;result.detail=result.duplicate_contents?"Preview only. Duplicate content exists; explicit override is required. Filename overwrite remains forbidden.":"Preview only. Confirm the exact source and metadata before importing.";}
                else{result.state=job->cancel.load()?app::FileManagementJobState::cancelled:app::FileManagementJobState::refused;result.detail="Import preview unavailable or cancelled; no import was attempted.";}
                finish(job,std::move(result));return;
            }
            case app::FileManagementTaskKind::preview_intake_purge:
                result.purge_preview=manager.preview_intake_purge(task.target,progress);result.state=result.purge_preview?app::FileManagementJobState::completed:job->cancel.load()?app::FileManagementJobState::cancelled:app::FileManagementJobState::refused;result.detail=result.purge_preview?"Read-only exact-object purge preview; explicit confirmation is still required.":"Purge preview unavailable or cancelled; nothing purged.";finish(job,std::move(result));return;
            case app::FileManagementTaskKind::reupload_block:{
                const auto& b=task.block;const auto changed=storage.set_file_reupload_block({true,{}},b.operation,b.file,b.expected_revision,b.blocked);
                if(changed&&changed.value){outcome={true,false,true,b.file,"Content re-upload policy saved; existing payloads and transfer counters unchanged."};}
                else outcome={false,false,false,{},"Re-upload policy refused; refresh current record and authority."};
                break;
            }
            }
            result.journal_confirmed=outcome.journal_confirmed;result.detail=outcome.detail;
            const bool intent_cancel=(task.kind==app::FileManagementTaskKind::cancel_operation||task.kind==app::FileManagementTaskKind::cancel_import)&&outcome.cancelled;
            if(outcome.completed||intent_cancel){result.state=app::FileManagementJobState::completed;result.result=outcome.result;}
            else result.state=outcome.journal_confirmed?app::FileManagementJobState::held:job->cancel.load()?app::FileManagementJobState::cancelled:app::FileManagementJobState::refused;
            finish(job,std::move(result));
        }catch(...){
            const bool view_task=task.kind>=app::FileManagementTaskKind::open_view&&task.kind<=app::FileManagementTaskKind::close_view;
            const bool readonly=task.kind==app::FileManagementTaskKind::scan_report||view_task||task.kind==app::FileManagementTaskKind::discover_unregistered||task.kind==app::FileManagementTaskKind::discover_import_folder||task.kind==app::FileManagementTaskKind::continue_unregistered||task.kind==app::FileManagementTaskKind::cancel_unregistered||task.kind==app::FileManagementTaskKind::inspect_files;
            if(view_task){view.reset();view_cursor={};}
            if(task.kind==app::FileManagementTaskKind::discover_unregistered||task.kind==app::FileManagementTaskKind::discover_import_folder||task.kind==app::FileManagementTaskKind::continue_unregistered){discovery.reset();folder_discovery.reset();discovery_cursor={};}
            result.state=readonly&&job->cancel.load()?app::FileManagementJobState::cancelled:app::FileManagementJobState::failed;result.preview.reset();result.purge_preview.reset();result.discovery.reset();result.inspection.reset();result.view.reset();result.report.reset();result.duplicate_contents=0;result.detail=readonly?"Read-only observation unavailable or cancelled; no catalog/payload changes committed.":"File operation status unavailable. Inspect durable pending operations before retrying; no success is asserted.";finish(job,std::move(result));
        }
    }
};
LinuxFileAdministration::LinuxFileAdministration(app::IFileStorage& s,std::filesystem::path root,std::shared_ptr<IFilePayloadBindings> bindings,app::LocalAccountAuthority authority,LinuxFileManager::Boundary boundary,IdleClock clock,app::IFtnMailStorage* ftn,Hatch hatch):impl_(std::make_unique<Impl>(s,std::move(root),std::move(bindings),authority,std::move(boundary),std::move(clock),ftn,std::move(hatch))){}
LinuxFileAdministration::LinuxFileAdministration(app::IFileStorage& s,std::filesystem::path root,std::shared_ptr<IFilePayloadBindings> bindings,app::MessageActor actor,std::function<std::optional<app::MessageActor>()> revalidate):impl_(std::make_unique<Impl>(s,std::move(root),std::move(bindings),app::LocalAccountAuthority::none,LinuxFileManager::Boundary{},IdleClock{},nullptr)){impl_->caller=std::move(actor);impl_->revalidate=std::move(revalidate);}
LinuxFileAdministration::~LinuxFileAdministration(){stop();}
app::FileManagementJobStatus LinuxFileAdministration::start(const app::FileManagementTask& task){
    auto& p=*impl_;if(!p.authorized())return rejected(app::LocalAccountResultCode::unauthorized);const auto canonical=app::encode_file_management_task(task);if(canonical.empty())return rejected(app::LocalAccountResultCode::invalid_request);
    std::lock_guard<std::mutex> serial(p.serial);if(p.stopping)return {};
    {std::lock_guard<std::mutex> lock(p.data);for(const auto& job:p.jobs)if(job->task.job==task.job)return job->canonical==canonical?p.snapshot(job):rejected(app::LocalAccountResultCode::invalid_request);
        if(!p.jobs.empty()&&p.jobs.back()->state.state==app::FileManagementJobState::running)return rejected(app::LocalAccountResultCode::conflict);}
    if(!p.allowed(task))return rejected(app::LocalAccountResultCode::unauthorized);
    if(p.worker.joinable())p.worker.join();
    p.expire_joined_view();auto job=std::make_shared<Impl::Job>();job->task=task;job->canonical=canonical;job->started=monotonic_millis();job->state.code=app::LocalAccountResultCode::ok;job->state.job=task.job;job->state.kind=task.kind;job->state.state=app::FileManagementJobState::running;job->state.detail="Native file operation running.";
    {std::lock_guard<std::mutex> lock(p.data);if(p.jobs.size()==8)p.jobs.erase(p.jobs.begin());p.jobs.push_back(job);}
    try{p.worker=std::thread([&p,job]{p.run(job);});}catch(...){app::FileManagementJobStatus failed;{std::lock_guard<std::mutex> lock(p.data);failed=job->state;}failed.state=app::FileManagementJobState::failed;failed.detail="Native worker could not start; no file operation attempted.";p.finish(job,std::move(failed));}
    std::lock_guard<std::mutex> lock(p.data);return p.snapshot(job);
}
app::FileManagementJobStatus LinuxFileAdministration::status(app::Identifier128 id){auto& p=*impl_;if(!p.authorized())return rejected(app::LocalAccountResultCode::unauthorized);if(!id.valid())return rejected(app::LocalAccountResultCode::invalid_request);std::lock_guard<std::mutex> lock(p.data);for(const auto& job:p.jobs)if(job->task.job==id)return p.snapshot(job);return rejected(app::LocalAccountResultCode::policy_rejected);}
app::FileManagementJobStatus LinuxFileAdministration::cancel(app::Identifier128 id){auto& p=*impl_;if(!p.authorized())return rejected(app::LocalAccountResultCode::unauthorized);if(!id.valid())return rejected(app::LocalAccountResultCode::invalid_request);std::lock_guard<std::mutex> lock(p.data);for(const auto& job:p.jobs)if(job->task.job==id){if(job->state.state==app::FileManagementJobState::running){job->cancel.store(true);job->state.cancellation_requested=true;}return p.snapshot(job);}return rejected(app::LocalAccountResultCode::policy_rejected);}
void LinuxFileAdministration::expire_idle_views(){auto& p=*impl_;std::lock_guard<std::mutex> serial(p.serial);if(p.stopping)return;{std::lock_guard<std::mutex> lock(p.data);if(!p.jobs.empty()&&p.jobs.back()->state.state==app::FileManagementJobState::running)return;}if(p.worker.joinable())p.worker.join();p.expire_joined_view();}
bool LinuxFileAdministration::active(){auto& p=*impl_;std::lock_guard<std::mutex> lock(p.data);return std::any_of(p.jobs.begin(),p.jobs.end(),[](const auto& job){return job->state.state==app::FileManagementJobState::running;});}
void LinuxFileAdministration::stop() noexcept {if(!impl_)return;auto& p=*impl_;std::lock_guard<std::mutex> serial(p.serial);p.stopping=true;{std::lock_guard<std::mutex> lock(p.data);for(const auto& job:p.jobs)if(job->state.state==app::FileManagementJobState::running){job->cancel.store(true);job->state.cancellation_requested=true;}}if(p.worker.joinable())p.worker.join();p.discovery.reset();p.folder_discovery.reset();p.discovery_cursor={};p.view.reset();p.view_cursor={};}
} // namespace sfx::platform::linux_host
