#include "../../src/apps/runtime/qwk_packet_completion.hpp"
#include "sfx/application/qwk_transfer.hpp"
#include <iostream>
#include <stdexcept>
namespace detail=sfx::apps::runtime::qwk_packet_detail;
unsigned checks{};
void check(bool ok,const char* label){++checks;if(!ok)throw std::runtime_error(label);}
std::string receipt(detail::Direction d,unsigned reason,std::size_t size){
    const bool send=d==detail::Direction::send,complete=reason==0;
    return std::string(send?"SFX-PACKET-SEND 1\n":"SFX-PACKET-RECEIVE 1\n")+"attempted "+(reason==3?"0":"1")+"\ncompleted "+(complete?"1":"0")+"\nbytes "+std::to_string(complete?size:0)+"\nzeof "+(complete?"1":"0")+"\nbatch 1\nfailure 0\nrefusal "+std::to_string(reason)+"\ninput 33\noutput 44\n";
}
int main()try{
    for(const auto d:{detail::Direction::receive,detail::Direction::send})for(unsigned reason=0;reason<=4;++reason){
        if(d==detail::Direction::receive?reason==4:reason!=0&&reason!=4)continue;
        const bool present=d==detail::Direction::send||reason==0;const std::size_t size=present?512:0;
        const auto text=receipt(d,reason,size);const auto marker=d==detail::Direction::receive?(present?'Z':'S'):(reason?'S':'z');
        const auto dsz=std::string(1,marker)+" "+std::to_string(reason?0:size)+" 0 0 0 0 0 "+(d==detail::Direction::receive?"payload.rep\n":"payload.qwk\n");
        const auto parsed=detail::completion(d,text,dsz,present,size);check(parsed&&parsed->consumed==33&&parsed->produced==44&&parsed->refusal==reason,"exact separate direction/policy completion");
        for(std::size_t n=0;n<text.size();++n){auto bad=text;bad[n]='!';check(!detail::completion(d,bad,dsz,present,size),"every receipt character participates in proof");}
        for(std::size_t n=0;n<dsz.size();++n){auto bad=dsz;bad[n]='!';check(!detail::completion(d,text,bad,present,size),"every DSZ character participates in proof");}
        check(!detail::completion(d,text+"extra",dsz,present,size),"receipt suffix rejected");check(!detail::completion(d,text,dsz+"extra",present,size),"DSZ suffix rejected");
        check(!detail::completion(d,text,dsz,!present,size),"file presence mismatch rejected");
        if(d!=detail::Direction::send||reason!=4)check(!detail::completion(d,text,dsz,present,size+1),"exact transferred file size required");
        else check(!detail::completion(d,text,dsz,present,16777217),"skipped send still requires actual prepared file within cap; zero transferred bytes are not its length");
        check(!detail::completion(d==detail::Direction::receive?detail::Direction::send:detail::Direction::receive,text,dsz,present,size),"mixed direction refused");
        for(const auto count:{"0","01","-1","+1","18446744073709551616","44 ","44\nextra"}){auto bad=text;bad.replace(bad.find("output ")+7,std::string::npos,std::string(count)+"\n");check(!detail::completion(d,bad,dsz,present,size),"noncanonical output evidence rejected");}
    }
    for(const auto d:{detail::Direction::receive,detail::Direction::send}){
        const std::string tail=std::string("**\x18" "B0800000000022d\r\n")+(d==detail::Direction::send?"OO":"");
        for(std::size_t fragment=1;fragment<=tail.size()+2;++fragment){detail::OutputLedger ledger;std::string bytes="prior bytes"+tail;for(std::size_t n=0;n<bytes.size();n+=fragment)check(ledger.record(std::string_view(bytes).substr(n,fragment)),"actual partial writes counted");check(ledger.finished(d,bytes.size()),"actual complete final header/OO across ring fragments");check(!ledger.finished(d,bytes.size()+1)&&!ledger.finished(d,bytes.size()-1),"offered rather than actual output never accepted");}
        for(std::size_t n=0;n<tail.size();++n){detail::OutputLedger ledger;auto bad=tail;bad[n]='!';ledger.record(bad);check(!ledger.finished(d,bad.size()),"every final wire byte required");}
        detail::OutputLedger partial;partial.record(tail.substr(0,tail.size()-1));check(!partial.finished(d,tail.size()-1),"truncated finish not success");
    }
    auto stop=std::make_shared<std::atomic<bool>>(false);{
        sfx::application::QwkReceiveResult first(sfx::application::QwkTransferCode::completed,{},nullptr,stop);
        {auto moved=std::move(first);check(!stop->load(),"moved-from result cannot cancel current import");}check(stop->load(),"command result destruction revokes late writer-held source");
    }
    std::cout<<"PASS "<<checks<<" packet completion/token unit checks; NOT native relay/client/storage acceptance\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL completion after "<<checks<<": "<<e.what()<<'\n';return 1;}
