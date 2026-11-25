#include "linux_web_ftn_transfer.hpp"
#include <iostream>
#include <map>

namespace {
namespace app=sfx::application;
namespace web=sfx::apps::runtime::web;
using Code=app::FtnConfigurationCode;
using Op=app::FtnConfigurationOperation;
unsigned checks{};
void check(bool value,const char* name){++checks;if(!value)throw std::runtime_error(name);}
app::FtnTransferToken token(unsigned value){app::FtnTransferToken result{};result[0]=static_cast<std::uint8_t>(value);return result;}
app::FtnConfigurationResponse result(Code code,std::int64_t revision=0){app::FtnConfigurationResponse r;r.code=code;r.revision=revision;return r;}
struct Fixture {
    app::FtnConfiguration saved;std::uint64_t now{1000};unsigned changes{},previews{},reads{};
    std::map<app::FtnTransferToken,std::int64_t> receipts;
    app::FtnConfigurationPublisher publisher;
    Fixture():publisher([&]()->std::optional<app::FtnConfiguration>{++reads;return saved;},
        [&](const auto& id,const auto& candidate){if(candidate.revision!=saved.revision)return result(Code::conflict);saved=candidate;++saved.revision;++changes;receipts[id]=saved.revision;return result(Code::ok,saved.revision);},
        [&](const auto& id){const auto found=receipts.find(id);return found==receipts.end()?result(Code::conflict):result(Code::ok,found->second);},
        [&](const auto& id,const auto& candidate){++previews;app::FtnRetentionPreview review;review.token=id;review.configuration_revision=candidate.revision;review.reviewed_utc=1700000000;review.draft[0]=1;review.changed=review.shortened=2;review.networks.push_back({"TEST","Review network",1,1,1,1700000000,1700000100});auto r=result(Code::ok,candidate.revision);r.bytes=app::encode_ftn_retention_preview(review);r.total=static_cast<std::uint32_t>(r.bytes.size());return r;}){
        saved.settings.mode=app::FtnMailerMode::disabled;saved.settings.listener_enabled=false;
        app::FtnNetwork network;network.code="TEST";network.name="Review network";network.address="2:100/2";network.msg_netmail_enabled=false;saved.networks.push_back(network);
        for(unsigned i=0;i<500;++i){app::FtnMessageEcho echo;echo.network="TEST";echo.tag="TEST."+std::to_string(i);echo.description=std::string(90,'a');echo.store_locally=false;saved.message_echoes.push_back(echo);}
    }
    app::FtnConfigurationResponse exchange(const app::FtnConfigurationRequest& request){return publisher.handle(request,now);}
};
}
int main(){try{
    Fixture fixture;const web::FtnExchange exchange=[&](const auto& request){return fixture.exchange(request);};
    check(app::validate_ftn_configuration(fixture.saved).empty(),"valid large FTN fixture");
    check(app::encode_ftn_configuration(fixture.saved).size()>app::kFtnConfigurationPageBytes,"configuration crosses multiple protocol pages");
    auto read=web::web_ftn_read(exchange,token(1));check(read.code==Code::ok&&read.configuration&&read.configuration->message_echoes.size()==500,"paged current snapshot assembled");
    check(web::web_ftn_read(exchange,token(2)).code==Code::ok,"completed read released its pinned reader");
    check(web::web_ftn_read(exchange,{}).code==Code::invalid,"zero token refused before transfer");
    auto candidate=*read.configuration;candidate.networks[0].name="Staged network";
    auto prepared=web::web_ftn_prepare(exchange,token(3),candidate);check(prepared.code==Code::ok&&prepared.review&&prepared.review->shortened==2,"exact retention preview returned before commit");
    check(fixture.changes==0&&fixture.saved.networks[0].name=="Review network","prepare cannot publish configuration");
    check(prepared.review->networks[0].from_forever==1&&prepared.review->networks[0].due_now==1,"review retains existing-original details");
    auto second=web::web_ftn_prepare(exchange,token(4),candidate);check(second.code==Code::busy,"second writer reports busy");
    check(web::web_ftn_commit(exchange,prepared).code==Code::ok&&fixture.saved.networks[0].name=="Staged network","busy refusal did not cancel another writer");
    check(fixture.changes==1&&web::web_ftn_commit(exchange,prepared).code==Code::invalid&&fixture.changes==1,"prepared save consumed exactly once");
    check(web::web_ftn_reconcile(exchange,token(3)).revision==2&&fixture.changes==1,"receipt check never republishes");
    check(web::web_ftn_prepare(exchange,token(5),candidate).code==Code::conflict,"stale configuration refused");
    candidate=fixture.saved;candidate.networks[0].name="Cancelled network";prepared=web::web_ftn_prepare(exchange,token(6),candidate);check(prepared.code==Code::ok,"new draft prepared");web::web_ftn_cancel(exchange,prepared);
    check(fixture.saved.networks[0].name=="Staged network"&&fixture.changes==1&&!prepared.review,"cancel retains saved settings and clears review");
    check(web::web_ftn_commit(exchange,prepared).code==Code::invalid,"cancelled review cannot commit");
    const web::FtnExchange lost_commit=[&](const auto& request){auto response=exchange(request);if(request.operation==Op::commit)throw std::runtime_error("reply lost after publication");return response;};
    prepared=web::web_ftn_prepare(lost_commit,token(7),candidate);check(prepared.code==Code::ok,"unknown-outcome case prepared");
    check(web::web_ftn_commit(lost_commit,prepared).code==Code::unavailable&&fixture.changes==2,"lost commit reply remains unknown");
    check(web::web_ftn_commit(exchange,prepared).code==Code::invalid&&fixture.changes==2,"unknown commit is not repeated");
    check(web::web_ftn_reconcile(exchange,token(7)).code==Code::ok&&fixture.changes==2,"exact receipt resolves lost reply without another write");
    check(web::web_ftn_reconcile(exchange,token(8)).code==Code::conflict,"missing receipt does not imply success");
    candidate=fixture.saved;candidate.networks[0].name="Expired draft";prepared=web::web_ftn_prepare(exchange,token(9),candidate);fixture.now+=60001;
    check(web::web_ftn_commit(exchange,prepared).code==Code::conflict&&fixture.changes==2,"expired retained preview cannot silently restart save");
    const web::FtnExchange lost_append=[&](const auto& request){auto response=exchange(request);if(request.operation==Op::append)throw std::runtime_error("append interrupted");return response;};
    check(web::web_ftn_prepare(lost_append,token(10),candidate).code==Code::unavailable&&fixture.changes==2,"append interruption never publishes");
    prepared=web::web_ftn_prepare(exchange,token(11),candidate);check(prepared.code==Code::ok,"append interruption cancelled its exact writer");web::web_ftn_cancel(exchange,prepared);
    const web::FtnExchange wrong_review=[&](const auto& request){auto response=exchange(request);if(request.operation==Op::preview_retention){auto review=app::decode_ftn_retention_preview(response.bytes);check(review.has_value(),"underlying preview valid");review->token=token(99);response.bytes=app::encode_ftn_retention_preview(*review);}return response;};
    check(web::web_ftn_prepare(wrong_review,token(12),candidate).code==Code::unavailable,"different operation preview rejected");
    prepared=web::web_ftn_prepare(exchange,token(13),candidate);check(prepared.code==Code::ok,"wrong preview cleaned only its writer");web::web_ftn_cancel(exchange,prepared);
    for(unsigned fault=0;fault<5;++fault){
        const web::FtnExchange malformed=[&](const auto& request){auto response=exchange(request);if(request.operation==Op::read){if(fault==0)response.total=1;else if(fault==1)response.offset=1;else if(fault==2)response.bytes.clear();else if(fault==3)response.total=app::kFtnConfigurationMaximumBytes+1;else response.revision=0;}return response;};
        check(web::web_ftn_read(malformed,token(20+fault)).code==Code::unavailable,"malformed page rejected");
        check(web::web_ftn_read(exchange,token(30+fault)).code==Code::ok,"malformed read releases owned reader");
    }
    const web::FtnExchange changed_revision=[&](const auto& request){auto response=exchange(request);if(request.operation==Op::read&&request.offset)++response.revision;return response;};
    check(web::web_ftn_read(changed_revision,token(40)).code==Code::unavailable,"mixed-revision pages refused");
    const auto before=fixture.previews;auto invalid=candidate;invalid.networks[0].name.clear();check(web::web_ftn_prepare(exchange,token(41),invalid).code==Code::invalid&&fixture.previews==before,"invalid candidate rejected before preview");
    const web::FtnExchange unavailable=[](const auto&)->app::FtnConfigurationResponse{throw std::runtime_error("host unavailable");};
    check(web::web_ftn_reconcile(unavailable,token(7)).code==Code::unavailable,"missing reconciliation response stays unresolved");
    check(web::web_ftn_reconcile(exchange,{}).code==Code::invalid,"invalid reconciliation token refused");
    check(fixture.changes==2,"all rejected cancelled and expired cases preserve published configuration");
    std::cout<<"PASS "<<checks<<" FTN web transfer checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<" "<<error.what()<<'\n';return 1;}}
