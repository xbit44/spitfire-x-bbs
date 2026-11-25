#include "linux_clamav_worker.hpp"

namespace sfx::apps::runtime {
namespace app=application;
namespace host=platform::linux_host;
LinuxClamAvWorker::LinuxClamAvWorker(std::filesystem::path root,app::IClock& clock,std::shared_ptr<AlphaOperationalJournal> journal):
    root_(std::move(root)),clock_(clock),journal_(std::move(journal)){
    current_.status=host::inspect_linux_clamav(root_);thread_=std::thread([this]{run();});
}
LinuxClamAvWorker::~LinuxClamAvWorker(){stop();}
LinuxClamAvWorkerSnapshot LinuxClamAvWorker::snapshot()const{std::lock_guard<std::mutex> lock(mutex_);auto value=current_;value.paused=paused_.load();return value;}
std::uint64_t LinuxClamAvWorker::request_update(){
    std::lock_guard<std::mutex> lock(mutex_);if(stopped_.load()||current_.active||current_.queued)return 0;
    cancelled_.store(false);current_.queued=true;current_.operation=++sequence_;wake_.notify_all();return current_.operation;
}
bool LinuxClamAvWorker::cancel(std::uint64_t operation){
    std::lock_guard<std::mutex> lock(mutex_);if(!operation||operation!=current_.operation||(!current_.active&&!current_.queued))return false;
    cancelled_.store(true);wake_.notify_all();return true;
}
void LinuxClamAvWorker::pause_automatic(bool paused){paused_.store(paused);wake_.notify_all();}
void LinuxClamAvWorker::stop()noexcept{stopped_.store(true);cancelled_.store(true);wake_.notify_all();if(thread_.joinable())thread_.join();}
void LinuxClamAvWorker::run()noexcept{
    try{
        std::int64_t completed_day=-1,retry_after{};
        std::unique_lock<std::mutex> lock(mutex_);
        while(!stopped_.load()){
            const auto utc=clock_.unix_seconds();const bool manual=current_.queued;
            if(!manual&&(paused_.load()||utc<=0||utc/86400<=completed_day||utc<retry_after)){wake_.wait_for(lock,std::chrono::seconds(60));continue;}
            if(!manual){cancelled_.store(false);current_.operation=++sequence_;}
            current_.active=true;current_.queued=false;current_.elapsed_milliseconds=0;const auto operation=current_.operation;lock.unlock();
            app::OperationalContext started;started.resource_id=1;started.resource_name=manual?"Manual ClamAV definition check started":"Automatic ClamAV definition check started";
            started.diagnostic_code=manual?"clamav_manual_started":"clamav_automatic_started";journal_->emit({app::OperationalKind::file_maintenance,{},std::move(started)});
            const auto result=host::update_linux_clamav(root_,utc,manual?host::LinuxClamAvUpdateMode::manual:host::LinuxClamAvUpdateMode::automatic,
                [this,manual]{return stopped_.load()||cancelled_.load()||(!manual&&paused_.load());},
                [this](std::uint64_t elapsed){std::lock_guard<std::mutex> guard(mutex_);current_.elapsed_milliseconds=elapsed;});
            const auto status=host::inspect_linux_clamav(root_,[this]{return stopped_.load();});
            const bool skipped=result.diagnostic=="clamav_update_already_attempted_today";
            const bool problem=!result.succeeded&&!result.cancelled&&!skipped;
            app::OperationalContext finished;finished.resource_id=1;finished.diagnostic_code=result.diagnostic;
            if(result.succeeded){
                finished.resource_name=result.change==host::LinuxClamAvDefinitionChange::initial?"ClamAV initial definitions installed and verified":
                    result.change==host::LinuxClamAvDefinitionChange::current?"ClamAV definitions already current and verified":
                    result.change==host::LinuxClamAvDefinitionChange::updated?"ClamAV definitions updated and verified":"ClamAV definitions verified; change history unavailable";
                if(result.daily_version)finished.resource_name+="; daily "+std::to_string(result.daily_version);
            }else finished.resource_name=result.cancelled?"ClamAV definition check cancelled":skipped?"ClamAV check skipped: already attempted today":
                status.definitions_verified?"ClamAV update needs attention; validated definitions retained":"ClamAV update needs attention; definitions unavailable or unverified";
            journal_->emit({problem?app::OperationalKind::file_maintenance_problem:app::OperationalKind::file_maintenance,
                problem?app::OperationalOutcome::unavailable:result.cancelled?app::OperationalOutcome::cancelled:app::OperationalOutcome::normal,std::move(finished)});
            if(result.succeeded)journal_->clamav_health(false,true,true,{});
            else if(problem||(skipped&&!status.definitions_verified))journal_->clamav_health(false,false,status.definitions_verified,result.diagnostic);
            if(!result.cleanup_complete)journal_->problem(app::AttentionKind::work_preserved,"ClamAV definition update","clamav_update_work_preserved");
            lock.lock();current_.last_update=result;current_.status=status;current_.active=false;current_.completed_operation=operation;
            if(!manual){if(result.attempted||skipped)completed_day=utc/86400;else retry_after=utc+3600;}
        }
    }catch(...){
        std::lock_guard<std::mutex> lock(mutex_);stopped_.store(true);current_.active=false;current_.queued=false;current_.status.diagnostic="clamav_worker_failed";
        journal_->clamav_health(false,false,false,"clamav_worker_failed");
    }
}
}
