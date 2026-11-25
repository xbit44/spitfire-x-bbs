#include "alpha_caller_files.hpp"
#include "sfx/application/qwk.hpp"
#include <algorithm>
#include <set>

namespace sfx::apps::runtime {
namespace app=application;
namespace win=platform::win32;
namespace inst=win::installation;
namespace {
bool same_actor(const app::FileActor& a,const app::FileActor& b){return a.account==b.account&&a.session==b.session&&a.account_revision==b.account_revision&&a.policy_revision==b.policy_revision;}
bool same_payload(const app::FilePayloadIdentity& a,const app::FilePayloadIdentity& b){return a.bytes==b.bytes&&a.sha256==b.sha256&&a.volume==b.volume&&a.file_id==b.file_id;}
std::optional<std::string> wire_name(std::string_view name){if(!app::valid_file_name(name))return {};auto encoded=app::qwk_utf8_to_cp437(name);if(!encoded||encoded->empty()||encoded->size()>128)return {};return encoded;}
std::string wire_key(std::string value){for(auto& c:value)if(c>='a'&&c<='z')c-=32;return value;}
CallerFileReport report(std::string name,std::string detail){CallerFileReport result;result.name=app::sanitize_file_text(name,128,false);result.detail=std::move(detail);return result;}
}
AlphaCallerFileTransfers::AlphaCallerFileTransfers(CallerFileTransferPorts p):ports_(std::move(p)){
    inst::require(ports_.actor&&ports_.operation&&ports_.local_date&&ports_.channel.authorized,"file_transfer_ports_required");
}
win::FilePayloadDirectory AlphaCallerFileTransfers::area(const app::FileConfiguration& config,app::FileAreaId id)const{
    const auto found=std::find_if(config.areas.begin(),config.areas.end(),[id](const auto& a){return a.area.id==id;});inst::require(found!=config.areas.end(),"file_transfer_area_unavailable");
    std::vector<std::filesystem::path> others;for(const auto& a:config.areas)if(!(a.area.id==id))others.push_back(std::filesystem::u8path(a.storage_path));
    return win::FilePayloadDirectory::area(ports_.installation,std::filesystem::u8path(found->storage_path),others,
        {L"Bin",L"Config",L"Data",L"Display",L"Backup",L"Logs",L"Work",L"Doors",L"BotGate"},ports_.owner);
}
bool AlphaCallerFileTransfers::live(const app::FileActor& actor,std::int64_t revision)const{
    if(!ports_.channel.authorized())return false;
    const auto deadline=ports_.channel.absolute_deadline?ports_.channel.absolute_deadline():0;
    if(deadline&&(!ports_.channel.now||ports_.channel.now()>=deadline))return false;
    const auto current=ports_.actor();if(!current||!same_actor(*current,actor))return false;
    const auto catalog=ports_.storage.file_catalog(actor);return catalog&&catalog.value&&catalog.value->revision==revision;
}
CallerFileBatchResult AlphaCallerFileTransfers::upload(const std::vector<app::FileUploadItem>& queue,bool private_submission,app::TransferProtocolPreference protocol){
    CallerFileBatchResult result;if(queue.empty()||queue.size()>app::kFileQueueMaximum)return result;
    for(const auto& row:queue)result.files.push_back(report(row.name,"Upload unavailable. Nothing added."));
    const app::FileConfigurationActor host{true,{}};const auto actor=ports_.actor();const auto config=ports_.storage.file_configuration(host);
    if(!actor||!config||!config.value||!live(*actor,config.value->policy.catalog_revision))return result;
    auto channel=ports_.channel;channel.authorized=[&]{return live(*actor,config.value->policy.catalog_revision);};
    AlphaFileTransfer transfer(ports_.installation,ports_.owner,std::move(channel),ports_.observer,ports_.work_problem);if(!transfer.available())return result;
    struct Prepared {std::size_t index{};std::string wire;std::unique_ptr<win::WindowsFileIntake> intake;};
    std::vector<Prepared> prepared;std::vector<NativeFileTransferItem> items;std::set<std::string> names;
    try{
        auto staging=win::FilePayloadDirectory::private_store(ports_.installation/L"Data"/L"FileIntake",ports_.owner);
        for(std::size_t i=0;i<queue.size();++i){const auto& row=queue[i];try{
            const auto wire=wire_name(row.name);if(!wire||!names.insert(wire_key(*wire)).second){result.files[i].detail="Filename is unsupported or repeated in this batch. Nothing added.";continue;}
            auto destination=row.destination;
            if(private_submission){const auto found=std::find_if(config.value->areas.begin(),config.value->areas.end(),[](const auto& a){return a.area.kind==app::FileAreaKind::private_sysop;});if(found==config.value->areas.end())continue;destination=found->area.id;}
            // Authoritative upload permission precedes ANY destination probe:
            // denied callers cannot distinguish a private filename's presence.
            auto intake=std::make_unique<win::WindowsFileIntake>(ports_.storage,host,*actor,app::FileIntakeStart{ports_.operation(),destination,private_submission,ports_.local_date()},staging);
            // A private S submission must never use another caller's friendly
            // filename as a pre-transfer existence oracle. Receive privately;
            // normal exact placement/reservation checks hold any collision
            // AFTER receiving, without overwriting or exposing its cause.
            const auto& placement_name=intake->record().placement_name.empty()?row.name:intake->record().placement_name;
            if(!private_submission){
            auto target=area(*config.value,destination);if(!target.absent(placement_name)){(void)intake->finish(row.name,false);result.files[i].held=true;result.files[i].detail="Filename is unavailable. Nothing overwritten; submission held privately.";continue;}
            auto placement=area(*config.value,intake->record().placement_area);if(!placement.absent(placement_name)){(void)intake->finish(row.name,false);result.files[i].held=true;result.files[i].detail="Filename is unavailable; submission held privately.";continue;}
            const auto status=ports_.storage.file_name_status(host,intake->record().placement_area,{placement_name});
            if(!status||!status.value||status.value->size()!=1){
                if(ports_.observer)ports_.observer("file_upload_name_lookup_failed",static_cast<std::uint32_t>(status.status.code),0,0);
                (void)intake->finish(row.name,false);result.files[i].held=true;result.files[i].detail="Upload destination lookup failed. No file data requested; nothing added.";continue;}
            if(status.value->front().registered.value||(status.value->front().pending&&!status.value->front().staging_only_hold)){
                const bool registered=status.value->front().registered.value!=0;
                if(ports_.observer)ports_.observer(registered?"file_upload_name_registered":"file_upload_name_reserved",0,0,0);
                (void)intake->finish(row.name,false);result.files[i].held=true;
                result.files[i].detail=registered?"Filename already registered in the upload destination. No file data requested; nothing overwritten.":"Filename reserved by an unfinished file placement or import. No file data requested; nothing overwritten.";continue;}
            }
            prepared.push_back({i,*wire,std::move(intake)});
        }catch(...){result.files[i].detail="Upload preparation failed. No acceptance is claimed.";}}
        for(auto& entry:prepared){auto* intake=entry.intake.get();inst::require(intake->record().staged_object.has_value(),"file_transfer_stage_unbound");items.push_back({entry.wire,intake->record().maximum_bytes,*intake->record().staged_object,[intake]{return intake->receive_handle();}});}
        if(items.empty())return result;const auto received=transfer.run(file_transfer_detail::Direction::receive,items,protocol);result.connected=received.code!=NativeFileTransferCode::disconnected;
        bool control_disposable=received.code==NativeFileTransferCode::completed&&received.files.size()==prepared.size();
        for(std::size_t i=0;i<prepared.size();++i){auto& entry=prepared[i];auto& out=result.files[entry.index];const auto& requested=queue[entry.index];
            const bool complete=received.code==NativeFileTransferCode::completed&&received.files.size()==prepared.size()&&received.files[i].completed&&live(*actor,config.value->policy.catalog_revision);
            auto outcome=entry.intake->finish(requested.name,complete,ports_.scan);out.scan=outcome.scan;out.bytes=entry.intake->record().payload.bytes;out.received=complete;out.deleted=outcome.deletion_confirmed;
            if(ports_.scan_notice&&outcome.scan.verdict!=app::FileScanVerdict::not_scanned)try{ports_.scan_notice(requested,outcome);}catch(...){} // Logging must not change intake/disposition.
            if(out.deleted){control_disposable=control_disposable&&outcome.journal_confirmed;out.detail=outcome.journal_confirmed?"fail - deleted":"Payload deleted; journal reconciliation required.";continue;}
            out.held=true;out.detail=outcome.detail;
            if(outcome.state!=app::FileIntakeState::ready||!ports_.review||!live(*actor,config.value->policy.catalog_revision)){
                control_disposable=false;
                if(ports_.observer)ports_.observer("file_upload_not_added",static_cast<std::uint32_t>(out.scan.verdict),0,static_cast<std::size_t>(out.bytes));continue;}
            const auto metadata=ports_.review(requested,outcome,out.bytes);
            if(!metadata||!app::valid_file_descriptions(metadata->descriptions)||!live(*actor,config.value->policy.catalog_revision)){
                control_disposable=false;
                (void)ports_.storage.resolve_file_intake(host,entry.intake->record().operation,entry.intake->record().payload,false,"Metadata not accepted; held privately.");out.detail="Metadata not accepted; held privately.";continue;}
            auto destination=area(*config.value,entry.intake->record().placement_area);outcome=entry.intake->accept(ports_.installation,destination,metadata->descriptions,metadata->sysop_only||requested.sysop_only);
            out.held=!outcome.accepted;out.detail=outcome.detail;
            if(outcome.accepted&&outcome.journal_confirmed){out.completed=true;out.file=outcome.accepted->id;out.detail=out.scan.verdict==app::FileScanVerdict::clean?"Scan complete - clean; file added":"Added (malware scanning Off).";}
            else control_disposable=false;
        }
        if(control_disposable&&received.confirm_completed)received.confirm_completed();
    }catch(...){for(auto& entry:prepared){auto& out=result.files[entry.index];if(out.completed||out.deleted)continue;try{(void)entry.intake->finish(queue[entry.index].name,false);(void)ports_.storage.resolve_file_intake(host,entry.intake->record().operation,entry.intake->record().payload,false,"Upload interrupted; held privately.");}catch(...){}out.held=true;out.detail="Upload interrupted; held privately. No acceptance is claimed.";}}
    return result;
}
CallerFileBatchResult AlphaCallerFileTransfers::download(const std::vector<app::FileId>& queue,app::TransferProtocolPreference protocol){
    CallerFileBatchResult result;if(queue.empty()||queue.size()>app::kFileQueueMaximum)return result;
    const app::FileConfigurationActor host{true,{}};const auto actor=ports_.actor();const auto config=ports_.storage.file_configuration(host);
    if(!actor||!config||!config.value||!live(*actor,config.value->policy.catalog_revision))return result;
    auto channel=ports_.channel;channel.authorized=[&]{return live(*actor,config.value->policy.catalog_revision);};AlphaFileTransfer transfer(ports_.installation,ports_.owner,std::move(channel),ports_.observer,ports_.work_problem);
    if(!transfer.available()){result.files.push_back(report({},"Transfer helper unavailable. Nothing downloaded."));return result;}
    struct Prepared {std::size_t index{};app::FileDownloadReservation reservation;win::FilePayloadLease lease;std::string wire;};
    std::vector<Prepared> prepared;std::set<std::string> names;std::set<std::int64_t> ids;
    // Always release uncompleted reservations on normal/exceptional return.
    // If authority is already revoked, dead-session recovery owns that release.
    struct Release {CallerFileTransferPorts& ports;const app::FileActor& actor;std::vector<Prepared>& prepared;~Release(){for(const auto& p:prepared)if(!p.reservation.completed)try{(void)ports.storage.finish_file_download(actor,p.reservation.operation,{},false);}catch(...){}}} release{ports_,*actor,prepared};
    for(const auto id:queue){if(!ids.insert(id.value).second)continue;
        auto out=report({},"File unavailable. Nothing downloaded.");out.file=id;const auto index=result.files.size();result.files.push_back(out);std::optional<app::FileDownloadReservation> reservation;
        try{
            const auto begun=ports_.storage.reserve_file_download(*actor,ports_.operation(),id,ports_.local_date());
            if(!begun||!begun.value)continue;
            if(!begun.value->reservation){result.files[index].detail=begun.value->allowance==app::FileDownloadAllowance::daily_limit?"Daily file-count limit reached.":"Upload/download file-count ratio requires an upload.";continue;}
            reservation=*begun.value->reservation;result.files[index].name=app::sanitize_file_text(reservation->file.name,128,false);result.files[index].bytes=reservation->payload.bytes;
            inst::require(!reservation->replayed&&!reservation->completed,"file_transfer_fresh_reservation_required");
            const auto wire=wire_name(reservation->file.name);inst::require(wire.has_value()&&names.insert(wire_key(*wire)).second,"file_transfer_duplicate_download_name");
            inst::require(reservation->payload.bytes<=app::kMaximumCallerFileBytes,"file_transfer_protocol_size_bound");
            const auto managed=ports_.storage.managed_file(host,id);inst::require(managed&&managed.value&&managed.value->file.revision==reservation->file.revision&&same_payload(managed.value->payload,reservation->payload),"file_transfer_native_context_changed");
            auto directory=area(*config.value,reservation->file.area);auto lease=directory.read(app::file_managed_payload_name(*managed.value));
            inst::require(same_payload(lease.identity([&](auto,auto){return live(*actor,config.value->policy.catalog_revision);}),reservation->payload),"file_transfer_download_payload_changed");
            prepared.push_back({index,*reservation,std::move(lease),*wire});reservation.reset();
        }catch(...){if(reservation)(void)ports_.storage.finish_file_download(*actor,reservation->operation,{},false);}
    }
    if(prepared.empty())return result;
    try{
        std::vector<NativeFileTransferItem> items;for(auto& p:prepared){auto* lease=&p.lease;items.push_back({p.wire,std::max<std::uint64_t>(1,p.reservation.payload.bytes),lease->native_object_identity(),[lease]{return lease->duplicate_transfer_handle(false);}});}
        const auto sent=transfer.run(file_transfer_detail::Direction::send,items,protocol);result.connected=sent.code!=NativeFileTransferCode::disconnected;
        for(std::size_t i=0;i<prepared.size();++i){auto& p=prepared[i];auto& out=result.files[p.index];out.detail="Download incomplete. No success counted.";
            if(sent.code!=NativeFileTransferCode::completed||sent.files.size()!=prepared.size())continue;
            if(sent.files[i].skipped){out.skipped=true;out.detail="Download skipped. No success counted.";continue;}
            if(!sent.files[i].completed||!live(*actor,config.value->policy.catalog_revision))continue;
            const auto actual=p.lease.identity([&](auto,auto){return live(*actor,config.value->policy.catalog_revision);});if(!same_payload(actual,p.reservation.payload))continue;
            auto recorded=ports_.storage.finish_file_download(*actor,p.reservation.operation,actual,true);
            // Same immutable reservation is safe to retry: a committed outcome
            // returns its durable receipt, never a second counter increment.
            if(!recorded)recorded=ports_.storage.finish_file_download(*actor,p.reservation.operation,actual,true);
            if(recorded&&recorded.value&&recorded.value->completed){p.reservation.completed=true;out.completed=true;out.detail="Downloaded.";}
        }
        if(sent.confirm_completed&&std::all_of(prepared.begin(),prepared.end(),[&](const auto& item){return item.reservation.completed||result.files[item.index].skipped;}))sent.confirm_completed();
    }catch(...){/* Unproven successes stay false; release guard retains safety. */}
    return result;
}
} // namespace sfx::apps::runtime
