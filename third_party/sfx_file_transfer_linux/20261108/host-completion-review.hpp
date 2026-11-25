// SFX-authored independent completion-accounting proof, October9 2026.
// GPL-2.0-or-later for this separate helper's reproducible qualification harness.
// Not compiled or linked into shipping SFX. No protocol/parser bypass.
#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
namespace completion_review {
struct Ledger {
    std::uint64_t forwarded{};
    std::array<unsigned char,2> last{};std::size_t retained{};
    bool append(std::string_view bytes)noexcept{
        if(bytes.size()>(std::numeric_limits<std::uint64_t>::max)()-forwarded)return false;
        for(unsigned char byte:bytes){
            if(retained==2){last[0]=last[1];last[1]=byte;}
            else last[retained++]=byte;
        }
        forwarded+=bytes.size();return true;
    }
    bool safe_enter_tail(std::uint64_t consumed,std::string_view pending)const noexcept{
        if(!consumed || consumed>forwarded)return false;
        const auto sent_tail=forwarded-consumed;
        if(sent_tail>2 || sent_tail>retained || pending.size()>2-sent_tail)return false;
        std::array<unsigned char,2> tail{};std::size_t size{};
        for(std::size_t n=retained-static_cast<std::size_t>(sent_tail);n<retained;++n)tail[size++]=last[n];
        for(unsigned char byte:pending)tail[size++]=byte;
        return size==0 || size==1 && (tail[0]=='\r' || tail[0]=='\n') ||
            size==2 && tail[0]=='\r' && tail[1]=='\n';
    }
};
inline std::optional<std::uint64_t> receipt_input(std::string_view receipt,std::size_t bytes,unsigned refusal=0){
    if(refusal>3 || (refusal?bytes!=0:!bytes || bytes>262144))return {};
    const auto prefix=std::string("SFX-MESSAGE-RECEIVE 4\nattempted ")+(refusal==3?"0":"1")+"\ncompleted "+(refusal?"0":"1")+"\nbytes "+std::to_string(bytes)+
        "\nzeof "+(refusal?"0":"1")+"\nbatch 1\nfailure 0\nrefusal "+std::to_string(refusal)+"\ninput ";
    if(receipt.size()<=prefix.size()+1 || receipt.substr(0,prefix.size())!=prefix || receipt.back()!='\n')return {};
    const auto number=receipt.substr(prefix.size(),receipt.size()-prefix.size()-1);
    if(number.empty() || number.size()>20 || number.front()=='0')return {};
    std::uint64_t value{};
    for(unsigned char c:number){if(c<'0' || c>'9' || value>((std::numeric_limits<std::uint64_t>::max)()-(c-'0'))/10)return {};value=value*10+(c-'0');}
    return value;
}
inline bool accept(bool normal_exit,bool owned_verified_files,bool dsz_matches,
    bool current_authority,std::size_t bytes,std::string_view receipt,const Ledger& ledger,std::string_view pending,unsigned refusal=0){
    if(!normal_exit || !owned_verified_files || !dsz_matches || !current_authority)return false;
    const auto count=receipt_input(receipt,bytes,refusal);
    return count && ledger.safe_enter_tail(*count,pending);
}
} // namespace completion_review
