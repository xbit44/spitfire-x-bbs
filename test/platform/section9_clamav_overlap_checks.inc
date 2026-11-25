// Current production updater, actual installed Runtime and real vendor checks.
// Synthetic writer/gate cases never write logs or launch a vendor while queued.
#include "sfx/platform/win32/clamav_status_gate.hpp"
#include <atomic>
#include <future>
#include <map>
#include <tuple>
#include <thread>
namespace {
using ClamFiles=std::map<ins::Path,std::tuple<std::uintmax_t,std::filesystem::file_time_type,std::string>>;
ClamFiles overlap_files(const ins::Path& root){
    ClamFiles result;
    for(const auto* role:{L"Bin/Tools/ClamAV",L"Config/ClamAV",L"Data/ClamAV",L"Logs/ClamAV"})
        for(const auto& entry:std::filesystem::recursive_directory_iterator(root/role)){
            auto path=entry.path();path.make_preferred();
            check(!(GetFileAttributesW(path.c_str())&FILE_ATTRIBUTE_REPARSE_POINT),"no redirected overlap input");
            if(entry.is_regular_file())result.emplace(path.lexically_relative(root),std::make_tuple(entry.file_size(),entry.last_write_time(),ins::file_sha256(path)));
        }
    return result;
}
bool vendor_log_write_open(const ins::Path& root){
    ins::Handle file(CreateFileW((root/L"Logs"/L"ClamAV"/L"freshclam.log").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    return file.get()==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION;
}
struct OverlapBlock {
    ins::Handle gate,writer;
    OverlapBlock(const ins::Path& root,const std::wstring& owner,bool admitted=true){
        if(admitted)gate=win::clamav_status_gate(win::FilePayloadDirectory::private_store(root/L"Work"/L"ClamAV",owner),owner);
        writer.reset(CreateFileW((root/L"Logs"/L"ClamAV"/L"freshclam.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        check(writer.get()!=INVALID_HANDLE_VALUE,"synthetic write-open only; no log write/truncation");
    }
    void close(){writer.reset();gate.reset();}
};
bool await_progress(const std::atomic<unsigned>& progress){const auto begin=GetTickCount64();while(!progress.load()&&GetTickCount64()-begin<3000)Sleep(10);return progress.load()>0;}
std::int64_t overlap_now(){return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
}
void clamav_overlap_checks(const ins::Path& root){
    const auto owner=ins::operator_sid();const auto receipt=ins::read_receipt(root,owner);
    win::verify_clamav_installation(root,owner,receipt.id);win::initialize_clamav_data(root,owner);
    const auto initial=win::inspect_clamav_status(root,owner,receipt.id);
    check(initial.verified&&initial.definitions_verified,"real ready definitions before overlap checks");
    const auto baseline=overlap_files(root);check(baseline.size()==37,"all37 persistent ClamAV files bound before waiting cases");
    const auto work=win::FilePayloadDirectory::private_store(root/L"Work"/L"ClamAV",owner);const auto work_before=work.names();
    {
        OverlapBlock block(root,owner);std::atomic<bool> cancel{};std::atomic<unsigned> progress{};
        auto waiting=std::async(std::launch::async,[&]{return win::update_clamav_definitions(root,owner,receipt.id,overlap_now(),[&]{return cancel.load();},[&](std::uint64_t){++progress;},5000,win::ClamAvUpdateMode::manual);});
        const bool queued=await_progress(progress);cancel=true;const auto result=waiting.get();block.close();
        std::cout<<"WAIT_CANCEL "<<result.diagnostic<<" progress="<<progress<<std::endl;
        check(queued&&result.cancelled&&!result.attempted&&!result.succeeded&&result.process_reaped&&result.cleanup_complete&&result.diagnostic=="clamav_update_cancelled","queued manual cancellation precedes vendor/log preflight and needs no scratch cleanup");
        check(work.names()==work_before,"cancelled wait creates no scratch");
    }
    {
        OverlapBlock block(root,owner);unsigned progress{};
        const auto result=win::update_clamav_definitions(root,owner,receipt.id,overlap_now(),{},[&](std::uint64_t){++progress;},200,win::ClamAvUpdateMode::manual);block.close();
        std::cout<<"WAIT_DEADLINE "<<result.diagnostic<<" progress="<<progress<<std::endl;
        check(progress&&!result.attempted&&!result.succeeded&&result.process_reaped&&result.cleanup_complete&&result.diagnostic=="clamav_update_timeout","total deadline bounds queued manual request without scratch or vendor");
        check(work.names()==work_before,"timed-out wait creates no scratch");
    }
    {
        OverlapBlock block(root,owner,false);
        const auto result=win::update_clamav_definitions(root,owner,receipt.id,overlap_now(),{},{},1000,win::ClamAvUpdateMode::manual);block.close();
        check(!result.attempted&&!result.succeeded&&result.process_reaped&&result.cleanup_complete&&result.diagnostic=="file_object_open_failed","unmanaged writer still refused; sharing protection never widened");
        check(work.names()==work_before,"refused external writer creates no scratch");
    }
    check(overlap_files(root)==baseline,"waiting cancel timeout and external writer preserve all37 bytes and mtime");
    const auto unchanged=win::inspect_clamav_status(root,owner,receipt.id);
    check(unchanged.definitions_verified&&unchanged.last_attempt==initial.last_attempt&&unchanged.last_success==initial.last_success,"queued failures preserve durable successful history");
    {
        OverlapBlock block(root,owner);std::atomic<bool> cancel{};std::atomic<unsigned> progress{};
        auto waiting=std::async(std::launch::async,[&]{return win::update_clamav_definitions(root,owner,receipt.id,overlap_now(),[&]{return cancel.load();},[&](std::uint64_t){++progress;},180000,win::ClamAvUpdateMode::manual);});
        const bool queued=await_progress(progress);if(!queued)cancel=true;
        const bool no_scratch=work.names()==work_before;block.close();const auto result=waiting.get();
        std::cout<<"WAIT_RESUME "<<result.diagnostic<<" progress="<<progress<<std::endl;
        check(queued&&no_scratch&&result.attempted&&result.succeeded&&result.process_reaped&&result.cleanup_complete,"released active writer admits real verified manual vendor check, no false busy or leaked scratch");
        check(work.names()==work_before,"resumed manual operation cleans its exact scratch before handing off admission");
    }
    const auto final=win::inspect_clamav_status(root,owner,receipt.id);
    check(final.definitions_verified&&final.last_mode==win::ClamAvUpdateMode::manual&&final.last_result=="clamav_definitions_verified","resumed manual outcome is actually persisted and verified");
}
void clamav_initial_overlap_checks(const ins::Path& root){
    const auto owner=ins::operator_sid();const auto receipt=ins::read_receipt(root,owner);
    const auto before=win::inspect_clamav_status(root,owner,receipt.id);
    check(before.verified&&!before.definitions_verified,"EXISTING engine without successful definitions, not fresh absent-ClamAV scenario");
    Runtime runtime(root);win::WindowsSecureIdentifierGenerator ids;
    const auto status=app::read_status_snapshot(0,[&](const app::AlphaStatusRequest& query)->std::optional<std::vector<std::uint8_t>>{return runtime.exchange(app::LocalControlMessageType::status_request,app::LocalControlMessageType::status_response,app::encode_status_request(query));});
    check(status.has_value(),"actual Runtime epoch before initial automatic/manual overlap");
    const auto page=[&]{const auto result=app::decode_log_page(runtime.exchange(app::LocalControlMessageType::log_page_request,app::LocalControlMessageType::log_page_response,app::encode_log_cursor({status->runtime_epoch,0})));check(result.has_value(),"actual bounded initial-overlap Events page");return *result;};
    const auto has_auto_success=[&]{for(const auto& encoded:page().entries){const auto record=app::decode_operational_record(encoded.record,encoded.sequence);if(record&&record->event&&record->event->context.resource_name=="ClamAV definitions verified")return true;}return false;};
    const auto started=GetTickCount64();while(!vendor_log_write_open(root)&&GetTickCount64()-started<30000)Sleep(10);
    check(vendor_log_write_open(root),"real automatic FreshClam has vendor log write-open before manual request");
    const auto operation=ids.generate();const auto now=overlap_now();
    const auto send=[&](app::ManualClamAvPhase phase,const std::string& diagnostic){const auto response=app::decode_control_response(runtime.exchange(app::LocalControlMessageType::clamav_manual_notice_request,app::LocalControlMessageType::clamav_manual_notice_response,app::encode_manual_clamav_notice({operation,phase,phase==app::ManualClamAvPhase::started?0:now,diagnostic})));check(response&&response->code==app::LocalControlResponseCode::ok,"actual authenticated initial-overlap manual Event accepted");};
    send(app::ManualClamAvPhase::started,{});bool waited_before_auto_done{};
    const auto result=win::update_clamav_definitions(root,owner,receipt.id,now,{},[&](std::uint64_t){if(!waited_before_auto_done&&!has_auto_success()&&vendor_log_write_open(root))waited_before_auto_done=true;},180000,win::ClamAvUpdateMode::manual);
    std::cout<<"INITIAL_OVERLAP "<<result.diagnostic<<" waited_before_auto_success="<<waited_before_auto_done<<std::endl;
    check(waited_before_auto_done&&result.attempted&&result.succeeded&&result.process_reaped&&result.cleanup_complete,"manual request waits during REAL initial automatic download then verifies with real vendor");
    send(app::ManualClamAvPhase::verified,result.diagnostic);
    unsigned automatic{},manual_start{},manual_done{},problems{};
    for(const auto& encoded:page().entries){const auto record=app::decode_operational_record(encoded.record,encoded.sequence);check(record.has_value(),"initial-overlap Event decodes");if(!record->event)continue;
        const auto& event=*record->event;const auto& name=event.context.resource_name;
        if(name=="ClamAV definitions verified")++automatic;
        if(name=="Manual ClamAV definition update started")++manual_start;
        if(name=="Manual ClamAV definition update completed; definitions verified")++manual_done;
        if(event.kind==app::OperationalKind::file_maintenance_problem)++problems;
    }
    check(automatic==1&&manual_start==1&&manual_done==1&&problems==0,"real initial automatic and manual success both logged, no scratch-retained handoff failure");
    const auto final=win::inspect_clamav_status(root,owner,receipt.id);
    check(final.definitions_verified&&final.last_mode==win::ClamAvUpdateMode::manual&&final.last_attempt==now,"current exact definitions and manual history agree after real overlap");
    runtime.stop();
}
