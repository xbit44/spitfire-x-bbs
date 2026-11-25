#include "../../src/apps/runtime/file_transfer_completion.hpp"
#include <iostream>
#include <stdexcept>
namespace f=sfx::apps::runtime::file_transfer_detail;
namespace {unsigned checks{};void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
std::string summary(f::Direction direction,unsigned count,unsigned attempted,unsigned refused,std::string slots){return std::string("SFX-FILE-TRANSFER 1\ndirection ")+(direction==f::Direction::receive?"R":"S")+"\ncount "+std::to_string(count)+"\nattempted "+std::to_string(attempted)+"\nrefused "+std::to_string(refused)+"\nbatch 1\nfailure 0\ninput 400\noutput 500\n"+slots;}
void changed(const std::string& source,const char* before,const char* after,f::Direction d,const std::vector<f::ExpectedFile>& e){auto text=source;const auto at=text.find(before);check(at!=std::string::npos,"test mutation token exists");text.replace(at,std::char_traits<char>::length(before),after);check(!f::completion(d,text,e),"mutated completion rejected");}}
int main(){try{
    const auto receive=f::Direction::receive,send=f::Direction::send;
    const auto good=summary(receive,3,2,0,"file 0 1 1 0 0 1\nfile 1 1 1 0 17 1\nfile 2 0 0 0 0 0\n");
    const std::vector<f::ExpectedFile> expected{{64,0},{64,17},{64,0}};
    const auto accepted=f::completion(receive,good,expected);check(accepted&&accepted->consumed==400&&accepted->produced==500&&accepted->files.size()==3&&accepted->files[0].completed&&accepted->files[1].bytes==17&&!accepted->files[2].offered,"actual zero/nonzero/absent shape accepted");
    check(!f::completion(send,good,expected),"direction cannot be exchanged");
    const auto refused=summary(receive,2,4,3,"file 0 1 1 0 17 1\nfile 1 1 0 2 0 0\n");
    check(f::completion(receive,refused,{{64,17},{64,0}}).has_value(),"known oversize plus unlisted/duplicate attempts accounted");
    check(!f::completion(receive,refused,{{64,17},{64,1}}),"skipped slot cannot contain received bytes");
    const auto empty=summary(receive,1,0,0,"file 0 0 0 0 0 0\n");check(f::completion(receive,empty,{{64,0}}).has_value(),"completed empty batch not completed file");
    const auto sent=summary(send,3,3,0,"file 0 1 1 0 0 1\nfile 1 1 0 1 0 0\nfile 2 1 1 0 64 1\n");
    const std::vector<f::ExpectedFile> sources{{64,0},{64,17},{64,64}};
    auto sent_proof=f::completion(send,sent,sources);check(sent_proof&&sent_proof->files[0].completed&&sent_proof->files[1].skipped&&!sent_proof->files[1].completed&&sent_proof->files[2].bytes==64,"successful sender, zero and skip distinguished");
    for(const auto& text:{good,good+'\n',good+"file 3 0 0 0 0 0\n"})if(text!=good)check(!f::completion(receive,text,expected),"trailing fields rejected");
    for(std::size_t i=0;i<good.size();++i)check(!f::completion(receive,std::string_view(good).substr(0,i),expected),"every truncated receipt rejected");
    for(const auto& pair:{std::pair{"batch 1","batch 0"},{"failure 0","failure 4"},{"input 400","input 0"},{"output 500","output 0"},{"count 3","count 2"},{"attempted 2","attempted 257"},{"refused 0","refused 1"},{"input 400","input 0400"},{"input 400","input +400"},{"input 400","input 18446744073709551616"},{"file 0 1 1 0 0 1","file 0 1 1 0 1 1"},{"file 1 1 1 0 17 1","file 1 1 1 0 16 1"},{"file 1 1 1 0 17 1","file 1 0 1 0 17 1"},{"file 1 1 1 0 17 1","file 1 1 1 0 17 0"},{"file 1 1 1 0 17 1","file 1 1 1 2 17 1"},{"file 2 0 0 0 0 0","file 1 0 0 0 0 0"},{"file 2 0 0 0 0 0","file 2 1 0 0 0 0"}})changed(good,pair.first,pair.second,receive,expected);
    changed(sent,"attempted 3","attempted 2",send,sources);changed(sent,"refused 0","refused 1",send,sources);changed(sent,"file 1 1 0 1 0 0","file 1 1 0 2 0 0",send,sources);changed(sent,"file 0 1 1 0 0 1","file 0 1 0 0 0 0",send,sources);
    check(!f::completion(receive,good,{}),"empty expected manifest refused");check(!f::completion(receive,empty,{{0,0}}),"zero configured limit refused");check(!f::completion(receive,empty,{{1073741825,0}}),"unsupported cap refused");
    std::string all;std::vector<f::ExpectedFile> large;for(unsigned i=0;i<256;++i){all+="file "+std::to_string(i)+" 1 1 0 1073741824 1\n";large.push_back({1073741824,1073741824});}
    check(f::completion(receive,summary(receive,256,256,0,all),large).has_value(),"maximum bounded manifest with inclusive per-file ceiling");
    large.push_back({1,0});check(!f::completion(receive,summary(receive,257,256,0,all),large),"over-count manifest refused");
    using P=sfx::application::TransferProtocolPreference;
    for(const auto protocol:{P::xmodem,P::ymodem,P::ymodem_g}){
        auto receipt=summary(receive,1,1,0,"file 0 1 1 0 17 1\n");
        receipt.replace(0,std::string("SFX-FILE-TRANSFER 1\n").size(),std::string("SFX-FILE-TRANSFER 2\nprotocol ")+f::protocol_code(protocol)+'\n');
        check(f::completion(receive,receipt,{{64,17}},protocol).has_value(),"explicit X/Y/G completion accepted");
        check(!f::completion(receive,receipt,{{64,17}}),"X/Y completion never accepted as ZMODEM");
        for(auto other:{P::xmodem,P::ymodem,P::ymodem_g})if(other!=protocol)check(!f::completion(receive,receipt,{{64,17}},other),"protocol substitution refused");
        for(std::size_t n=0;n<receipt.size();++n)check(!f::completion(receive,std::string_view(receipt).substr(0,n),{{64,17}},protocol),"truncated X/Y completion refused");
        f::OutputLedger ack;check(ack.record(protocol==P::ymodem_g?"dataG":"data\x06")&&ack.finished(receive,protocol,5),"actual final protocol marker and byte count required");
        check(!ack.finished(receive,protocol,4),"produced count mismatch refused");check(ack.record("?")&&!ack.finished(receive,protocol,6),"unexpected output tail refused");
        f::OutputLedger output;
        std::string final=protocol==P::xmodem?std::string(1,'\x04'):std::string("\x01\0\xff",3)+std::string(130,'\0');
        check(output.record(final)&&output.finished(send,protocol,final.size()),"actual X EOT or full Y empty header required");
        final.back()='!';f::OutputLedger bad;check(bad.record(final)&&!bad.finished(send,protocol,final.size()),"corrupt final protocol marker refused");
    }
    check(!f::completion(receive,good,expected,P::select_at_transfer),"selector is never a native protocol");
    std::cout<<"PASS file transfer completion "<<checks<<" checks; receipt parsing only, not actual transport/accounting\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL file completion after "<<checks<<": "<<e.what()<<'\n';return 1;}}
