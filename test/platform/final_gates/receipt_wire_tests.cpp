#include "receipt_wire.hpp"
#include <iostream>
#include <stdexcept>
namespace {
unsigned checks=0;
void check(bool b,const char* what){++checks;if(!b)throw std::runtime_error(what);}
std::string replace(std::string text,const std::string& from,const std::string& to){
    const auto at=text.find(from);if(at==std::string::npos)throw std::runtime_error("test fixture token missing");
    text.replace(at,from.size(),to);return text;
}
void refused(const std::string& bytes){
    auto out=final_receipt_wire::sample();out.prior.generation=99;const auto sentinel=out;
    check(!final_receipt_wire::decode(bytes,out),"malformed receipt rejected");
    check(out==sentinel,"failed parse never publishes partial output");
}
void run(){
    using namespace final_receipt_wire;
    const auto r=sample();const auto good=encode(r);
    check(!good.empty()&&good.size()<maximum_bytes,"bounded canonical test receipt");
    Record decoded;check(decode(good,decoded)&&decoded==r,"exact committed round trip");
    for(auto phase:{Phase::fresh_intent,Phase::committed,Phase::retired}){
        auto p=r;p.phase=phase;Record out;
        check(decode(encode(p),out)&&out==p,"non-transition phase round trip");
        check(committed_matches(p,p,true,true,true)==(phase==Phase::committed),"intent/retired cannot authorize use");
    }
    for(auto phase:{Phase::prepared_update,Phase::prepared_rollback}){
        auto p=r;p.phase=phase;p.prior.generation=phase==Phase::prepared_update?1:2;
        p.next={phase==Phase::prepared_update?2U:1U,std::string(64,'F'),{500,600},{std::string(64,'1'),std::string(64,'2')}};
        Record out;check(decode(encode(p),out)&&out==p,"prepared transition carries both full pairs");
        check(!committed_matches(p,p,true,true,true),"prepared pair is not activation authority");
        auto wrong=p;wrong.next.generation=wrong.prior.generation;
        check(encode(wrong).empty(),"same-generation transition rejected");
        wrong=p;wrong.next.ids[0]=wrong.prior.ids[1];
        check(encode(wrong).empty(),"cross-pair identity reuse rejected");
    }
    for(std::size_t n=0;n<good.size();++n)refused(good.substr(0,n));
    refused(good+"\n");refused(good+"unknown=value\n");refused(good+good);
    refused(std::string(maximum_bytes,'X'));refused(std::string(maximum_bytes+1,'X'));
    refused(replace(good,"_V1\n","_V2\n"));refused(replace(good,"phase=committed","phase=unknown"));
    refused(replace(good,"serial=7","serial=07"));refused(replace(good,"serial=7","serial=-7"));
    refused(replace(good,"serial=7","serial=+7"));refused(replace(good,"serial=7","serial=7 "));
    refused(replace(good,"serial=7","serial=4294967296"));refused(replace(good,"root-id=100","root-id=18446744073709551616"));
    refused(replace(good,"root-id=100","root-id=0"));refused(replace(good,"bin-id=200","bin-id=100"));
    refused(replace(good,"file-id=300","file-id=400"));refused(replace(good,"file-id=300","file-id=100"));
    refused(replace(good,"program=SFXRuntime.exe","program=SFXControlPanel.exe"));
    refused(replace(good,"program=SFXRuntime.exe","program=unexpected.exe"));
    refused(replace(good,"program=SFXRuntime.exe","program=../SFXRuntime.exe"));
    refused(replace(good,"root="+r.canonical_root,"root=C:\\SFX"));
    refused(replace(good,"root="+r.canonical_root,"root="+r.canonical_root+"\\..\\outside"));
    refused(replace(good,"root="+r.canonical_root,"root=\\\\server\\share"));
    refused(replace(good,"operator="+r.operator_sid,"operator=S-1-5-18"));
    refused(replace(good,"operator="+r.operator_sid,"operator=S-1-5-21-01-2-3-1001"));
    refused(replace(good,"policy="+r.policy_hash,"policy="+std::string(64,'G')));
    for(std::size_t at=0;at<good.size();++at){
        for(char invalid:{'\0','\r','\t',static_cast<char>(0xff)}){
            auto changed=good;changed[at]=invalid;refused(changed);
        }
        auto changed=good;changed[at]=changed[at]=='0'?'1':'0';Record out;
        const bool parsed=decode(changed,out);
        check(!parsed||!committed_matches(out,r,true,true,true),"single-byte mutation never authenticates original object set");
    }
    for(unsigned mask=0;mask!=8;++mask)
        check(committed_matches(r,r,!!(mask&1),!!(mask&2),!!(mask&4))==(mask==7),"store/security/inventory independently required");
    auto maximum=r;maximum.volume_serial=std::numeric_limits<std::uint32_t>::max();
    maximum.root_id=std::numeric_limits<std::uint64_t>::max();maximum.bin_id=maximum.root_id-1;
    maximum.prior.generation=maximum.root_id;maximum.prior.ids={maximum.root_id-2,maximum.root_id-3};
    maximum.operator_sid="S-1-5-21-4294967295-4294967295-4294967295-4294967295";
    check(decode(encode(maximum),decoded)&&decoded==maximum,"maximum numeric widths round trip without wrap");
    std::cout<<"Canonical sample bytes="<<good.size()<<"; maximum numeric sample bytes="<<encode(maximum).size()<<"; hard cap="<<maximum_bytes<<'\n';
}
}
int main(){try{run();std::cout<<"RECEIPT CODEC PASS checks="<<checks<<" failed=0 skipped=0; test-only codec, no native store authority\n";return 0;}
catch(const std::exception& e){std::cerr<<"RECEIPT CODEC FAIL checks="<<checks<<' '<<e.what()<<'\n';return 1;}}
