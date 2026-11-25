#include "linux_web_ftn_drafts.hpp"
#include <algorithm>

namespace sfx::apps::runtime::web {
namespace {
namespace app=application;using Code=app::FtnConfigurationCode;
void require(bool condition,const char* error){if(!condition)throw std::runtime_error(error);}
void writable(const WebFtnDraft& draft,std::uint64_t revision){require(draft.state==WebFtnDraftState::editing,"ftn_draft_locked");require(draft.edit_revision==revision,"ftn_draft_revision_changed");}
}
WebFtnDrafts::WebFtnDrafts(FtnExchange exchange,FtnSeal seal,TokenFactory token,Now now,FtnListReader reader):exchange_(std::move(exchange)),seal_(std::move(seal)),token_(std::move(token)),now_(std::move(now)),list_reader_(std::move(reader)){}
WebFtnDrafts::~WebFtnDrafts(){clear();}
app::FtnTransferToken WebFtnDrafts::new_token(){const auto token=token_();require(std::any_of(token.begin(),token.end(),[](auto b){return b!=0;})&&!entries_.count(token),"ftn_token_unavailable");return token;}
void WebFtnDrafts::expire(){const auto now=now_();for(auto i=entries_.begin();i!=entries_.end();){if(now-i->second.last>=900000){if(i->second.value.state==WebFtnDraftState::review)web_ftn_cancel(exchange_,i->second.prepared);i=entries_.erase(i);}else ++i;}}
WebFtnDrafts::Entry& WebFtnDrafts::find(const std::string& owner,app::FtnTransferToken handle){expire();auto i=entries_.find(handle);require(i!=entries_.end()&&i->second.owner==owner,"ftn_draft_unavailable");i->second.last=now_();return i->second;}
WebFtnDraft WebFtnDrafts::open(const std::string& owner){
    std::lock_guard<std::mutex> lock(mutex_);expire();require(!owner.empty()&&owner.size()<=128,"ftn_owner_invalid");require(entries_.size()<4,"ftn_drafts_busy");
    const auto handle=new_token();auto snapshot=web_ftn_read(exchange_,handle);require(snapshot.code==Code::ok&&snapshot.configuration.has_value(),"ftn_catalog_unavailable");
    Entry entry;entry.owner=owner;entry.last=now_();entry.value.handle=handle;entry.value.configuration=std::move(*snapshot.configuration);
    return entries_.emplace(handle,std::move(entry)).first->second.value;
}
WebFtnDraft WebFtnDrafts::read(const std::string& owner,app::FtnTransferToken handle){std::lock_guard<std::mutex> lock(mutex_);return find(owner,handle).value;}
WebFtnDraft WebFtnDrafts::edit(const std::string& owner,app::FtnTransferToken handle,std::uint64_t revision,const nlohmann::json& request){
    std::lock_guard<std::mutex> lock(mutex_);auto& e=find(owner,handle);writable(e.value,revision);require(e.value.edit_revision<UINT64_MAX,"ftn_draft_revision_limit");
    auto candidate=web_ftn_record_update(request,e.value.configuration,seal_);require(!app::encode_ftn_configuration(candidate).empty(),"ftn_catalog_limit");
    e.value.configuration=std::move(candidate);++e.value.edit_revision;return e.value;
}
WebFtnDraft WebFtnDrafts::prepare(const std::string& owner,app::FtnTransferToken handle,std::uint64_t revision){
    std::lock_guard<std::mutex> lock(mutex_);auto& e=find(owner,handle);writable(e.value,revision);
    e.value.operation=new_token();e.prepared=web_ftn_prepare(exchange_,e.value.operation,e.value.configuration);
    if(e.prepared.code!=Code::ok){app::FtnConfigurationResponse response;response.code=e.prepared.code;e.value.result=response;e.value.operation={};return e.value;}
    e.value.review=e.prepared.review;e.value.result.reset();e.value.state=WebFtnDraftState::review;return e.value;
}
WebFtnDraft WebFtnDrafts::edit_batch(const std::string& owner,app::FtnTransferToken handle,std::uint64_t revision,const nlohmann::json& changes){
    std::lock_guard<std::mutex> lock(mutex_);auto& e=find(owner,handle);writable(e.value,revision);
    require(changes.is_array()&&!changes.empty()&&changes.size()<=8,"ftn_setup_step_invalid");require(e.value.edit_revision<UINT64_MAX,"ftn_draft_revision_limit");
    auto candidate=e.value.configuration;
    for(const auto& change:changes)candidate=web_ftn_record_update(change,candidate,seal_);
    require(!app::encode_ftn_configuration(candidate).empty(),"ftn_catalog_limit");
    e.value.configuration=std::move(candidate);++e.value.edit_revision;return e.value;
}
WebFtnDraft WebFtnDrafts::commit(const std::string& owner,app::FtnTransferToken handle,app::FtnTransferToken operation){
    std::lock_guard<std::mutex> lock(mutex_);auto& e=find(owner,handle);require(e.value.state==WebFtnDraftState::review&&e.value.operation==operation,"ftn_review_unavailable");
    // Set the locked state before any native call, including an exception or
    // lost HTTP response. Only read-only reconciliation follows submission.
    e.value.state=WebFtnDraftState::submitted;e.value.review.reset();e.value.result=web_ftn_commit(exchange_,e.prepared);return e.value;
}
WebFtnDraft WebFtnDrafts::check_result(const std::string& owner,app::FtnTransferToken handle){
    std::lock_guard<std::mutex> lock(mutex_);auto& e=find(owner,handle);require(e.value.state==WebFtnDraftState::submitted,"ftn_save_not_submitted");
    const auto result=web_ftn_reconcile(exchange_,e.value.operation);
    // A temporarily unavailable lookup cannot erase a known commit result.
    if(result.code==Code::ok||!e.value.result||e.value.result->code!=Code::ok)e.value.result=result;
    return e.value;
}
WebFtnDraft WebFtnDrafts::cancel_review(const std::string& owner,app::FtnTransferToken handle){
    std::lock_guard<std::mutex> lock(mutex_);auto& e=find(owner,handle);require(e.value.state==WebFtnDraftState::review,"ftn_review_unavailable");
    web_ftn_cancel(exchange_,e.prepared);e.value.review.reset();e.value.result.reset();e.value.operation={};e.value.state=WebFtnDraftState::editing;return e.value;
}
void WebFtnDrafts::discard(const std::string& owner,app::FtnTransferToken handle){std::lock_guard<std::mutex> lock(mutex_);auto& e=find(owner,handle);if(e.value.state==WebFtnDraftState::review)web_ftn_cancel(exchange_,e.prepared);entries_.erase(handle);}
WebFtnListSnapshot WebFtnDrafts::start_list(const std::string& owner,app::FtnTransferToken handle,std::uint64_t revision,std::string network,std::string source,bool url,bool cp437,std::string selected_source){
    std::lock_guard<std::mutex> lock(mutex_);auto& e=find(owner,handle);writable(e.value,revision);
    const auto net=std::find_if(e.value.configuration.networks.begin(),e.value.configuration.networks.end(),[&](const auto& n){return web_ftn_same_id(n.code,network);});require(net!=e.value.configuration.networks.end(),"ftn_network_removed");
    require(source.size()<=4096&&source.find('\0')==std::string::npos&&(url?app::valid_ftn_echo_list_url(source):!source.empty()&&source.front()=='/'),"ftn_list_source_invalid");
    if(selected_source.empty())selected_source=source;
    require(selected_source.size()<=(url?2048U:1024U)&&selected_source.find('\0')==std::string::npos&&(!url||selected_source==source),"ftn_list_saved_source_invalid");
    if(e.pending_list)e.pending_list->cancel();
    e.pending_list.reset();
    e.pending_list=std::make_shared<WebFtnListJob>(new_token(),net->code,std::move(source),url,cp437,list_reader_,std::move(selected_source));return e.pending_list->snapshot();
}
WebFtnListSnapshot WebFtnDrafts::list_for(Entry& e,app::FtnTransferToken token){
    if(std::all_of(token.begin(),token.end(),[](auto b){return b==0;})){if(e.pending_list)token=e.pending_list->snapshot().token;else if(e.list)token=e.list->snapshot().token;}
    if(e.pending_list){auto value=e.pending_list->snapshot();if(value.token==token){if(value.ready&&!value.active)e.list=e.pending_list;return value;}}
    if(e.list){auto value=e.list->snapshot();if(value.token==token)return value;}
    throw std::runtime_error("ftn_list_unavailable");
}
WebFtnListSnapshot WebFtnDrafts::read_list(const std::string& owner,app::FtnTransferToken handle,app::FtnTransferToken token){std::lock_guard<std::mutex> lock(mutex_);return list_for(find(owner,handle),token);}
void WebFtnDrafts::cancel_list(const std::string& owner,app::FtnTransferToken handle,app::FtnTransferToken token){std::lock_guard<std::mutex> lock(mutex_);auto& e=find(owner,handle);require(e.pending_list&&e.pending_list->snapshot().token==token,"ftn_list_unavailable");e.pending_list->cancel();}
app::FtnEchoImportPlan WebFtnDrafts::import_preview(const std::string& owner,app::FtnTransferToken handle,std::uint64_t revision,app::FtnTransferToken token,const WebFtnImportCatalog& catalog,const std::vector<std::string>& selected,app::FtnEchoImportRequirements requirements,bool update){
    std::lock_guard<std::mutex> lock(mutex_);auto& e=find(owner,handle);writable(e.value,revision);return web_ftn_import_plan(e.value.configuration,list_for(e,token),catalog,selected,requirements,update);
}
WebFtnDraft WebFtnDrafts::import_apply(const std::string& owner,app::FtnTransferToken handle,std::uint64_t revision,app::FtnTransferToken token,const WebFtnImportCatalog& catalog,const std::vector<std::string>& selected,app::FtnEchoImportRequirements requirements,bool update){
    std::lock_guard<std::mutex> lock(mutex_);auto& e=find(owner,handle);writable(e.value,revision);require(e.value.edit_revision<UINT64_MAX,"ftn_draft_revision_limit");
    auto plan=web_ftn_import_plan(e.value.configuration,list_for(e,token),catalog,selected,requirements,update);require(plan.issues.empty()&&plan.selected>0&&!app::encode_ftn_configuration(plan.configuration).empty(),"ftn_import_plan_invalid");
    e.value.configuration=std::move(plan.configuration);++e.value.edit_revision;return e.value;
}
void WebFtnDrafts::clear(){std::lock_guard<std::mutex> lock(mutex_);for(auto& item:entries_)if(item.second.value.state==WebFtnDraftState::review)web_ftn_cancel(exchange_,item.second.prepared);entries_.clear();}
} // namespace sfx::apps::runtime::web
