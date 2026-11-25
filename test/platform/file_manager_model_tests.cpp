#include "client_worker.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
namespace ui=sfx::file_manager;namespace app=sfx::application;namespace win=sfx::platform::win32;
namespace {
unsigned checks{};
void check(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
app::Identifier128 id(unsigned n){app::Identifier128 v;v.bytes[0]=51;v.bytes[14]=static_cast<std::uint8_t>(n>>8);v.bytes[15]=static_cast<std::uint8_t>(n);return v;}
struct FakeState {std::mutex mutex;std::condition_variable changed;bool block{},entered{},release{},wrong_detail{},root_exact{};unsigned factories{},reads{},starts{},applied{},active{},maximum{},destroyed{};std::vector<std::uint8_t> exact;app::FileManagementTask task;};
app::ManagedFile record(unsigned n){app::ManagedFile r;auto& f=r.file;f.id={n};f.area={1};f.revision=1;f.publication_sequence=n;f.name=std::to_string(n)+".txt";f.descriptions={"Fake model row",""};f.public_uploader="Public Sysop";f.bytes=1;f.sha256[0]=1;f.origin=app::FileOrigin::sysop_import;r.payload={1,f.sha256,"fake-volume","fake-object-"+std::to_string(n)};return r;}
app::FileManagementJobStatus completed(const app::FileManagementTask& task){app::FileManagementJobStatus s;s.code=app::LocalAccountResultCode::ok;s.job=task.job;s.kind=task.kind;s.state=app::FileManagementJobState::completed;s.journal_confirmed=true;s.result=task.operation.file;return s;}
class Fake final:public win::IFileAdministrationClient {
    std::shared_ptr<FakeState> p_;
public:
    explicit Fake(std::shared_ptr<FakeState> p):p_(std::move(p)){}
    ~Fake(){std::lock_guard<std::mutex> lock(p_->mutex);++p_->destroyed;}
    bool online()const noexcept override{return true;}
    std::optional<app::FileConfiguration> configuration()override{return app::FileConfiguration{};}
    app::FileManagementReadSnapshot read(const app::FileManagementReadRequest& q)override{
        std::unique_lock<std::mutex> lock(p_->mutex);++p_->reads;++p_->active;p_->maximum=std::max(p_->maximum,p_->active);if(p_->block){p_->entered=true;p_->changed.notify_all();p_->changed.wait(lock,[this]{return p_->release;});p_->block=false;}
        app::FileManagementReadSnapshot page;page.code=app::LocalAccountResultCode::ok;page.kind=q.kind;
        if(q.kind==app::FileManagementQueryKind::detail)page.records={record(static_cast<unsigned>(q.selection.value)+(p_->wrong_detail?1:0))};
        else if(q.kind==app::FileManagementQueryKind::records){for(auto n=static_cast<unsigned>(q.after.value)+1;n<=36&&page.records.size()<2;++n)page.records.push_back(record(n));if(!page.records.empty()&&page.records.back().file.id.value<36)page.next=page.records.back().file.id;}
        --p_->active;return page;
    }
    app::FileManagementJobStatus start(const app::FileManagementTask& task)override{
        std::lock_guard<std::mutex> lock(p_->mutex);++p_->starts;const auto bytes=app::encode_file_management_task(task);
        if(p_->exact.empty()){p_->exact=bytes;p_->task=task;++p_->applied;return {};}
        if(bytes!=p_->exact)return {app::LocalAccountResultCode::invalid_request};return completed(task);
    }
    app::FileManagementJobStatus status(app::Identifier128)override{return {app::LocalAccountResultCode::policy_rejected};}
    app::FileManagementJobStatus cancel(app::Identifier128)override{return {};}
};
ui::ClientReply reply(ui::ClientWorker& worker){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(std::chrono::steady_clock::now()<end){auto result=worker.poll();if(result)return std::move(*result);std::this_thread::sleep_for(std::chrono::milliseconds(1));}throw std::runtime_error("typed model reply timeout");}
ui::ClientReply call(ui::ClientWorker& worker,ui::ClientCommand request){check(worker.submit(std::move(request)),"single queued typed command accepted");return reply(worker);}
}
int main()try{
    auto state=std::make_shared<FakeState>();ui::ClientWorker worker("C:/not-used-model",L"fake-owner",[state](const auto& root,const auto& owner,std::string&)->std::unique_ptr<win::IFileAdministrationClient>{std::lock_guard<std::mutex> lock(state->mutex);state->root_exact=root==std::filesystem::path("C:/not-used-model")&&owner==L"fake-owner";++state->factories;return std::make_unique<Fake>(state);});
    ui::ClientCommand request;request.serial=1;request.kind=ui::ClientCommandKind::connect;auto result=call(worker,request);check(result.connected&&result.online&&result.configuration&&state->factories==1&&state->root_exact,"explicit typed connect/configuration and exact selected root; fake ports only");
    {std::lock_guard<std::mutex> lock(state->mutex);state->block=true;}
    request={};request.serial=2;request.kind=ui::ClientCommandKind::read;check(worker.submit(request),"read submitted without blocking UI thread");
    {std::unique_lock<std::mutex> lock(state->mutex);check(state->changed.wait_for(lock,std::chrono::seconds(5),[&]{return state->entered;}),"transport thread reaches actual blocking fake port");}
    auto other=request;other.serial=3;check(worker.busy()&&!worker.submit(other)&&!worker.poll(),"second command is refused while first owner call is active");
    {std::lock_guard<std::mutex> lock(state->mutex);state->release=true;}state->changed.notify_all();result=reply(worker);
    check(result.read_code==app::LocalAccountResultCode::ok&&result.records.size()==32&&result.records.front().file.id.value==1&&result.records.back().file.id.value==32&&result.next.value==32&&state->reads==16&&state->maximum==1,"sixteen bounded two-record frames form one32-row UI page without parallel client calls");
    request.serial=4;request.query.after=result.next;result=call(worker,request);check(result.records.size()==4&&result.records.front().file.id.value==33&&!result.next.value,"next UI page uses exact stable ID cursor");
    request.serial=5;request.query={};request.query.kind=app::FileManagementQueryKind::detail;request.query.selection={7};result=call(worker,request);check(result.read_code==app::LocalAccountResultCode::ok&&result.records.size()==1&&result.records[0].file.id.value==7,"detail binds selected stable identity");
    {std::lock_guard<std::mutex> lock(state->mutex);state->wrong_detail=true;}request.serial=6;result=call(worker,request);check(result.read_code==app::LocalAccountResultCode::unavailable&&result.records.empty(),"mismatched detail cannot be shown as the selected record");
    ui::JobTracker tracking;app::FileManagementTask task;task.job=id(1);task.kind=app::FileManagementTaskKind::operation;task.operation.operation=id(2);task.operation.action=app::FileOperationAction::mark_missing;task.operation.file={9};task.operation.expected_revision=1;
    check(tracking.begin(task)&&tracking.blocks_edits()&&tracking.state()==ui::JobTracking::unknown,"new start freezes edits until definite reply");request={};request.serial=7;request.kind=ui::ClientCommandKind::start;request.task=task;result=call(worker,request);check(!tracking.observe(result.status,true)&&tracking.state()==ui::JobTracking::unknown&&state->applied==1,"lost start response is unknown, never a failure/success assertion");
    auto changed=task;changed.job=id(3);check(!tracking.begin(changed)&&app::encode_file_management_task(*tracking.exact_task())==state->exact,"unknown state blocks new identity and preserves exact retry bytes");request.serial=8;request.kind=ui::ClientCommandKind::status;request.job=task.job;result=call(worker,request);check(!tracking.observe(result.status,false)&&tracking.blocks_edits(),"evicted/missing status cannot prove original request did not execute");
    request.serial=9;request.kind=ui::ClientCommandKind::start;request.task=*tracking.exact_task();result=call(worker,request);check(tracking.observe(result.status,true)&&!tracking.blocks_edits()&&state->starts==2&&state->applied==1,"same canonical retry reconciles completed intent exactly once in fake native port");tracking.link_failed();check(!tracking.blocks_edits(),"unrelated later read failure cannot resurrect completed job");auto late=completed(task);late.state=app::FileManagementJobState::running;late.journal_confirmed=false;late.result={};check(!tracking.observe(late,false)&&!tracking.blocks_edits(),"late status cannot reopen a known terminal receipt");
    task.job=id(4);check(tracking.begin(task),"new intent allowed after proven terminal state");auto mismatched=completed(task);mismatched.job=id(5);check(!tracking.observe(mismatched,false)&&tracking.blocks_edits(),"wrong job response retains freeze");app::FileManagementJobStatus uncertain{app::LocalAccountResultCode::storage_failure};check(!tracking.observe(uncertain,true)&&tracking.blocks_edits(),"storage failure alone is not proof of no execution");app::FileManagementJobStatus refusal{app::LocalAccountResultCode::conflict};check(tracking.observe(refusal,true)&&!tracking.blocks_edits(),"explicit start conflict is a definite refusal, not a running job");
    request={};request.serial=10;request.kind=ui::ClientCommandKind::disconnect;result=call(worker,request);check(!result.connected&&state->destroyed==1,"explicit disconnect releases typed owner client on transport worker before UI exit");request.serial=11;request.kind=ui::ClientCommandKind::configuration;result=call(worker,request);check(!result.connected&&state->factories==1,"read after disconnect does not silently reacquire offline ownership");worker.stop();request.serial=12;check(!worker.submit(request),"normal worker stop prevents further transport calls");
    unsigned failed_factories{};ui::ClientWorker unavailable("C:/not-used-model",L"fake-owner",[&](const auto&,const auto&,std::string& detail)->std::unique_ptr<win::IFileAdministrationClient>{++failed_factories;detail="Fake unavailable";return {};});request={};request.serial=11;request.kind=ui::ClientCommandKind::connect;result=call(unavailable,request);check(!result.connected&&failed_factories==1,"failed factory is surfaced without another root/fallback");request.serial=12;request.kind=ui::ClientCommandKind::configuration;result=call(unavailable,request);check(!result.connected&&failed_factories==1,"ordinary read never silently reconnects or opens offline host");unavailable.stop();
    std::cout<<"PASS File Manager client model "<<checks<<" checks; serialized fake typed ports/unknown-job policy only, NOT native installed relay, SQL, GUI or physical-client evidence\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL File Manager model after "<<checks<<": "<<e.what()<<'\n';return 1;}
