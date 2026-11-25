#include "sfx/application/local_control_protocol.hpp"
#include "sfx/application/local_qwk_administration.hpp"
#include <iostream>
#include <stdexcept>
namespace app=sfx::application;
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
int main()try{
    app::QwkPolicyChange change;change.operation.bytes[0]=1;change.expected_revision=9;change.board_id="x-bit";change.maximum_messages=10000;
    const auto wire=app::encode_qwk_policy_change(change);const auto decoded=app::decode_qwk_policy_change(wire);
    check(!wire.empty()&&decoded&&decoded->operation==change.operation&&decoded->expected_revision==9&&decoded->board_id=="x-bit"&&decoded->maximum_messages==10000,"typed policy-only change roundtrip, canonicalized by storage not guessed by wire");
    for(std::size_t n=0;n<wire.size();++n)check(!app::decode_qwk_policy_change({wire.begin(),wire.begin()+n}),"every truncated change refused");
    auto suffix=wire;suffix.push_back(0);check(!app::decode_qwk_policy_change(suffix),"change suffix refused");
    for(const auto name:{"","CON","LPT1","1BOARD","LONGBOARD","BAD/ID","BAD ID","A:ID","NETMAIL"}){auto bad=change;bad.board_id=name;check(!app::valid_qwk_policy_change(bad)&&app::encode_qwk_policy_change(bad).empty(),"unsafe/reserved QWK ID refused before IPC");}
    for(const auto count:{0U,65536U,UINT32_MAX}){auto bad=change;bad.maximum_messages=count;check(!app::valid_qwk_policy_change(bad)&&app::encode_qwk_policy_change(bad).empty(),"no count overflow/zero/Unlimited representation");}
    for(const auto wrap:{std::optional<bool>{},std::optional<bool>{false},std::optional<bool>{true}})for(const auto replay:{false,true}){
        app::LocalQwkPolicyResult result{app::LocalAccountResultCode::ok,{"X-BIT",65535,4,wrap,replay}};const auto bytes=app::encode_qwk_policy_result(result);const auto read=app::decode_qwk_policy_result(bytes);
        check(read&&read->policy.board_id=="X-BIT"&&read->policy.maximum_messages==65535&&read->policy.revision==4&&read->policy.word_wrap_default==wrap&&read->policy.replayed==replay,"exact board/count/revision/reserved default/reconcile result");
        for(std::size_t n=0;n<bytes.size();++n)check(!app::decode_qwk_policy_result({bytes.begin(),bytes.begin()+n}),"every truncated success result refused");
        auto bad=bytes;bad.back()=2;check(!app::decode_qwk_policy_result(bad),"nonboolean replay refused");bad=bytes;bad[bad.size()-2]=3;check(!app::decode_qwk_policy_result(bad),"unknown owner-default state refused");bad=bytes;bad.push_back(0);check(!app::decode_qwk_policy_result(bad),"result suffix refused");
    }
    for(const auto code:{app::LocalAccountResultCode::unavailable,app::LocalAccountResultCode::conflict,app::LocalAccountResultCode::unauthorized,app::LocalAccountResultCode::storage_failure}){const auto bytes=app::encode_qwk_policy_result({code,{}});const auto read=app::decode_qwk_policy_result(bytes);check(bytes.size()==1&&read&&read->code==code,"failure cannot imply stale successful policy");}
    check(app::kAlphaLocalControlVersion.major==50&&app::kAlphaLocalControlVersion.minor==0,"current matched IPC50.0 carries the existing private QWK Config frames");
    std::cout<<"PASS "<<checks<<" QWK policy protocol units; not Config GUI/pipe/storage authority acceptance\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL policy protocol after "<<checks<<": "<<e.what()<<'\n';return 1;}
