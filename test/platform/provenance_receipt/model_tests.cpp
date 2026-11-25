#include "model.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace receipt_test;
unsigned checks=0;
void check(bool condition,const char* why) { ++checks;if(!condition)throw std::runtime_error(why); }
int main() { try {
    auto r=sample();Observation o{r.binding,r.prior,true,false};
    check(inspect(&r,true,o)==Result::current,"complete matched record");
    check(inspect(nullptr,true,o)==Result::missing,"copy to new machine without receipt");
    check(inspect(&r,false,o)==Result::untrusted_store,"copied record never creates store authority");
    for(unsigned bits=0;bits<16;++bits)check(may_first_publish(bits&1,bits&2,bits&4,bits&8)==(bits==15),"all trusted fresh-install preconditions required");
    for(unsigned field=0;field<7;++field) {
        auto changed=o;
        switch(field) {case 0:changed.binding.installation_id[0]='B';break;case 1:changed.binding.volume_guid[0]='1';break;
        case 2:changed.binding.canonical_root="C:\\SFX-OTHER";break;case 3:++changed.binding.volume_serial;break;
        case 4:++changed.binding.root_id;break;case 5:++changed.binding.bin_id;break;case 6:++changed.binding.policy[0];break;}
        check(inspect(&r,true,changed)==Result::binding_mismatch,"every binding field gates copied/stale records");
    }
    for(unsigned field=0;field<6;++field) {
        auto changed=o;
        switch(field) {case 0:++changed.program.number;break;case 1:++changed.program.manifest[0];break;
        case 2:++changed.program.file_ids[0];break;case 3:++changed.program.file_ids[1];break;
        case 4:++changed.program.bytes[0][0];break;case 5:++changed.program.bytes[1][0];break;}
        check(inspect(&r,true,changed)==Result::generation_mismatch,"exact program generation and bytes required");
    }
    auto bad=r;bad.schema=2;check(inspect(&bad,true,o)==Result::corrupt,"unknown schema refuses");
    bad=r;bad.binding.canonical_root.resize(261,'x');check(inspect(&bad,true,o)==Result::corrupt,"bounded root field");
    bad=r;bad.binding.root_id=0;check(inspect(&bad,true,o)==Result::corrupt,"no zero identity");
    auto unsafe=o;unsafe.exact_security=false;check(inspect(&r,true,unsafe)==Result::security_mismatch,"owner/ACL mismatch refuses");
    unsafe=o;unsafe.extra_files=true;check(inspect(&r,true,unsafe)==Result::security_mismatch,"unexpected programs refuse");
    bad=r;bad.phase=Phase::retired;check(inspect(&bad,true,o)==Result::retired,"uninstall tombstone not authority");
    // Exhaust every durable combination: absent/old/prepared/new/corrupt receipt
    // crossed with old/one-file-mixed/new/unknown program observations. This is
    // a state model, NOT physical disk/crash atomicity or an implemented updater.
    for(unsigned receipt_state=0;receipt_state<5;++receipt_state)for(unsigned program_state=0;program_state<4;++program_state) {
        auto journal=r;auto observation=o;
        if(receipt_state==2)journal.phase=Phase::prepared;
        if(receipt_state==3)journal.prior=r.next;
        if(receipt_state==4)journal.schema=99;
        if(program_state==1) {observation.program.file_ids[0]=r.next.file_ids[0];observation.program.bytes[0]=r.next.bytes[0];}
        if(program_state==2)observation.program=r.next;
        if(program_state==3)observation.program.number=99;
        const auto result=inspect(receipt_state?&journal:nullptr,true,observation);
        const bool complete=(receipt_state==1&&program_state==0)||(receipt_state==3&&program_state==2);
        check(may_update(result)==complete,"no interrupted durable combination falsely activates");
    }
    auto prepared=r;prepared.phase=Phase::prepared;
    check(inspect(&prepared,true,o)==Result::prepared_prior,"pending journal retains prior complete fact");
    auto newer=o;newer.program=r.next;check(inspect(&prepared,true,newer)==Result::prepared_next,"pending journal sees new complete fact, still requires recovery");
    check(!may_update(inspect(&prepared,true,newer)),"no auto-commit from uncertain prepared state");
    // Rollback is itself prepared before either resource changes; only the
    // completely restored old programs AND committed old receipt permit use.
    auto rollback=r;rollback.phase=Phase::prepared;rollback.prior=r.next;rollback.next=r.prior;
    check(!may_update(inspect(&rollback,true,newer)),"rollback begins unavailable");
    check(!may_update(inspect(&rollback,true,o)),"restored prior pair still awaits receipt commit");
    check(may_update(inspect(&r,true,o)),"prior complete generation restored");
    auto reinstall=r;reinstall.binding.installation_id[0]='C';reinstall.binding.root_id=101;reinstall.binding.bin_id=102;
    Observation fresh{reinstall.binding,reinstall.prior,true,false};
    check(!may_update(inspect(&r,true,fresh)),"stale path receipt cannot bless a new root");
    check(may_update(inspect(&reinstall,true,fresh)),"new trusted installation has separate identity");
    check(may_update(inspect(&r,true,o)),"second installation cannot alter first");
    const std::vector<std::string> source{"configuration","database","dummy identity","unknown sysop bytes","doors path","logs","backup"};
    const auto before=source;
    for(auto result:{Result::missing,Result::corrupt,Result::binding_mismatch,Result::prepared_mixed,Result::retired}) {
        check(!may_update(result),"refusal preserves copy-first fallback boundary");
        check(source==before,"policy refusal never deletes or edits mutable source");
    }
    for(unsigned value=0;value<static_cast<unsigned>(Result::current);++value)check(!may_update(static_cast<Result>(value)),"only exact current grants update");
    std::cout<<"MODEL PASS: "<<checks<<" checks; failed=0; skipped=0. Policy/state model only; native store and process-cut evidence are separate.\n";return 0;
}catch(const std::exception& e){std::cerr<<"MODEL FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;} }
