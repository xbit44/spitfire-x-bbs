#include "linux_web_ftn_transfer.hpp"
#include <algorithm>

namespace sfx::apps::runtime::web {
namespace {
namespace app=application;
using Code=app::FtnConfigurationCode;
using Operation=app::FtnConfigurationOperation;
void cancel(const FtnExchange& exchange,app::FtnTransferToken token) noexcept{
    try{app::FtnConfigurationRequest request;request.operation=Operation::cancel;request.token=token;(void)exchange(request);}catch(...){ }
}
struct CancelTransfer {
    const FtnExchange& exchange;app::FtnTransferToken token;bool active{true};
    ~CancelTransfer(){if(active)cancel(exchange,token);}
};
app::FtnConfigurationResponse failure(Code code){app::FtnConfigurationResponse out;out.code=code;return out;}
}
WebFtnSnapshot web_ftn_read(const FtnExchange& exchange,application::FtnTransferToken token){
    app::FtnConfigurationRequest request;request.token=token;
    if(app::encode_ftn_configuration_request(request).empty())return {Code::invalid,{}};
    CancelTransfer release{exchange,token};std::vector<std::uint8_t> bytes;std::uint32_t total{};
    try{
        for(;;){
            const auto response=exchange(request);if(response.code!=Code::ok)return {response.code,{}};
            if(response.revision<1||response.revision==INT64_MAX||!response.total||response.total>app::kFtnConfigurationMaximumBytes||
                response.offset!=bytes.size()||response.offset>response.total||response.bytes.empty()||response.bytes.size()>app::kFtnConfigurationPageBytes||
                response.bytes.size()>response.total-response.offset||(total&&(total!=response.total||request.revision!=response.revision)))return {Code::unavailable,{}};
            if(!total){total=response.total;request.revision=response.revision;bytes.reserve(total);}
            bytes.insert(bytes.end(),response.bytes.begin(),response.bytes.end());
            if(bytes.size()==total)break;
            request.offset=static_cast<std::uint32_t>(bytes.size());
        }
        auto configuration=app::decode_ftn_configuration(bytes);
        if(!configuration||configuration->revision!=request.revision)return {Code::unavailable,{}};
        return {Code::ok,std::move(configuration)};
    }catch(const std::exception&){return {};}
}
WebFtnPrepared web_ftn_prepare(const FtnExchange& exchange,application::FtnTransferToken token,const application::FtnConfiguration& configuration){
    WebFtnPrepared out;
    try{
        if(!app::validate_ftn_configuration(configuration).empty()){out.code=Code::invalid;return out;}
        const auto bytes=app::encode_ftn_configuration(configuration);if(bytes.empty()){out.code=Code::invalid;return out;}
        app::FtnConfigurationRequest request;request.operation=Operation::begin;request.token=token;request.revision=configuration.revision;request.total=static_cast<std::uint32_t>(bytes.size());
        if(app::encode_ftn_configuration_request(request).empty()){out.code=Code::invalid;return out;}
        // A refusal cannot cancel another editor. Once our token was submitted,
        // cancellation is safe even if the native reply itself was interrupted.
        CancelTransfer release{exchange,token};auto response=exchange(request);out.code=response.code;
        if(response.code!=Code::ok)return out;
        request.operation=Operation::append;
        for(std::size_t offset=0;offset<bytes.size();offset+=app::kFtnConfigurationPageBytes){
            const auto end=std::min(bytes.size(),offset+app::kFtnConfigurationPageBytes);request.offset=static_cast<std::uint32_t>(offset);request.bytes={bytes.begin()+offset,bytes.begin()+end};
            response=exchange(request);out.code=response.code;if(response.code!=Code::ok)return out;
            if(response.revision!=configuration.revision||response.total!=bytes.size()||response.offset!=end){out.code=Code::unavailable;return out;}
        }
        request.operation=Operation::preview_retention;request.offset=0;request.bytes.clear();response=exchange(request);out.code=response.code;
        if(response.code!=Code::ok)return out;
        out.review=app::decode_ftn_retention_preview(response.bytes);
        if(!out.review||out.review->token!=token||out.review->configuration_revision!=configuration.revision){out.review.reset();out.code=Code::unavailable;return out;}
        request.operation=Operation::commit;out.commit=std::move(request);release.active=false;return out;
    }catch(const std::exception&){return {};}
}
application::FtnConfigurationResponse web_ftn_commit(const FtnExchange& exchange,WebFtnPrepared& prepared){
    if(prepared.code!=Code::ok||!prepared.review||prepared.commit.operation!=Operation::commit||prepared.commit.token!=prepared.review->token||
        prepared.commit.revision!=prepared.review->configuration_revision||app::encode_ftn_configuration_request(prepared.commit).empty())return failure(Code::invalid);
    const auto request=prepared.commit;prepared.review.reset();prepared.code=Code::unavailable;
    // A lost commit reply is not permission to issue another commit. Keep the
    // token for a separate read-only receipt lookup; never rebuild/resubmit.
    try{const auto response=exchange(request);cancel(exchange,request.token);prepared.code=response.code;return response;}
    catch(const std::exception&){cancel(exchange,request.token);return failure(Code::unavailable);}
}
void web_ftn_cancel(const FtnExchange& exchange,WebFtnPrepared& prepared) noexcept{
    cancel(exchange,prepared.commit.token);prepared.review.reset();prepared.code=Code::conflict;
}
application::FtnConfigurationResponse web_ftn_reconcile(const FtnExchange& exchange,application::FtnTransferToken token){
    app::FtnConfigurationRequest request;request.operation=Operation::reconcile;request.token=token;
    if(app::encode_ftn_configuration_request(request).empty())return failure(Code::invalid);
    try{return exchange(request);}catch(const std::exception&){return failure(Code::unavailable);}
}
} // namespace sfx::apps::runtime::web
