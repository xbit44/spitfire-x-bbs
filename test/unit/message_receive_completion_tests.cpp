#include "../../src/apps/runtime/message_receive_completion.hpp"
#include "../../src/apps/runtime/message_receive_deadline.hpp"
#include "../../src/apps/runtime/message_receive_policy.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace sfx::apps::runtime::message_receive_detail;
unsigned checks=0;
void check(bool yes,const char* label){++checks;if(!yes)throw std::runtime_error(label);}
std::string receipt(std::string count){return "SFX-MESSAGE-RECEIVE 2\nattempted 1\ncompleted 1\nbytes 4\nzeof 1\nbatch 1\nfailure 0\ninput "+count+"\n";}
int main()try{
    for (unsigned reason=0;reason<=3;++reason) {
        const bool has_payload=reason==0;
        const auto bytes=has_payload?4U:0U;
        const auto policy=std::string("SFX-MESSAGE-RECEIVE 4\nattempted ")+(reason==3?"0":"1")+"\ncompleted "+
            (has_payload?"1":"0")+"\nbytes "+std::to_string(bytes)+"\nzeof "+
            (has_payload?"1":"0")+"\nbatch 1\nfailure 0\nrefusal "+std::to_string(reason)+"\ninput 11\n";
        const auto dsz=std::string(has_payload?"Z ":"S ")+std::to_string(bytes)+" 0 0 0 0 0 payload.bin\n";
        const auto parsed=policy_completion(policy,dsz,has_payload,bytes);
        check(parsed && parsed->consumed==11 && static_cast<unsigned>(parsed->refusal)==reason,"exact profile4 outcome including no-file cancellation is not a posted file");
        check(!completed_input(policy,bytes),"old production profile2 rejects profile4");
        auto old_profile=policy;old_profile[20]='3';check(!policy_completion(old_profile,dsz,has_payload,bytes),"mixed profile3 receipt cannot authorize profile4 handoff");
        for(std::size_t pos=0;pos<policy.size();++pos){auto bad=policy;bad[pos]='!';check(!policy_completion(bad,dsz,has_payload,bytes),"every policy receipt byte checked");}
        for(std::size_t pos=0;pos<dsz.size();++pos){auto bad=dsz;bad[pos]='!';check(!policy_completion(policy,bad,has_payload,bytes),"every DSZ byte checked");}
        check(!policy_completion(policy,dsz,!has_payload,bytes),"file-presence mismatch not downgraded to refusal");
        check(!policy_completion(policy,dsz,has_payload,bytes+1),"exact payload length required");
        check(!policy_completion(policy+"extra",dsz,has_payload,bytes),"receipt suffix rejected");
        for(auto count:{"0","01","-1","+1","18446744073709551616","11 extra","11\nextra","11 "}) {
            const auto bad=policy.substr(0,policy.find("input ")+6)+count+"\n";
            check(!policy_completion(bad,dsz,has_payload,bytes),"noncanonical overflow and suffixed policy input counts refused");
        }
        for(const auto& tail:{std::string{},std::string("\r"),std::string("\n"),std::string("\r\n"),std::string("\r\r"),std::string("\n\r"),std::string("Q"),std::string("\0",1),std::string("\x18"),std::string("\x1b[A")}) {
            for(std::size_t split=0;split<=tail.size();++split){
                ForwardedInput ledger;check(ledger.record("protocol-OO") && ledger.record(std::string_view(tail).substr(0,split)),"policy counts actual successfully forwarded prefixes");
                check(ledger.completion_tail(parsed->consumed,std::string_view(tail).substr(split))==(tail.empty() || tail=="\r" || tail=="\n" || tail=="\r\n"),"policy receipt does not waive one-Enter accounting");
            }
        }
    }
    check(!policy_completion(receipt("11"),"Z 4 0 0 0 0 0 payload.bin\n",true,4),"candidate parser cannot accept old helper receipt");
    for(const auto ceiling:{std::uint64_t(0),std::uint64_t(101000),std::uint64_t(301000),std::uint64_t(601000)}){
        std::uint64_t sticky=UINT64_MAX;
        for(std::uint64_t elapsed=0;elapsed<=300000;elapsed+=5000){
            const auto limits=receive_deadlines(ceiling,1000,1000+elapsed);
            const auto expected=ceiling?std::min(ceiling,std::uint64_t(301000)):301000;
            sticky=std::min(sticky,limits.hard);
            check(sticky==expected,"sliding idle never latches as hard deadline");
            check(limits.transport==std::min(expected,61000+elapsed),"transport enforces current idle and every earlier hard ceiling");
        }
    }
    check(receive_deadlines(101000,1000,90000).hard==101000,"earlier caller/drain limit not extended by traffic");
    const std::string protocol="known wire boundary OO";
    for(const auto& tail:{std::string{},std::string("\r"),std::string("\n"),std::string("\r\n"),std::string("\r\r"),std::string("\n\r"),std::string("\n\n"),std::string("\r\n\r"),std::string("x"),std::string("\0",1),std::string("\x18"),std::string("\x1b[A"),std::string(65536,'\r')}){
        const bool allowed=tail.empty() || tail=="\r" || tail=="\n" || tail=="\r\n";
        for(std::size_t split=0;split<=std::min<std::size_t>(tail.size(),4);++split){
            for(std::size_t fragment=1;fragment<=protocol.size();++fragment){
                ForwardedInput ledger;
                for(std::size_t pos=0;pos<protocol.size();pos+=fragment)check(ledger.record(std::string_view(protocol).substr(pos,fragment)),"successful send prefix counted");
                check(ledger.record(std::string_view(tail).substr(0,split)),"successful trailing prefix counted");
                check(ledger.sent()==protocol.size()+split,"offered/pending bytes not counted as sent");
                check(ledger.completion_tail(protocol.size(),std::string_view(tail).substr(split))==allowed,"every successful-send/pending split enforces one logical Enter only");
                check(!ledger.completion_tail(0,{}) && !ledger.completion_tail(ledger.sent()+1,{}),"missing or forged completion count cannot exceed sends");
            }
        }
    }
    check(!advanced_count(UINT64_MAX,1) && advanced_count(UINT64_MAX,0)==UINT64_MAX && advanced_count(UINT64_MAX-1,1)==UINT64_MAX,"production counter overflow checked before update");
    const auto good=receipt(std::to_string(protocol.size()));
    check(completed_input(good,4)==protocol.size(),"exact profile2 receipt parsed");
    for(std::size_t pos=0;pos<good.size();++pos){auto bad=good;bad[pos]='!';check(!completed_input(bad,4),"every receipt-field corruption rejected");}
    for(auto value:{"0","01","-1","+1","18446744073709551616","1 extra","1\nextra","1 "})check(!completed_input(receipt(value),4),"noncanonical/overflow/suffixed count refused");
    check(!completed_input(good,0) && !completed_input(good,262145) && !completed_input(good,5),"payload size and exact bytes mandatory");
    check(completed_input(receipt("18446744073709551615"),4)==UINT64_MAX,"largest canonical counter parses but still must match actual sends");
    for(std::size_t split=0;split<=ForwardedOutput::final_header.size();++split){
        ForwardedOutput out;out.record("previous handshake bytes");out.record(ForwardedOutput::final_header.substr(0,split));
        check(split==20 || !out.finished(),"partial final protocol output cannot claim completion");
        out.record(ForwardedOutput::final_header.substr(split));check(out.finished(),"complete hex ZFIN acknowledged only after caller writes");
        out.record("x");check(!out.finished(),"unexpected later output cannot stand for flushed ZFIN");
    }
    for(std::size_t n=0;n<20;++n){auto bad=std::string(ForwardedOutput::final_header);bad[n]='!';ForwardedOutput out;out.record(bad);check(!out.finished(),"every final-output byte mandatory");}
    std::cout<<"PASS "<<checks<<" production completion accounting checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}
