// Test-only receipt policy model. Not a production wire format or updater.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <tuple>
namespace receipt_test {
using Hash = std::array<std::uint8_t,32>;
struct Binding {
    std::string installation_id, volume_guid, canonical_root;
    std::uint32_t volume_serial=0;
    std::uint64_t root_id=0, bin_id=0;
    Hash policy{};
    bool operator==(const Binding& b) const {
        return std::tie(installation_id,volume_guid,canonical_root,volume_serial,root_id,bin_id,policy)==
            std::tie(b.installation_id,b.volume_guid,b.canonical_root,b.volume_serial,b.root_id,b.bin_id,b.policy);
    }
};
struct Generation {
    std::uint64_t number=0;
    Hash manifest{};
    std::array<std::uint64_t,2> file_ids{};
    std::array<Hash,2> bytes{};
    bool operator==(const Generation& g) const {
        return std::tie(number,manifest,file_ids,bytes)==std::tie(g.number,g.manifest,g.file_ids,g.bytes);
    }
};
enum class Phase { committed, prepared, retired };
struct Receipt { unsigned schema=1; Binding binding; Generation prior,next; Phase phase=Phase::committed; };
enum class Result { missing, untrusted_store, corrupt, binding_mismatch, security_mismatch,
    generation_mismatch, prepared_prior, prepared_next, prepared_mixed, retired, current };
struct Observation { Binding binding; Generation program; bool exact_security=false; bool extra_files=false; };
inline bool bounded(const Binding& b) {
    return b.installation_id.size()==32 && b.volume_guid.size()==36 && !b.canonical_root.empty() &&
        b.canonical_root.size()<=260 && b.volume_serial && b.root_id && b.bin_id;
}
inline bool valid(const Generation& g) { return g.number && g.file_ids[0] && g.file_ids[1] && g.file_ids[0]!=g.file_ids[1]; }
inline Result inspect(const Receipt* r,bool protected_authority,const Observation& o) {
    if(!r) return Result::missing;
    if(!protected_authority) return Result::untrusted_store;
    if(r->schema!=1||!bounded(r->binding)||!valid(r->prior)||
        (r->phase!=Phase::committed&&r->phase!=Phase::prepared&&r->phase!=Phase::retired)||
        (r->phase==Phase::prepared&&(!valid(r->next)||r->next.number==r->prior.number))) return Result::corrupt;
    if(!(r->binding==o.binding)) return Result::binding_mismatch;
    if(!o.exact_security||o.extra_files) return Result::security_mismatch;
    if(r->phase==Phase::retired) return Result::retired;
    if(r->phase==Phase::prepared) {
        if(o.program==r->prior) return Result::prepared_prior;
        if(o.program==r->next) return Result::prepared_next;
        return Result::prepared_mixed;
    }
    return o.program==r->prior?Result::current:Result::generation_mismatch;
}
inline bool may_update(Result r) { return r==Result::current; }
// Conceptual inside-Setup preconditions only, never an externally supplied token.
inline bool may_first_publish(bool trusted_media,bool freshly_created,bool final_program_checks,bool import_success) {
    return trusted_media&&freshly_created&&final_program_checks&&import_success;
}
inline Receipt sample() {
    Receipt r;
    r.binding={std::string(32,'A'),"00000000-0000-0000-0000-000000000001","C:\\SFX-DUMMY",7,10,20,{}};
    r.binding.policy.fill(1);r.prior.number=1;r.prior.manifest.fill(2);r.prior.file_ids={30,40};
    r.prior.bytes[0].fill(3);r.prior.bytes[1].fill(4);
    r.next=r.prior;r.next.number=2;r.next.manifest.fill(5);r.next.file_ids={50,60};
    r.next.bytes[0].fill(6);r.next.bytes[1].fill(7);return r;
}
}
