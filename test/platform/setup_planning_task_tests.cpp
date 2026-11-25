#include "../../src/apps/setup/planning_task.hpp"
#include <chrono>
#include <future>
#include <iostream>
namespace setup=sfx::apps::setup;
namespace sfx::apps::setup {
Plan inspect_plan(const install::Path&,bool,const PlanningCallbacks&){throw std::runtime_error("unexpected real inspector in task-only test");}
}
static unsigned checks{};
static void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
static void ready(setup::PlanningTask& task){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(!task.ready()){if(std::chrono::steady_clock::now()>=end)throw std::runtime_error("read-only task timeout");std::this_thread::yield();}}
int main()try{
    std::promise<void> entered;auto arrived=entered.get_future();
    setup::PlanningTask task;std::wstring selected=L"selected synthetic root";
    task.start(selected,false,[&](const auto& root,bool remove,const auto& cb){
        check(root==L"selected synthetic root"&&!remove,"one immutable selected-root/action snapshot");
        cb.status(setup::PlanningPhase::installed_files);entered.set_value();
        while(cb.keep_going())std::this_thread::yield();
        throw setup::PlanningCancelled();return setup::Plan{};
    });
    selected=L"changed UI value";check(arrived.wait_for(std::chrono::seconds(5))==std::future_status::ready,"bounded worker arrival");
    check(task.active()&&!task.ready()&&task.phase()==setup::PlanningPhase::installed_files,"responsive owner can observe in-flight stage");
    bool refused=false;try{task.start(L"other",false);}catch(const std::logic_error&){refused=true;}
    check(refused,"duplicate review cannot start another task");
    task.cancel();ready(task);bool cancelled=false;try{task.take();}catch(const setup::PlanningCancelled&){cancelled=true;}
    check(cancelled&&!task.active(),"typed cancellation joins and clears task for retry");
    task.start(L"retry",true,[](const auto&,bool remove,const auto&){if(!remove)throw std::runtime_error("snapshot lost");setup::Plan p;p.operation=setup::Operation::uninstall;return p;});ready(task);
    check(task.take().operation==setup::Operation::uninstall&&!task.active(),"retry returns exactly one checked plan");
    task.start(L"failure",false,[](const auto&,bool,const auto&)->setup::Plan{throw std::runtime_error("synthetic planning failure");});ready(task);bool failed=false;try{task.take();}catch(const std::runtime_error&){failed=true;}
    check(failed&&!task.active(),"error is rethrown after join and allows retry");
    std::promise<void> destroying;auto destroying_ready=destroying.get_future();std::atomic<bool> retired{false};
    {setup::PlanningTask owned;owned.start(L"owned",false,[&](const auto&,bool,const auto& cb){destroying.set_value();while(cb.keep_going())std::this_thread::yield();retired=true;throw setup::PlanningCancelled();return setup::Plan{};});check(destroying_ready.wait_for(std::chrono::seconds(5))==std::future_status::ready,"bounded destructor-test arrival");}
    check(retired,"destruction cancels and joins; no detached task outlives its owner");
    const auto first=setup::planning_status(setup::PlanningPhase::embedded_payload,0);const auto later=setup::planning_status(setup::PlanningPhase::installed_files,1250);
    check(first.find(L"Checking installation...")==0&&first.find(L"Checking time: 0 seconds")!=first.npos,"immediate honest initial feedback");
    check(later.find(L"Checking installed files")!=later.npos&&later.find(L"Checking time: 1 second")!=later.npos&&later!=first,"activity/current step and actual check elapsed");
    check(later.find(L'%')==later.npos&&later.find(L"ETA")==later.npos,"no percentage or ETA");
    std::cout<<"PASS setup planning task/lifecycle checks="<<checks<<"; no UI/UAC/installation\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
