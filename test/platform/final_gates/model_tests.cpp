#include "media_contract.hpp"
#include "../provenance_receipt/model.hpp"
#include <iostream>
#include <stdexcept>
namespace {
unsigned checks = 0;
void check(bool b, const char* text) { ++checks; if (!b) throw std::runtime_error(text); }
void media_tests() {
    using namespace final_gate_test;
    for (unsigned mask = 0; mask < 1024; ++mask) {
        const MediaObservation o{
            !!(mask&1), !!(mask&2), !!(mask&4), !!(mask&8), !!(mask&16),
            !!(mask&32), !!(mask&64), !!(mask&128), !!(mask&256), !!(mask&512)};
        check(may_privileged_use(o) == (mask == 1023), "every independent launch prerequisite required");
    }
    for (unsigned mask = 0; mask < 128; ++mask) {
        const FreshFacts f{ !!(mask&1), !!(mask&2), !!(mask&4), !!(mask&8),
            !!(mask&16), !!(mask&32), !!(mask&64) };
        check(may_first_commit(f) == (mask == 127), "no snapshot-only fresh receipt issuance");
    }
}
void state_tests() {
    using namespace final_gate_test;
    for (auto phase : {Phase::absent,Phase::fresh_intent,Phase::committed,
                       Phase::prepared_update,Phase::prepared_rollback,Phase::retired,Phase::corrupt}) {
        for (auto pair : {Pair::absent,Pair::old_pair,Pair::new_pair,Pair::mixed,Pair::unexpected}) {
            for (unsigned generation = 0; generation != 4; ++generation) {
                for (unsigned mask = 0; mask != 32; ++mask) {
                    DurableObservation o{phase,pair,!!(mask&1),!!(mask&2),!!(mask&4),
                                         !!(mask&8),!!(mask&16),generation};
                    const bool preserved = phase == Phase::absent && pair == Pair::absent && o.source_preserved;
                    const bool trusted = phase == Phase::committed && (mask&23) == 23;
                    const auto expected = preserved ? State::preserved_source :
                        trusted && pair == Pair::old_pair && generation == 1 ? State::intact_prior :
                        trusted && pair == Pair::new_pair && generation == 2 ? State::verified_new :
                        State::incomplete_refusal;
                    check(recover(o) == expected, "all durable states classify without guessed repair/activation");
                }
            }
        }
    }
}
void binding_tests() {
    using namespace receipt_test;
    const auto r = sample();
    const Observation correct{r.binding,r.prior,true,false};
    check(inspect(&r,true,correct)==Result::current, "committed exact old pair");
    check(inspect(nullptr,true,correct)==Result::missing, "missing receipt refuses");
    check(inspect(&r,false,correct)==Result::untrusted_store, "candidate-side receipt refuses");
    for (unsigned field = 0; field != 15; ++field) {
        auto o = correct;
        switch(field) {
        case 0: o.binding.installation_id[0]='B';break;
        case 1: o.binding.volume_guid[0]='1';break;
        case 2: o.binding.canonical_root += "-copy";break;
        case 3: ++o.binding.volume_serial;break;
        case 4: ++o.binding.root_id;break;
        case 5: ++o.binding.bin_id;break;
        case 6: ++o.binding.policy[0];break;
        case 7: ++o.program.number;break;
        case 8: ++o.program.manifest[0];break;
        case 9: ++o.program.file_ids[0];break;
        case 10: ++o.program.file_ids[1];break;
        case 11: ++o.program.bytes[0][0];break;
        case 12: ++o.program.bytes[1][0];break;
        case 13: o.exact_security=false;break;
        case 14: o.extra_files=true;break;
        }
        check(!may_update(inspect(&r,true,o)), "every object/policy/program correlation required");
    }
    for (unsigned i = 0; i != 4; ++i) {
        auto p = r;p.phase=Phase::prepared;
        auto o = correct;
        if (i&1) {o.program.file_ids[0]=r.next.file_ids[0];o.program.bytes[0]=r.next.bytes[0];}
        if (i&2) {o.program.file_ids[1]=r.next.file_ids[1];o.program.bytes[1]=r.next.bytes[1];}
        check(!may_update(inspect(&p,true,o)), "prepared old/new/mixed never ordinary update authority");
    }
}
}
int main() {
    try {
        media_tests(); state_tests(); binding_tests();
        std::cout << "FINAL-GATE MODEL PASS checks=" << checks << " failed=0 skipped=0\n";
        std::cout << "Policy enumeration only; no native receipt issuance, loader or process-crash proof.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "MODEL FAIL checks=" << checks << ' ' << e.what() << '\n'; return 1;
    }
}
