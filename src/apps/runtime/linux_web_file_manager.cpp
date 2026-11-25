#include "linux_web_file_manager.hpp"
#include <charconv>
#include <climits>

namespace sfx::apps::runtime::web {
namespace app=application;using Json=nlohmann::json;
namespace {
void require(bool ok){if(!ok)throw std::runtime_error("file_manager_request_invalid");}
void keys(const Json& j,std::initializer_list<const char*> names){require(j.is_object()&&j.size()==names.size());for(const auto* n:names)require(j.contains(n));}
std::string text(const Json& j,std::size_t max){require(j.is_string());auto s=j.get<std::string>();require(s.size()<=max&&s.find('\0')==s.npos);return s;}
std::int64_t integer(const Json& j,std::int64_t min=1){const auto s=text(j,20);std::int64_t n{};const auto read=std::from_chars(s.data(),s.data()+s.size(),n);require(read.ec==std::errc{}&&read.ptr==s.data()+s.size()&&n>=min&&n<INT64_MAX&&std::to_string(n)==s);return n;}
std::string id_text(app::Identifier128 id){constexpr char hex[]="0123456789abcdef";std::string out;for(auto b:id.bytes){out+=hex[b>>4];out+=hex[b&15];}return out;}
app::Identifier128 identifier(const Json& j){const auto s=text(j,32);require(s.size()==32);app::Identifier128 id;const auto digit=[](char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("file_manager_identifier_invalid");};for(std::size_t i=0;i<16;++i)id.bytes[i]=static_cast<std::uint8_t>(digit(s[2*i])*16+digit(s[2*i+1]));require(id.valid());return id;}
Json hatch(const app::FtnHatchRequest& h){return {{"operation",id_text(h.operation)},{"file",std::to_string(h.file.value)},{"revision",std::to_string(h.file_revision)},{"catalog_revision",std::to_string(h.configuration_revision)},{"network",h.network},{"tag",h.tag},{"origin",h.origin},{"description",h.description},{"destinations",h.destinations}};}
bool terminal(const WebFileReview& r){return r.known&&r.status.state!=app::FileManagementJobState::running;}
}
app::FileManagementReadRequest web_file_query(const Json& j){
    keys(j,{"kind","area","search","after","selection","pending_after"});app::FileManagementReadRequest q;const auto kind=text(j.at("kind"),16);
    if(kind=="records")q.kind=app::FileManagementQueryKind::records;else if(kind=="detail")q.kind=app::FileManagementQueryKind::detail;
    else if(kind=="operations")q.kind=app::FileManagementQueryKind::operations;else if(kind=="imports")q.kind=app::FileManagementQueryKind::imports;
    else if(kind=="intake")q.kind=app::FileManagementQueryKind::intake;else if(kind=="hatches")q.kind=app::FileManagementQueryKind::hatches;else require(false);
    q.filter.area={integer(j.at("area"),0)};q.filter.search=text(j.at("search"),128);q.filter.field=q.kind==app::FileManagementQueryKind::records?app::FileManagementSearch::filename:app::FileManagementSearch::any;
    if(q.kind==app::FileManagementQueryKind::records)q.filter.state=app::FileCatalogState::available;
    q.after={integer(j.at("after"),0)};q.selection={integer(j.at("selection"),0)};if(!j.at("pending_after").is_null())q.pending_after=identifier(j.at("pending_after"));
    require(app::valid_file_management_read_request(q));return q;
}
app::FileManagementTask web_file_task(const Json& j,app::Identifier128 job,app::Identifier128 operation){
    require(j.is_object()&&j.contains("action"));const auto action=text(j.at("action"),32);app::FileManagementTask t;t.job=job;using K=app::FileManagementTaskKind;
    if(action=="rename"||action=="move"||action=="remove"){
        if(action!="remove")keys(j,{"action","file","revision","destination","destination_revision","filename"});else keys(j,{"action","file","revision"});
        auto& o=t.operation;o.operation=operation;o.file={integer(j.at("file"))};o.expected_revision=integer(j.at("revision"));o.action=action=="rename"?app::FileOperationAction::rename:action=="move"?app::FileOperationAction::move:app::FileOperationAction::purge;
        if(action!="remove")o.filename=text(j.at("filename"),255);
        if(action!="remove"){o.destination={integer(j.at("destination"))};o.expected_destination_revision=integer(j.at("destination_revision"));}
    }else if(action=="pending"||action=="offline"){
        keys(j,{"action"});t.kind=action=="pending"?K::scan_pending:K::scan_offline;t.target=t.job;
    }else if(action=="view"){
        keys(j,{"action","file"});t.kind=K::open_view;t.view_file={integer(j.at("file"))};t.view_archive=true;
    }else if(action=="page"||action=="member"||action=="index"||action=="close_view"||action=="report"){
        keys(j,{"action","target","page"});t.target=identifier(j.at("target"));const auto page=integer(j.at("page"),0);require(page<=65535);t.view_page=static_cast<std::uint16_t>(page);t.view_member=t.view_page;
        t.kind=action=="page"?K::page_view:action=="member"?K::read_view_member:action=="index"?K::view_archive_index:action=="report"?K::scan_report:K::close_view;
    }else if(action=="inspect"){
        keys(j,{"action","area","filename","file"});t.kind=K::inspect_files;t.inspection_request.filter.area={integer(j.at("area"))};t.inspection_request.filter.search=text(j.at("filename"),128);t.inspection_request.after={integer(j.at("file"))-1};
    }else if(action=="hatch"){
        keys(j,{"action","file","revision","catalog_revision","network","tag","origin","description","destinations"});t.kind=K::hatch_file;auto& h=t.hatch;h.operation=operation;h.file={integer(j.at("file"))};h.file_revision=integer(j.at("revision"));h.configuration_revision=integer(j.at("catalog_revision"));h.network=text(j.at("network"),48);h.tag=text(j.at("tag"),128);h.origin=text(j.at("origin"),96);h.description=text(j.at("description"),4096);
        require(j.at("destinations").is_array()&&j.at("destinations").size()<=app::kFtnHatchMaximumDestinations);for(const auto& d:j.at("destinations"))h.destinations.push_back(text(d,64));
    }else{
        keys(j,{"action","target"});t.target=identifier(j.at("target"));
        if(action=="retry_operation")t.kind=K::reconcile_operation;else if(action=="cancel_operation")t.kind=K::cancel_operation;else if(action=="retry_import")t.kind=K::reconcile_import;else if(action=="cancel_import")t.kind=K::cancel_import;else if(action=="retry_intake")t.kind=K::reconcile_intake;else if(action=="retry_hatch")t.kind=K::reconcile_hatch;else require(false);
    }
    require(app::valid_file_management_task(t));return t;
}
Json web_file_snapshot(const app::FileManagementReadSnapshot& s){
    Json rows=Json::array(),pending=Json::array();
    for(const auto& r:s.records){const auto& f=r.file;rows.push_back({{"id",std::to_string(f.id.value)},{"area",std::to_string(f.area.value)},{"revision",std::to_string(f.revision)},{"name",f.name},{"short",f.descriptions.short_text},{"long",f.descriptions.long_text},{"uploader",f.public_uploader},{"bytes",std::to_string(f.bytes)},{"downloads",std::to_string(f.downloads)},{"uploaded",std::to_string(f.uploaded_utc)},{"state",static_cast<unsigned>(f.state)},{"pending",r.operation_pending},{"purged",r.purged},{"sysop_only",f.sysop_only}});}
    for(const auto& p:s.pending)pending.push_back({{"operation",id_text(p.operation)},{"file",std::to_string(p.file.value)},{"area",std::to_string(p.area.value)},{"name",p.filename},{"detail",p.detail},{"state",p.state},{"action",p.action},{"hatch",p.hatch?hatch(*p.hatch):Json(nullptr)}});
    return {{"ok",s.code==app::LocalAccountResultCode::ok},{"records",rows},{"pending",pending},{"next",std::to_string(s.next.value)},{"pending_next",s.pending_next.valid()?Json(id_text(s.pending_next)):Json(nullptr)}};
}
Json web_file_status(const app::FileManagementJobStatus& s){
    constexpr const char* states[]{"Running","Completed","Held","Cancelled","Refused","Failed"};const auto state=static_cast<unsigned>(s.state);Json view=nullptr,inspection=nullptr,report=nullptr;
    if(s.view){const auto& v=*s.view;Json entries=Json::array(),cells=Json::array();for(const auto& entry:v.entries)entries.push_back({{"index",entry.index},{"name",entry.name},{"bytes",std::to_string(entry.bytes)},{"directory",entry.directory},{"encrypted",entry.encrypted},{"readable",entry.readable}});for(const auto& cell:v.cells)cells.push_back({cell.glyph,cell.attribute});
        view={{"cursor",id_text(v.cursor)},{"file",std::to_string(v.file.value)},{"revision",std::to_string(v.revision)},{"filename",v.filename},{"member",v.member},{"archive",v.archive},{"page",v.page},{"total",v.total},{"entries",entries},{"cells",cells}};}
    if(s.inspection){inspection=web_file_snapshot(s.inspection->catalog);inspection["presence"]=Json::array();for(auto p:s.inspection->presence)inspection["presence"].push_back(static_cast<unsigned>(p));}
    if(s.report)report={{"page",s.report->page},{"pages",s.report->pages}};
    return {{"ok",s.code==app::LocalAccountResultCode::ok},{"job",id_text(s.job)},{"state",state<6?states[state]:"Unknown"},{"running",s.state==app::FileManagementJobState::running},{"detail",s.detail},{"bytes_done",std::to_string(s.bytes_done)},{"bytes_total",std::to_string(s.bytes_total)},{"elapsed_ms",std::to_string(s.elapsed_millis)},{"cancel_requested",s.cancellation_requested},{"journal_confirmed",s.journal_confirmed},{"result",std::to_string(s.result.value)},{"view",view},{"inspection",inspection},{"report",report}};
}
Json web_hatch_catalog(const app::FtnHatchCatalog& c){Json networks=Json::array(),echoes=Json::array(),destinations=Json::array();for(const auto& n:c.networks)networks.push_back({{"code",n.code},{"name",n.name},{"addresses",n.addresses},{"enabled",n.enabled}});for(const auto& e:c.echoes)echoes.push_back({{"network",e.network},{"tag",e.tag},{"area",std::to_string(e.file_area)}});for(const auto& d:c.destinations)destinations.push_back({{"network",d.network},{"tag",d.echo},{"link",d.link},{"name",d.name}});return {{"revision",std::to_string(c.revision)},{"networks",networks},{"echoes",echoes},{"destinations",destinations}};}
WebFileActions::WebFileActions(app::IClock& clock,Start start,Read status,Read cancel):clock_(clock),start_(std::move(start)),status_(std::move(status)),cancel_(std::move(cancel)){}
void WebFileActions::expire(){const auto now=clock_.monotonic_millis();for(auto i=states_.begin();i!=states_.end();){const bool old=now-i->second.touched>=600000;if(old)refresh(i->second);if(old&&(!i->second.review.submitted||terminal(i->second.review)))i=states_.erase(i);else ++i;}}
void WebFileActions::refresh(State& s){if(!s.review.submitted||terminal(s.review))return;try{const auto result=status_(s.review.task.job);if(result.job==s.review.task.job){s.review.status=result;s.review.known=true;}}catch(...){s.review.known=false;}}
std::optional<WebFileReview> WebFileActions::prepare(std::string owner,app::FileManagementTask task,Json choices){
    std::lock_guard<std::mutex> lock(mutex_);expire();if(owner.empty()||owner.size()>128||!app::valid_file_management_task(task))return {};
    const auto prior=states_.find(owner);if(prior!=states_.end()){refresh(prior->second);if(prior->second.review.submitted&&!terminal(prior->second.review))return {};}
    if(prior==states_.end()&&states_.size()>=32)return {};
    using K=app::FileManagementTaskKind;
    if(task.kind==K::page_view||task.kind==K::read_view_member||task.kind==K::view_archive_index||task.kind==K::close_view){if(prior==states_.end()||!prior->second.review.status.view||!(prior->second.review.status.view->cursor==task.target))return {};}
    if(task.kind==K::scan_report){if(prior==states_.end())return {};const auto& old=prior->second.review.task;if(!(task.target==(old.kind==K::scan_report?old.target:old.job)))return {};}
    if(task.kind==K::hatch_file){if(prior==states_.end()||!prior->second.review.status.inspection)return {};const auto& p=*prior->second.review.status.inspection;bool matched{};for(std::size_t i=0;i<p.catalog.records.size()&&i<p.presence.size();++i){const auto& f=p.catalog.records[i];if(f.file.id==task.hatch.file&&f.file.revision==task.hatch.file_revision&&!f.operation_pending&&!f.purged&&f.file.state==app::FileCatalogState::available&&p.presence[i]==app::FilePayloadPresence::present)matched=true;}if(!matched)return {};}
    auto& s=states_[std::move(owner)];s={WebFileReview{browser_token(),std::move(task),std::move(choices),false,false,{}},clock_.monotonic_millis()};return s.review;
}
std::optional<WebFileReview> WebFileActions::current(std::string_view owner){std::lock_guard<std::mutex> lock(mutex_);expire();const auto i=states_.find(std::string(owner));if(i==states_.end())return {};refresh(i->second);i->second.touched=clock_.monotonic_millis();return i->second.review;}
std::optional<WebFileReview> WebFileActions::act(std::string_view owner,std::string_view handle,std::string_view action){
    std::lock_guard<std::mutex> lock(mutex_);expire();const auto i=states_.find(std::string(owner));if(i==states_.end()||!browser_secret_equal(i->second.review.handle,handle))return {};auto& s=i->second;auto& r=s.review;s.touched=clock_.monotonic_millis();
    if(action=="discard"){if(r.submitted)return {};states_.erase(i);return {};}
    require(action=="submit"||action=="retry"||action=="status"||action=="cancel");
    try{std::optional<app::FileManagementJobStatus> result;
        if(action=="submit"&&!r.submitted){r.submitted=true;result=start_(r.task);}else if(action=="retry"&&r.submitted&&!terminal(r)){result=start_(r.task);}else if(action=="cancel"&&r.submitted&&!terminal(r)){result=cancel_(r.task.job);}else if(r.submitted&&!terminal(r))result=status_(r.task.job);
        if(result&&result->job==r.task.job){r.status=*result;r.known=true;}
    }catch(...){r.known=false;}
    return r;
}
void WebFileActions::clear(){std::lock_guard<std::mutex> lock(mutex_);states_.clear();}
}
