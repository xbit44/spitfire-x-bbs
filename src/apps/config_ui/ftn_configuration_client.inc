// Uses the same protected owner channel as the other configuration editors.
#ifdef SFX_ALPHA_TEST_AUTOMATION
std::optional<app::FtnConfiguration> (*ftn_query_fixture)()=nullptr;
#endif
app::FtnTransferToken ftn_token(){win32::WindowsSecureIdentifierGenerator ids;return ids.generate().bytes;}
std::optional<app::FtnConfigurationResponse> ftn_exchange(State& s,const app::FtnConfigurationRequest& q){
    const auto bytes=app::encode_ftn_configuration_request(q);if(bytes.empty())return {};
    app::LocalControlFrame response;
    if(!online_exchange(s,request(s,app::LocalControlMessageType::ftn_configuration_request,bytes),app::LocalControlMessageType::ftn_configuration_response,response))return {};
    return app::decode_ftn_configuration_response(response.payload);
}
std::optional<app::FtnConfiguration> query_ftn(State& s){
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if(ftn_query_fixture)return ftn_query_fixture();
#endif
    if(s.navigation_rendering){if(!load_editor_state(s))return {};app::LocalControlFrame q,response;q.type=app::LocalControlMessageType::ftn_configuration_request;q.request_id=s.request_id++;if(!online_exchange(s,q,app::LocalControlMessageType::ftn_configuration_response,response))return {};return app::decode_ftn_configuration(response.payload);}
    if(s.system_recovery_only||!load_editor_state(s))return {};
    if(s.mode==State::ConnectionMode::offline){const auto result=win32::offline_ftn_configuration(s.root,s.owner);return result&&result.value?result.value:std::nullopt;}
    if(s.mode!=State::ConnectionMode::online)return {};
    app::FtnConfigurationRequest q;q.token=ftn_token();std::vector<std::uint8_t> bytes;std::int64_t revision{};std::uint32_t total{};
    const auto cancel=[&]{auto stop=q;stop.operation=app::FtnConfigurationOperation::cancel;stop.offset=stop.total=0;stop.bytes.clear();ftn_exchange(s,stop);};
    for(;;){
        const auto page=ftn_exchange(s,q);
        if(!page||page->code!=app::FtnConfigurationCode::ok||page->revision<1||!page->total||page->offset!=bytes.size()||page->bytes.empty()||
           (revision&&(page->revision!=revision||page->total!=total))){cancel();return {};}
        if(!revision){revision=page->revision;total=page->total;bytes.reserve(total);}
        bytes.insert(bytes.end(),page->bytes.begin(),page->bytes.end());
        if(bytes.size()==total)break;
        q.offset=static_cast<std::uint32_t>(bytes.size());q.revision=revision;
    }
    cancel();const auto c=app::decode_ftn_configuration(bytes);return c&&c->revision==revision?c:std::nullopt;
}
#include "ftn_retention_review.inc"
struct FtnEditorSave {app::FtnConfigurationCode code{app::FtnConfigurationCode::unavailable};std::optional<app::FtnConfiguration> configuration;std::int64_t applied_revision{};bool cancelled{};};
#ifdef SFX_ALPHA_TEST_AUTOMATION
FtnEditorSave (*ftn_change_fixture)(const app::FtnConfiguration&)=nullptr;
FtnEditorSave (*ftn_reconcile_fixture)(State&)=nullptr;
#endif
FtnEditorSave apply_ftn(State& s,const app::FtnConfiguration& c){
    FtnEditorSave result;
    if(s.ftn.pending)return result;
    // The authoritative online/offline host checks changed credentials against
    // its current saved catalog. Preserved unreadable values must remain editable.
    if(!app::validate_ftn_configuration(c).empty()){result.code=app::FtnConfigurationCode::invalid;return result;}
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if(ftn_change_fixture)return ftn_change_fixture(c);
#endif
    if(s.system_recovery_only||!load_editor_state(s))return result;
    const auto token=ftn_token();
    if(s.mode==State::ConnectionMode::offline){
        const auto review=win32::offline_preview_ftn_retention(s.root,s.owner,token,c);
        if(!review||!review.value){result.code=(review.status.code==app::StorageErrorCode::backpressure||review.status.code==app::StorageErrorCode::busy)?app::FtnConfigurationCode::busy:review.status.code==app::StorageErrorCode::conflict?app::FtnConfigurationCode::conflict:app::FtnConfigurationCode::unavailable;return result;}
        if(!ftn_review_retention(s,*review.value)){result.cancelled=true;return result;}
        s.ftn.pending=FtnEditorState::Pending{token,c};
        const auto saved=win32::offline_change_ftn_configuration(s.root,s.owner,token,c);
        if(saved&&saved.value){s.ftn.pending.reset();return {app::FtnConfigurationCode::ok,saved.value->configuration,saved.value->applied_revision};}
        result.code=saved.status.code==app::StorageErrorCode::backpressure?app::FtnConfigurationCode::busy:saved.status.code==app::StorageErrorCode::conflict?app::FtnConfigurationCode::conflict:app::FtnConfigurationCode::unavailable;if(result.code!=app::FtnConfigurationCode::unavailable)s.ftn.pending.reset();return result;
    }
    if(s.mode!=State::ConnectionMode::online)return result;
    const auto bytes=app::encode_ftn_configuration(c);if(bytes.empty()){result.code=app::FtnConfigurationCode::invalid;return result;}
    app::FtnConfigurationRequest q;q.token=token;q.operation=app::FtnConfigurationOperation::begin;q.revision=c.revision;q.total=static_cast<std::uint32_t>(bytes.size());
    const auto cancel=[&]{auto stop=q;stop.operation=app::FtnConfigurationOperation::cancel;stop.offset=stop.total=0;stop.bytes.clear();ftn_exchange(s,stop);};
    auto response=ftn_exchange(s,q);
    if(!response||response->code!=app::FtnConfigurationCode::ok){if(response)result.code=response->code;return result;}
    q.operation=app::FtnConfigurationOperation::append;
    for(std::size_t offset=0;offset<bytes.size();offset+=app::kFtnConfigurationPageBytes){
        const auto end=std::min(bytes.size(),offset+app::kFtnConfigurationPageBytes);q.offset=static_cast<std::uint32_t>(offset);q.bytes={bytes.begin()+offset,bytes.begin()+end};
        response=ftn_exchange(s,q);if(!response||response->code!=app::FtnConfigurationCode::ok||response->offset!=end){if(response)result.code=response->code;cancel();return result;}
    }
    q.operation=app::FtnConfigurationOperation::preview_retention;q.offset=0;q.bytes.clear();response=ftn_exchange(s,q);
    if(!response||response->code!=app::FtnConfigurationCode::ok){if(response)result.code=response->code;cancel();return result;}
    const auto review=app::decode_ftn_retention_preview(response->bytes);
    if(!review||review->token!=token||review->configuration_revision!=c.revision){cancel();return result;}
    if(!ftn_review_retention(s,*review)){cancel();result.cancelled=true;return result;}
    q.operation=app::FtnConfigurationOperation::commit;s.ftn.pending=FtnEditorState::Pending{token,c};response=ftn_exchange(s,q);
    if(!response){q.operation=app::FtnConfigurationOperation::reconcile;q.total=0;response=ftn_exchange(s,q);}
    if(!response||response->code!=app::FtnConfigurationCode::ok){if(response){result.code=response->code;if(response->code!=app::FtnConfigurationCode::unavailable)s.ftn.pending.reset();}cancel();return result;}
    s.ftn.pending.reset();result.code=app::FtnConfigurationCode::ok;result.applied_revision=response->revision;result.configuration=query_ftn(s);return result;
}
FtnEditorSave reconcile_ftn(State& s){
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if(ftn_reconcile_fixture)return ftn_reconcile_fixture(s);
#endif
    if(!s.ftn.pending)return {};const auto pending=*s.ftn.pending;if(!load_editor_state(s))return {};
    if(s.mode==State::ConnectionMode::offline){const auto saved=win32::offline_change_ftn_configuration(s.root,s.owner,pending.token,pending.configuration);if(saved&&saved.value){s.ftn.pending.reset();return {app::FtnConfigurationCode::ok,saved.value->configuration,saved.value->applied_revision};}if(saved.status.code==app::StorageErrorCode::conflict||saved.status.code==app::StorageErrorCode::backpressure)s.ftn.pending.reset();return {};}
    app::FtnConfigurationRequest q;q.operation=app::FtnConfigurationOperation::reconcile;q.token=pending.token;const auto r=ftn_exchange(s,q);if(!r)return {};if(r->code==app::FtnConfigurationCode::ok){s.ftn.pending.reset();return {r->code,query_ftn(s),r->revision};}if(r->code==app::FtnConfigurationCode::conflict)s.ftn.pending.reset();return {r->code,{},0};
}
