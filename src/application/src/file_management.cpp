#include "sfx/application/file_management.hpp"
#include "sfx/application/ftn_work.hpp"
#include <algorithm>
#include <limits>
namespace sfx::application {
namespace {
bool file_owner_text(std::string_view text,std::size_t maximum,bool empty=false){
    return (empty||!text.empty())&&text.size()<=maximum&&valid_file_utf8(text)&&
        std::all_of(text.begin(),text.end(),[](unsigned char c){return c>=32&&c!=127;});
}
bool file_owner_payload(const FilePayloadIdentity& p){return p.bytes<=static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())&&
    file_owner_text(p.volume,128)&&file_owner_text(p.file_id,128)&&std::any_of(p.sha256.begin(),p.sha256.end(),[](auto b){return b!=0;});}
bool file_owner_import(const FileImportRequest& r){
    const auto& f=r.registration;
    return r.mode<=FileImportMode::move&&file_owner_text(r.source_path,8192)&&f.operation.valid()&&f.area.value>0&&f.expected_area_revision>0&&
        valid_file_name(f.filename)&&valid_file_descriptions(f.descriptions)&&file_owner_text(f.public_uploader,64)&&file_owner_payload(f.payload)&&
        f.origin==FileOrigin::sysop_import&&!f.uploader&&valid_utc_seconds(f.original_upload_utc);
}
bool file_owner_purge(const FileIntakePurge& p){return p.operation.valid()&&(!p.staging||file_owner_payload(*p.staging))&&(!p.placed||file_owner_payload(*p.placed));}
bool file_owner_record(const ManagedFile& r){const auto& f=r.file;
    if(!r.native_basename.empty()&&(r.native_basename.size()!=32||f.origin!=FileOrigin::caller_upload||!std::all_of(r.native_basename.begin(),r.native_basename.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})))return false;
    return f.id.value>0&&f.area.value>0&&f.revision>0&&f.publication_sequence>0&&valid_utc_seconds(f.published_utc)&&valid_utc_seconds(f.uploaded_utc)&&valid_file_name(f.name)&&valid_file_descriptions(f.descriptions)&&file_owner_text(f.public_uploader,64,f.origin==FileOrigin::ftn)&&(!f.uploader||(*f.uploader>0&&f.origin!=FileOrigin::ftn))&&f.bytes<=INT64_MAX&&f.downloads<=INT64_MAX&&f.origin<=FileOrigin::ftn&&f.state<=FileCatalogState::missing&&!f.new_to_caller&&file_owner_payload(r.payload)&&r.payload.bytes==f.bytes&&r.payload.sha256==f.sha256&&r.intended_area.value>=0&&file_owner_text(r.removed_key,128,true);
}
LocalAccountResultCode management_read_code(StorageErrorCode code){
    if(code==StorageErrorCode::invalid_argument)return LocalAccountResultCode::invalid_request;
    if(code==StorageErrorCode::conflict)return LocalAccountResultCode::conflict;
    if(code==StorageErrorCode::permission_denied||code==StorageErrorCode::not_found)return LocalAccountResultCode::policy_rejected;
    return LocalAccountResultCode::storage_failure;
}
}
bool valid_file_management_read_request(const FileManagementReadRequest& q) noexcept {
    if(q.kind>FileManagementQueryKind::hatches)return false;
    const auto& f=q.filter;
    if(q.kind==FileManagementQueryKind::records)return !q.selection.value&&!q.pending_after.valid()&&q.after.value>=0&&f.area.value>=0&&(!f.state||*f.state<=FileCatalogState::missing)&&f.field<=FileManagementSearch::uploader&&file_owner_text(f.search,128,true);
    if(f.area.value||f.state||f.field!=FileManagementSearch::any||!f.search.empty()||q.after.value)return false;
    if(q.kind==FileManagementQueryKind::hatches)return q.selection.value>0;
    return q.kind==FileManagementQueryKind::detail?q.selection.value>0&&!q.pending_after.valid():!q.selection.value;
}
bool valid_file_management_read_snapshot(const FileManagementReadSnapshot& s) noexcept {
    if(s.code>LocalAccountResultCode::system_sysop_protected||s.kind>FileManagementQueryKind::hatches)return false;
    if(s.code!=LocalAccountResultCode::ok)return s.records.empty()&&s.pending.empty()&&!s.next.value&&!s.pending_next.valid();
    if(s.kind==FileManagementQueryKind::records||s.kind==FileManagementQueryKind::detail){
        if(!s.pending.empty()||s.pending_next.valid()||s.next.value<0||s.records.size()>kFileManagementRecordPage||(s.kind==FileManagementQueryKind::detail&&(s.records.size()!=1||s.next.value)))return false;
        std::int64_t prior{};for(const auto& row:s.records){if(!file_owner_record(row)||row.file.id.value<=prior)return false;prior=row.file.id.value;}
        return !s.next.value||(!s.records.empty()&&s.next.value==s.records.back().file.id.value);
    }
    if(!s.records.empty()||s.next.value||s.pending.size()>kFileManagementPendingPage)return false;
    if(s.kind==FileManagementQueryKind::hatches&&s.pending.size()>1)return false;
    Identifier128 prior;for(const auto& row:s.pending){
        if(!row.operation.valid()||(prior.valid()&&!(prior.bytes<row.operation.bytes))||row.file.value<0||row.area.value<0||(!row.filename.empty()&&!valid_file_name(row.filename))||!file_owner_text(row.detail,512,true))return false;
        if(s.kind==FileManagementQueryKind::hatches){if(!row.hatch||!valid_ftn_hatch_request(*row.hatch)||!(row.hatch->operation==row.operation)||!(row.hatch->file==row.file)||row.action||row.state>static_cast<unsigned>(FtnHatchState::held))return false;}
        else{if(row.hatch)return false;if(s.kind==FileManagementQueryKind::intake?(row.state>static_cast<unsigned>(FileIntakeState::held)||row.action!=0):(row.state>static_cast<unsigned>(FileOperationState::cancelled)||row.action>static_cast<unsigned>(FileOperationAction::mark_missing)))return false;}
        if(s.kind==FileManagementQueryKind::imports&&row.action)return false;
        prior=row.operation;
    }
    return !s.pending_next.valid()||(!s.pending.empty()&&s.pending_next==s.pending.back().operation);
}
LocalFileManagementQueries::LocalFileManagementQueries(IFileStorage& storage,LocalAccountAuthority authority,IFtnMailStorage* ftn):storage_(storage),authority_(authority),ftn_(ftn){}
LocalFileManagementQueries::LocalFileManagementQueries(IFileStorage& storage,MessageActor actor):storage_(storage),authority_(LocalAccountAuthority::none),caller_(std::move(actor)){}
FileManagementReadSnapshot LocalFileManagementQueries::snapshot(const FileManagementReadRequest& q){
    FileManagementReadSnapshot result;if(q.kind<=FileManagementQueryKind::hatches)result.kind=q.kind;
    if(authority_!=LocalAccountAuthority::installation_owner&&!caller_){result.code=LocalAccountResultCode::unauthorized;return result;}
    if(caller_&&q.kind!=FileManagementQueryKind::records&&q.kind!=FileManagementQueryKind::detail){result.code=LocalAccountResultCode::unauthorized;return result;}
    if(!valid_file_management_read_request(q)){result.code=LocalAccountResultCode::invalid_request;return result;}
    try{
        const FileConfigurationActor owner{authority_==LocalAccountAuthority::installation_owner,caller_};
        if(q.kind==FileManagementQueryKind::hatches){if(!ftn_)return result;const auto read=ftn_->ftn_hatches_for_file(owner,q.selection,q.pending_after,2);if(!read||!read.value){result.code=management_read_code(read.status.code);return result;}for(const auto& row:*read.value){if(!result.pending.empty()){result.pending_next=result.pending.back().operation;break;}FileManagementPending item;item.operation=row.request.operation;item.file=row.request.file;item.area=row.source_area;item.state=static_cast<std::uint8_t>(row.state);item.filename=row.filename;item.detail=row.reason;item.hatch=row.request;result.pending.push_back(std::move(item));}result.code=LocalAccountResultCode::ok;return result;}
        if(q.kind==FileManagementQueryKind::records){const auto read=storage_.managed_file_page(owner,q.filter,q.after,kFileManagementRecordPage);if(!read||!read.value){result.code=management_read_code(read.status.code);return result;}result.records=read.value->files;for(auto& row:result.records)row.file.descriptions.long_text.clear();result.next=read.value->next;}
        else if(q.kind==FileManagementQueryKind::detail){const auto read=storage_.managed_file(owner,q.selection);if(!read||!read.value){result.code=management_read_code(read.status.code);return result;}result.records.push_back(*read.value);}
        else if(q.kind==FileManagementQueryKind::operations){const auto read=storage_.pending_file_operations(owner,q.pending_after,kFileManagementPendingPage+1);if(!read||!read.value){result.code=management_read_code(read.status.code);return result;}for(const auto& row:*read.value){if(result.pending.size()==kFileManagementPendingPage){result.pending_next=result.pending.back().operation;break;}result.pending.push_back({row.request.operation,row.request.file,row.request.destination,static_cast<std::uint8_t>(row.state),static_cast<std::uint8_t>(row.request.action),row.source.file.name,sanitize_file_text(row.detail,512,false)});}}
        else if(q.kind==FileManagementQueryKind::imports){const auto read=storage_.pending_file_imports(owner,q.pending_after,kFileManagementPendingPage+1);if(!read||!read.value){result.code=management_read_code(read.status.code);return result;}for(const auto& row:*read.value){if(result.pending.size()==kFileManagementPendingPage){result.pending_next=result.pending.back().operation;break;}result.pending.push_back({row.request.registration.operation,{},row.request.registration.area,static_cast<std::uint8_t>(row.state),0,row.request.registration.filename,sanitize_file_text(row.detail,512,false)});}}
        else {const auto read=storage_.pending_file_intake(owner,kFileManagementPendingPage+1,q.pending_after);if(!read||!read.value){result.code=management_read_code(read.status.code);return result;}for(const auto& row:*read.value){if(result.pending.size()==kFileManagementPendingPage){result.pending_next=result.pending.back().operation;break;}result.pending.push_back({row.operation,row.accepted_file,row.area,static_cast<std::uint8_t>(row.state),0,row.filename,sanitize_file_text(row.scan.detail,512,false)});}}
        result.code=LocalAccountResultCode::ok;if(!valid_file_management_read_snapshot(result))return {LocalAccountResultCode::storage_failure};return result;
    }catch(...){return {};}
}
bool valid_file_management_operation(const FileOperationRequest& r) noexcept {
    if(!r.operation.valid()||r.file.value<=0||r.expected_revision<=0||r.expected_revision==std::numeric_limits<std::int64_t>::max()||r.action>FileOperationAction::mark_missing||r.destination.value<0||r.expected_destination_revision<0)return false;
    if(r.action==FileOperationAction::metadata)return !r.destination.value&&!r.expected_destination_revision&&r.filename.empty()&&valid_file_descriptions(r.descriptions)&&file_owner_text(r.public_uploader,64,true);
    if(r.action==FileOperationAction::rename||r.action==FileOperationAction::copy||r.action==FileOperationAction::move||r.action==FileOperationAction::restore)
        return r.destination.value>0&&r.expected_destination_revision>0&&valid_file_name(r.filename)&&r.descriptions.short_text.empty()&&r.descriptions.long_text.empty()&&r.public_uploader.empty();
    return !r.destination.value&&!r.expected_destination_revision&&r.filename.empty()&&r.descriptions.short_text.empty()&&r.descriptions.long_text.empty()&&r.public_uploader.empty();
}
bool valid_file_management_task(const FileManagementTask& t) noexcept {
    if(!t.job.valid()||t.kind>FileManagementTaskKind::reconcile_hatch)return false;
    switch(t.kind){
    case FileManagementTaskKind::hatch_file:return valid_ftn_hatch_request(t.hatch);
    case FileManagementTaskKind::operation:return valid_file_management_operation(t.operation);
    case FileManagementTaskKind::preview_import:return t.choice.operation.valid()&&t.choice.area.value>0&&file_owner_text(t.choice.source_path,8192)&&
        (t.choice.filename.empty()||valid_file_name(t.choice.filename))&&file_owner_text(t.choice.public_uploader,64,true)&&t.choice.mode<=FileImportMode::move&&(!t.choice.descriptions||valid_file_descriptions(*t.choice.descriptions));
    case FileManagementTaskKind::import_file:return file_owner_import(t.import);
    case FileManagementTaskKind::purge_intake:return !t.purge.completed&&file_owner_purge(t.purge);
    case FileManagementTaskKind::reupload_block:return t.block.operation.valid()&&t.block.file.value>0&&t.block.expected_revision>0&&t.block.expected_revision<std::numeric_limits<std::int64_t>::max();
    case FileManagementTaskKind::discover_unregistered:return t.discovery_area.value>0;
    case FileManagementTaskKind::discover_import_folder:return t.discovery_area.value>0&&file_owner_text(t.discovery_folder,8192);
    case FileManagementTaskKind::inspect_files:return t.inspection_request.kind==FileManagementQueryKind::records&&valid_file_management_read_request(t.inspection_request);
    case FileManagementTaskKind::open_view:return t.view_file.value>0;
    case FileManagementTaskKind::scan_report:return t.target.valid()&&t.view_page<512;
    case FileManagementTaskKind::page_view:
    case FileManagementTaskKind::view_archive_index:return t.target.valid()&&t.view_page<500;
    case FileManagementTaskKind::read_view_member:return t.target.valid()&&t.view_member<10000;
    default:return t.target.valid();
    }
}
bool valid_file_management_view(const FileManagementViewPage& v) noexcept {
    if(!v.cursor.valid()||v.file.value<=0||v.revision<=0||!valid_file_name(v.filename)||!file_owner_text(v.member,512,true))return false;
    if(v.archive){
        if(v.total>10000||!v.cells.empty()||!v.member.empty()||v.page>499)return false;
        const auto first=static_cast<std::size_t>(v.page)*kFileViewArchivePage;if(first>v.total||(first==v.total&&first!=0)||v.entries.size()!=std::min<std::size_t>(kFileViewArchivePage,v.total-first))return false;
        for(std::size_t n=0;n<v.entries.size();++n){const auto& e=v.entries[n];if(e.index!=first+n||!file_owner_text(e.name,512)||e.bytes>=UINT32_MAX||(e.readable&&(e.directory||e.encrypted)))return false;}return true;
    }
    if(!v.total||v.total>1000||!v.entries.empty()||v.page>=40)return false;
    const auto first=static_cast<std::size_t>(v.page)*kFileViewRows;if(first>=v.total||v.cells.size()!=std::min<std::size_t>(kFileViewRows,v.total-first)*kFileViewColumns)return false;
    return std::all_of(v.cells.begin(),v.cells.end(),[](const auto& c){return c.glyph>=32&&c.glyph!=127;});
}
bool valid_file_management_status(const FileManagementJobStatus& s) noexcept {
    if(s.code>LocalAccountResultCode::system_sysop_protected)return false;
    if(s.code!=LocalAccountResultCode::ok)return !s.preview&&!s.purge_preview&&!s.discovery&&!s.inspection&&!s.view&&!s.report&&!s.journal_confirmed&&!s.result.value;
    const bool bulk=s.kind==FileManagementTaskKind::scan_pending||s.kind==FileManagementTaskKind::scan_offline||s.kind==FileManagementTaskKind::scan_report;
    const bool detail_ok=s.detail.size()<=(bulk?16384u:512u)&&valid_file_utf8(s.detail)&&std::all_of(s.detail.begin(),s.detail.end(),[bulk](unsigned char c){return (bulk&&c==10)||(c>=32&&c!=127);});
    if(!s.job.valid()||s.kind>FileManagementTaskKind::reconcile_hatch||s.state>FileManagementJobState::failed||s.bytes_done>s.bytes_total||s.result.value<0||!detail_ok)return false;
    if(s.report&&(!bulk||s.report->pages==0||s.report->pages>512||s.report->page>=s.report->pages||s.state==FileManagementJobState::running))return false;
    if(s.kind==FileManagementTaskKind::scan_report&&s.state==FileManagementJobState::completed)return s.report.has_value()&&!s.journal_confirmed&&!s.result.value&&!s.duplicate_contents&&!s.preview&&!s.purge_preview&&!s.discovery&&!s.inspection&&!s.view;
    if(s.duplicate_contents&&(s.kind!=FileManagementTaskKind::preview_import||s.state!=FileManagementJobState::completed))return false;
    if(s.preview&&(s.kind!=FileManagementTaskKind::preview_import||s.state!=FileManagementJobState::completed||!file_owner_import(*s.preview)))return false;
    if(s.purge_preview&&(s.kind!=FileManagementTaskKind::preview_intake_purge||s.state!=FileManagementJobState::completed||!file_owner_purge(*s.purge_preview)))return false;
    if(s.discovery){const auto& d=*s.discovery;if((s.kind!=FileManagementTaskKind::discover_unregistered&&s.kind!=FileManagementTaskKind::continue_unregistered&&s.kind!=FileManagementTaskKind::discover_import_folder)||s.state!=FileManagementJobState::completed||!(d.cursor==s.job)||d.area.value<=0||d.names.size()>100||!file_owner_text(d.source_folder,8192,true))return false;for(const auto& name:d.names)if(!valid_file_name(name))return false;}
    if(s.inspection){const auto& i=*s.inspection;if(s.kind!=FileManagementTaskKind::inspect_files||s.state!=FileManagementJobState::completed||i.catalog.code!=LocalAccountResultCode::ok||i.catalog.kind!=FileManagementQueryKind::records||!valid_file_management_read_snapshot(i.catalog)||i.presence.size()!=i.catalog.records.size())return false;for(const auto p:i.presence)if(p>FilePayloadPresence::pending)return false;}
    if(s.view&&((s.kind!=FileManagementTaskKind::open_view&&s.kind!=FileManagementTaskKind::page_view&&s.kind!=FileManagementTaskKind::read_view_member&&s.kind!=FileManagementTaskKind::view_archive_index)||s.state!=FileManagementJobState::completed||!(s.view->cursor==s.job)||!valid_file_management_view(*s.view)))return false;
    if(s.state==FileManagementJobState::completed){
        if(s.kind==FileManagementTaskKind::preview_import)return s.preview.has_value()&&!s.journal_confirmed&&!s.result.value;
        if(s.kind==FileManagementTaskKind::preview_intake_purge)return s.purge_preview.has_value()&&!s.journal_confirmed&&!s.result.value;
        if(s.kind==FileManagementTaskKind::discover_unregistered||s.kind==FileManagementTaskKind::continue_unregistered||s.kind==FileManagementTaskKind::discover_import_folder)return s.discovery.has_value()&&!s.journal_confirmed&&!s.result.value;
        if(s.kind==FileManagementTaskKind::cancel_unregistered)return !s.discovery&&!s.inspection&&!s.journal_confirmed&&!s.result.value;
        if(s.kind==FileManagementTaskKind::inspect_files)return s.inspection.has_value()&&!s.journal_confirmed&&!s.result.value;
        if(s.kind==FileManagementTaskKind::open_view||s.kind==FileManagementTaskKind::page_view||s.kind==FileManagementTaskKind::read_view_member||s.kind==FileManagementTaskKind::view_archive_index)return s.view.has_value()&&!s.journal_confirmed&&!s.result.value;
        if(s.kind==FileManagementTaskKind::close_view)return !s.view&&!s.report&&!s.journal_confirmed&&!s.result.value;
        return s.journal_confirmed;
    }
    return !s.preview&&!s.purge_preview&&!s.discovery&&!s.inspection&&!s.view&&!s.result.value;
}
} // namespace sfx::application
