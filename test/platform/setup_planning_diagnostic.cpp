// Real, unchanged planning checks only. No execute, worker, dialog or board writes.
#include "setup_engine.hpp"
#include "planning_task.hpp"
#include <iostream>
#include <string_view>
#include <vector>
namespace setup=sfx::apps::setup;
static const char* phase_name(setup::PlanningPhase p){
    switch(p){
    case setup::PlanningPhase::total:return "total";
    case setup::PlanningPhase::embedded_payload:return "embedded_payload";
    case setup::PlanningPhase::location:return "location";
    case setup::PlanningPhase::receipt:return "receipt";
    case setup::PlanningPhase::installed_files:return "installed_files";
    case setup::PlanningPhase::installed_boundaries:return "installed_boundaries";
    case setup::PlanningPhase::installed_program:return "installed_program";
    case setup::PlanningPhase::installed_open_security:return "installed_open_security";
    case setup::PlanningPhase::installed_native_identity:return "installed_native_identity";
    case setup::PlanningPhase::installed_read:return "installed_read";
    case setup::PlanningPhase::installed_sha:return "installed_sha";
    case setup::PlanningPhase::source_recognition:return "source_recognition";
    case setup::PlanningPhase::details:return "details";
    }return "unknown";
}
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;
    const std::wstring action=argv[1];
    if(action==L"--reject-role-identity"){
        const auto root=setup::install::Path(L"C:\\Program Files\\SFX-Beta4-Test-20261113-r2");
        auto receipt=setup::install::read_receipt(root,setup::install::operator_sid());receipt.program_ids[0]^=1;
        std::vector<setup::install::StructureVerificationTiming> observations;
        setup::install::StructureVerificationCallbacks callbacks;callbacks.timing=[&](const auto& e){observations.push_back(e);};
        bool rejected=false;try{setup::install::verify_structure(receipt,true,callbacks);}catch(const std::exception& e){rejected=std::string_view(e.what())=="installation_program_mismatch";}
        bool read_or_hash=false;for(const auto& e:observations)if(e.detail==setup::install::StructureVerificationTiming::Detail::file_read||e.detail==setup::install::StructureVerificationTiming::Detail::sha)read_or_hash=true;
        const bool valid=rejected&&!read_or_hash;
        std::cout<<"{\"short_circuit\":\""<<(valid?"PASS":"FAIL")<<"\",\"native_mismatch_rejected\":"<<(rejected?"true":"false")<<",\"read_or_sha_called\":"<<(read_or_hash?"true":"false")<<"}\n";return valid?0:1;
    }
    if(action==L"--wording"){
        setup::Plan plan;plan.root=L"C:\\SFX Engineering Plan";plan.backup=plan.root/L"Backup"/L"planned.zip";
        plan.operation=setup::Operation::maintenance;
        const auto maintenance=setup::summary(plan);plan.operation=setup::Operation::rebuild;const auto rebuild=setup::summary(plan);
        bool valid=maintenance.find(L"Create and verify a mutable-data backup")!=maintenance.npos&&maintenance.find(L"Verified mutable-data backup")==maintenance.npos&&rebuild.find(L"Create and verify a preservation ZIP")!=rebuild.npos;
        for(unsigned flags=0;flags!=4;++flags){setup::Options options;options.start_menu=(flags&1)!=0;options.desktop=(flags&2)!=0;const auto complete=setup::completion_text(plan,options);valid=valid&&complete.find(L"Verified backup: ")!=complete.npos&&(flags?complete.find(L"selected shortcut")!=complete.npos:complete.find((plan.root/L"Bin"/L"SFXControlPanel.exe").native())!=complete.npos);}
        plan.operation=setup::Operation::maintenance;plan.backup.clear();valid=valid&&setup::summary(plan).find(L"Optional data backup skipped. Keep your own usable backup.")!=std::wstring::npos;
        std::cout<<"{\"wording\":\""<<(valid?"PASS":"FAIL")<<"\",\"scope\":\"actual summary/completion formatters only; no execution\"}\n";return valid?0:1;
    }
    if(action!=L"--inspect"&&action!=L"--inspect-on-task"&&action!=L"--reject"&&action!=L"--cancel-at-installed"&&action!=L"--observer-throws"&&action!=L"--control-observer-throws")return 2;
    const auto root=setup::install::Path(L"C:\\Program Files\\SFX-Beta4-Test-20261113-r2");
    std::vector<setup::PlanningTiming> events;bool cancelled=false,in_installed=false;
    setup::PlanningCallbacks callbacks;
    callbacks.keep_going=[&]{if(action==L"--control-observer-throws"&&in_installed)throw std::runtime_error("synthetic control observer failure");return !cancelled;};
    callbacks.status=[&](auto phase){
        if(phase==setup::PlanningPhase::installed_files)in_installed=true;
        if(action==L"--cancel-at-installed"&&phase==setup::PlanningPhase::installed_files)cancelled=true;
        if(action==L"--observer-throws")throw std::runtime_error("synthetic observer failure");
    };
    callbacks.timing=[&](const auto& event){events.push_back(event);if(action==L"--observer-throws")throw std::runtime_error("synthetic timing observer failure");};
    const char* result="failed";const char* operation="none";bool expected=false;
    setup::PlanningTask::Timing task_timing;
    try{
        const auto plan=[&]{
            if(action!=L"--inspect-on-task")return setup::inspect_plan(action==L"--reject"?setup::install::Path(L"relative"):root,false,callbacks);
            setup::PlanningTask task;
            task.start(root,false,[&](const auto& path,bool remove,const auto& task_callbacks){auto observed=task_callbacks;observed.timing=callbacks.timing;return setup::inspect_plan(path,remove,observed);});
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(60);
            while(!task.ready()){if(std::chrono::steady_clock::now()>=end){task.cancel();throw std::runtime_error("read-only task timeout");}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            auto result=task.take();task_timing=task.last_timing();return result;
        }();
        operation=plan.operation==setup::Operation::maintenance?"maintenance":"unexpected";
        expected=(action==L"--inspect"||action==L"--inspect-on-task"||action==L"--observer-throws")&&plan.operation==setup::Operation::maintenance;
        result="accepted";
    }catch(const setup::PlanningCancelled&){result="cancelled";expected=action==L"--cancel-at-installed";}
    catch(const setup::PlanningObservationFailure&){result="observer_failed";expected=action==L"--control-observer-throws";}
    catch(const std::exception& error){result="rejected";expected=action==L"--reject"&&std::string_view(error.what())=="installation_requires_local_absolute_root";}
    bool first=true;std::cout<<"{\"outcome\":\""<<result<<"\",\"operation\":\""<<operation<<"\",\"expected\":"<<(expected?"true":"false")<<",\"events\":[";
    for(const auto& event:events){if(!first)std::cout<<',';first=false;std::cout<<"{\"phase\":\""<<phase_name(event.phase)<<"\",\"role\":"<<event.role<<",\"us\":"<<event.microseconds<<",\"completed\":"<<(event.completed?"true":"false")<<'}';}
    std::cout<<"],\"task\":{\"requested_us\":"<<task_timing.requested_us<<",\"started_us\":"<<task_timing.started_us<<",\"finished_us\":"<<task_timing.finished_us<<",\"consumed_us\":"<<task_timing.consumed_us<<"}}\n";return expected?0:1;
}
