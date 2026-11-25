#pragma once
#include "setup_engine.hpp"
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <thread>

namespace sfx::apps::setup {
// The UI owns and joins this task. The task never stores an HWND or Ui pointer.
class PlanningTask final {
public:
    struct Timing {std::uint64_t requested_us{},started_us{},finished_us{},consumed_us{};};
    static std::uint64_t now_us() noexcept{return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
    using Inspect=std::function<Plan(const install::Path&,bool,const PlanningCallbacks&)>;
    PlanningTask()=default;
    PlanningTask(const PlanningTask&)=delete;
    PlanningTask& operator=(const PlanningTask&)=delete;
    ~PlanningTask(){cancel();if(thread_.joinable())thread_.join();}
    bool active()const noexcept{return bool(state_);}
    bool ready()const noexcept{return state_&&state_->done.load(std::memory_order_acquire);}
    PlanningPhase phase()const noexcept{return state_?state_->phase.load(std::memory_order_relaxed):PlanningPhase::total;}
    const Timing& last_timing()const noexcept{return last_timing_;}
    void cancel()noexcept{if(state_)state_->cancelled.store(true,std::memory_order_relaxed);}
    void start(install::Path root,bool uninstall,Inspect inspect=[](const auto& path,bool remove,const auto& callbacks){return inspect_plan(path,remove,callbacks);}){
        if(active()||thread_.joinable())throw std::logic_error("setup_review_already_running");
        auto next=std::make_shared<State>();next->timing.requested_us=now_us();state_=next;
        try{
            thread_=std::thread([next,root=std::move(root),uninstall,inspect=std::move(inspect)]{
                next->timing.started_us=now_us();
                PlanningCallbacks callbacks;
                callbacks.keep_going=[next]{return !next->cancelled.load(std::memory_order_relaxed);};
                callbacks.status=[next](PlanningPhase phase){next->phase.store(phase,std::memory_order_relaxed);};
                try{
                    if(!callbacks.keep_going())throw PlanningCancelled();
                    auto plan=inspect(root,uninstall,callbacks);
                    if(!callbacks.keep_going())throw PlanningCancelled();
                    next->plan=std::move(plan);
                }catch(...){next->error=std::current_exception();}
                next->timing.finished_us=now_us();next->done.store(true,std::memory_order_release);
            });
        }catch(...){state_.reset();throw;}
    }
    Plan take(){
        if(!ready())throw std::logic_error("setup_review_not_finished");
        thread_.join();auto result=std::move(state_);result->timing.consumed_us=now_us();last_timing_=result->timing;
        if(result->error)std::rethrow_exception(result->error);
        if(!result->plan)throw std::runtime_error("setup_review_result_missing");
        return std::move(*result->plan);
    }
private:
    struct State {
        std::atomic<bool> cancelled{false},done{false};
        std::atomic<PlanningPhase> phase{PlanningPhase::embedded_payload};
        std::optional<Plan> plan;
        std::exception_ptr error;
        Timing timing;
    };
    std::shared_ptr<State> state_;
    std::thread thread_;
    Timing last_timing_;
};
inline std::wstring planning_status(PlanningPhase phase,std::uint64_t elapsed_ms){
    const wchar_t* step=L"Checking installation";
    switch(phase){
    case PlanningPhase::embedded_payload:step=L"Checking setup files";break;
    case PlanningPhase::location:step=L"Checking installation location";break;
    case PlanningPhase::receipt:step=L"Checking installation record";break;
    case PlanningPhase::installed_files:case PlanningPhase::installed_boundaries:case PlanningPhase::installed_program:step=L"Checking installed files";break;
    case PlanningPhase::source_recognition:step=L"Checking existing board";break;
    case PlanningPhase::details:step=L"Preparing review";break;
    default:break;
    }
    constexpr wchar_t activity[]{L'|',L'/',L'-',L'\\'};
    const auto seconds=elapsed_ms/1000;
    return std::wstring(L"Checking installation... ")+activity[(elapsed_ms/250)%4]+L"\n"+step+L"\nChecking time: "+std::to_wstring(seconds)+(seconds==1?L" second":L" seconds");
}
} // namespace sfx::apps::setup
