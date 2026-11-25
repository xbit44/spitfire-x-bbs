// Bounded TEST-ONLY receipt codec. Not a production format or authority source.
// Decoding untrusted bytes grants no authority; a native caller must separately
// prove the selected HKLM store and match actual protected objects and media.
#pragma once
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
namespace final_receipt_wire {
constexpr std::size_t maximum_bytes = 4096;
enum class Phase { fresh_intent, committed, prepared_update, prepared_rollback, retired };
struct Pair {
    std::uint64_t generation = 0;
    std::string manifest;
    std::array<std::uint64_t,2> ids{};
    std::array<std::string,2> hashes;
    bool operator==(const Pair& p) const {
        return std::tie(generation,manifest,ids,hashes)==std::tie(p.generation,p.manifest,p.ids,p.hashes);
    }
};
struct Record {
    Phase phase = Phase::fresh_intent;
    std::string installation_id, volume_guid, canonical_root, operator_sid, policy_hash;
    std::uint32_t volume_serial = 0;
    std::uint64_t root_id = 0, bin_id = 0;
    Pair prior, next;
    bool operator==(const Record& r) const {
        return std::tie(phase,installation_id,volume_guid,canonical_root,operator_sid,policy_hash,
            volume_serial,root_id,bin_id,prior,next)==std::tie(r.phase,r.installation_id,r.volume_guid,
            r.canonical_root,r.operator_sid,r.policy_hash,r.volume_serial,r.root_id,r.bin_id,r.prior,r.next);
    }
};
inline bool upper_hex(std::string_view s, std::size_t n) {
    return s.size()==n && std::all_of(s.begin(),s.end(),[](char c){return (c>='0'&&c<='9')||(c>='A'&&c<='F');});
}
inline bool number(std::string_view s, std::uint64_t& out) {
    if(s.empty() || s.size()>20 || (s.size()>1 && s[0]=='0')) return false;
    if(!std::all_of(s.begin(),s.end(),[](char c){return c>='0'&&c<='9';})) return false;
    std::uint64_t n=0;const auto result=std::from_chars(s.data(),s.data()+s.size(),n);
    if(result.ec!=std::errc{} || result.ptr!=s.data()+s.size()) return false;
    out=n;return true;
}
inline bool guid(std::string_view s) {
    if(s.size()!=36) return false;
    for(std::size_t i=0;i<s.size();++i) {
        if(i==8 || i==13 || i==18 || i==23) {if(s[i]!='-')return false;}
        else if(!upper_hex(s.substr(i,1),1)) return false;
    }
    return true;
}
inline bool sid(std::string_view s) {
    // Fixture's designated interactive Windows principal, never a display name.
    if(s.size()>184 || s.substr(0,9)!="S-1-5-21-") return false;
    s.remove_prefix(9);
    for(unsigned i=0;i<4;++i) {
        const auto end=s.find('-');
        const auto part=s.substr(0,end);
        std::uint64_t n=0;
        if(!number(part,n) || n>std::numeric_limits<std::uint32_t>::max()) return false;
        if(i==3) return end==std::string_view::npos && n!=0;
        if(end==std::string_view::npos) return false;
        s.remove_prefix(end+1);
    }
    return false;
}
inline bool pair_valid(const Pair& p) {
    return p.generation && upper_hex(p.manifest,64) && p.ids[0] && p.ids[1] && p.ids[0]!=p.ids[1] &&
        upper_hex(p.hashes[0],64) && upper_hex(p.hashes[1],64);
}
inline bool pair_empty(const Pair& p) { return p==Pair{}; }
inline const char* phase_name(Phase p) {
    switch(p) {
    case Phase::fresh_intent:return "fresh-intent";
    case Phase::committed:return "committed";
    case Phase::prepared_update:return "prepared-update";
    case Phase::prepared_rollback:return "prepared-rollback";
    case Phase::retired:return "retired";
    }
    return nullptr;
}
inline bool valid(const Record& r) {
    if(!phase_name(r.phase) || !upper_hex(r.installation_id,32) || !guid(r.volume_guid) ||
        r.canonical_root!="C:\\xtest-sfx-final-"+r.installation_id || !sid(r.operator_sid) ||
        !upper_hex(r.policy_hash,64) || !r.volume_serial || !r.root_id || !r.bin_id ||
        r.root_id==r.bin_id || !pair_valid(r.prior)) return false;
    if(r.phase==Phase::prepared_update || r.phase==Phase::prepared_rollback) {
        if(!pair_valid(r.next)) return false;
        if(r.phase==Phase::prepared_update && !(r.next.generation>r.prior.generation)) return false;
        if(r.phase==Phase::prepared_rollback && !(r.next.generation<r.prior.generation)) return false;
        for(auto id:r.prior.ids) for(auto other:r.next.ids) if(id==other) return false;
    } else if(!pair_empty(r.next)) return false;
    for(auto id:r.prior.ids) if(id==r.root_id || id==r.bin_id) return false;
    for(auto id:r.next.ids) if(id && (id==r.root_id || id==r.bin_id)) return false;
    return true;
}
inline std::string encode(const Record& r) {
    if(!valid(r)) return {};
    std::string s="SFX_FINAL_RECEIPT_TEST_V1\n";
    const auto add=[&](const char* key,const std::string& value){s+=key;s+='=';s+=value;s+='\n';};
    add("phase",phase_name(r.phase));add("id",r.installation_id);add("volume",r.volume_guid);
    add("root",r.canonical_root);add("operator",r.operator_sid);add("policy",r.policy_hash);
    add("serial",std::to_string(r.volume_serial));add("root-id",std::to_string(r.root_id));add("bin-id",std::to_string(r.bin_id));
    const auto pair=[&](const char* label,const Pair& p){
        add("pair",label);add("generation",std::to_string(p.generation));add("manifest",p.manifest);
        add("program","SFXControlPanel.exe");add("file-id",std::to_string(p.ids[0]));add("sha256",p.hashes[0]);
        add("program","SFXRuntime.exe");add("file-id",std::to_string(p.ids[1]));add("sha256",p.hashes[1]);
    };
    pair("prior",r.prior);pair("next",r.next);
    return s.size()<=maximum_bytes?s:std::string{};
}
class Reader {
    std::string_view data_;
    bool ok_=true;
public:
    explicit Reader(std::string_view data):data_(data){}
    std::string_view line() {
        const auto end=data_.find('\n');
        if(!ok_ || end==std::string_view::npos){ok_=false;return {};}
        const auto s=data_.substr(0,end);data_.remove_prefix(end+1);return s;
    }
    std::string_view field(std::string_view key) {
        const auto s=line();
        if(s.size()<=key.size() || s.substr(0,key.size())!=key || s[key.size()]!='='){ok_=false;return {};}
        return s.substr(key.size()+1);
    }
    bool exact(std::string_view key,std::string_view value){if(field(key)!=value)ok_=false;return ok_;}
    std::uint64_t numeric(std::string_view key){std::uint64_t n=0;if(!number(field(key),n))ok_=false;return n;}
    bool complete()const{return ok_&&data_.empty();}
};
inline bool decode(std::string_view bytes,Record& output) {
    if(bytes.empty() || bytes.size()>maximum_bytes) return false;
    for(unsigned char c:bytes) if(c!='\n' && (c<32 || c>126)) return false;
    Reader in(bytes);if(in.line()!="SFX_FINAL_RECEIPT_TEST_V1")return false;
    Record r;const auto phase=in.field("phase");
    if(phase=="fresh-intent")r.phase=Phase::fresh_intent;
    else if(phase=="committed")r.phase=Phase::committed;
    else if(phase=="prepared-update")r.phase=Phase::prepared_update;
    else if(phase=="prepared-rollback")r.phase=Phase::prepared_rollback;
    else if(phase=="retired")r.phase=Phase::retired;
    else return false;
    r.installation_id=in.field("id");r.volume_guid=in.field("volume");r.canonical_root=in.field("root");
    r.operator_sid=in.field("operator");r.policy_hash=in.field("policy");
    const auto serial=in.numeric("serial");if(serial>std::numeric_limits<std::uint32_t>::max())return false;
    r.volume_serial=static_cast<std::uint32_t>(serial);r.root_id=in.numeric("root-id");r.bin_id=in.numeric("bin-id");
    const auto pair=[&](const char* label,Pair& p){
        in.exact("pair",label);p.generation=in.numeric("generation");p.manifest=in.field("manifest");
        in.exact("program","SFXControlPanel.exe");p.ids[0]=in.numeric("file-id");p.hashes[0]=in.field("sha256");
        in.exact("program","SFXRuntime.exe");p.ids[1]=in.numeric("file-id");p.hashes[1]=in.field("sha256");
    };
    pair("prior",r.prior);pair("next",r.next);
    if(!in.complete() || !valid(r) || encode(r)!=bytes) return false;
    output=std::move(r);return true;
}
inline bool committed_matches(const Record& receipt,const Record& actual,bool trusted_store,bool exact_security,bool complete_inventory) {
    // No native authority is inferred by calling this model comparison.
    return trusted_store && exact_security && complete_inventory && receipt.phase==Phase::committed &&
        valid(receipt) && valid(actual) && receipt==actual;
}
inline Record sample() {
    Record r;r.phase=Phase::committed;r.installation_id=std::string(32,'A');
    r.volume_guid="01234567-89AB-CDEF-0123-456789ABCDEF";
    r.canonical_root="C:\\xtest-sfx-final-"+r.installation_id;
    r.operator_sid="S-1-5-21-111-222-333-1001";r.policy_hash=std::string(64,'B');
    r.volume_serial=7;r.root_id=100;r.bin_id=200;
    r.prior={1,std::string(64,'C'),{300,400},{std::string(64,'D'),std::string(64,'E')}};
    return r;
}
}
