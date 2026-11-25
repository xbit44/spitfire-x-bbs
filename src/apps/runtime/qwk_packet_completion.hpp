#pragma once
// Independently authored SFX host receipt/relay proof, not GPL helper code.
// A parse result alone never proves owned-file identity, current authority,
// actual transport writes, cancellation or durable import/delivery progress.
#include "message_receive_completion.hpp"
#include <array>
#include <charconv>
#include <optional>
#include <string>

namespace sfx::apps::runtime::qwk_packet_detail {
enum class Direction {receive,send};
struct Completion {std::uint64_t consumed{},produced{};unsigned refusal{};};
inline std::optional<Completion> completion(Direction direction,std::string_view receipt,
    std::string_view dsz,bool payload_present,std::size_t payload_bytes){
    if(payload_present?(!payload_bytes||payload_bytes>16777216):payload_bytes!=0)return {};
    const auto number=[&](std::string_view& source,std::string_view name)->std::optional<std::uint64_t>{
        if(source.substr(0,name.size())!=name)return {};
        source.remove_prefix(name.size());const auto end=source.find('\n');if(end==std::string_view::npos)return {};
        const auto text=source.substr(0,end);if(text.empty()||text.size()>20||(text.size()>1&&text.front()=='0'))return {};
        std::uint64_t value{};const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size())return {};
        source.remove_prefix(end+1);return value;
    };
    const std::string_view header=direction==Direction::receive?"SFX-PACKET-RECEIVE 1\n":"SFX-PACKET-SEND 1\n";
    if(receipt.substr(0,header.size())!=header)return {};
    receipt.remove_prefix(header.size());
    std::array<std::uint64_t,9> fields{};std::size_t at{};
    for(const auto key:{"attempted ","completed ","bytes ","zeof ","batch ","failure ","refusal ","input ","output "}){const auto value=number(receipt,key);if(!value)return {};fields[at++]=*value;}
    if(!receipt.empty()||fields[4]!=1||fields[5]||!fields[7]||!fields[8])return {};
    const auto refused=fields[6];
    if(direction==Direction::receive){
        if(refused>3||((refused==0)!=payload_present)||fields[0]!=(refused==3?0U:1U)||fields[1]!=(payload_present?1U:0U)||fields[2]!=payload_bytes||fields[3]!=(payload_present?1U:0U))return {};
    }else{
        // Sender payload always exists even when the receiver explicitly skips.
        if(!payload_present||(refused!=0&&refused!=4)||fields[0]!=1||fields[1]!=(refused?0U:1U)||fields[2]!=(refused?0U:payload_bytes)||fields[3]!=(refused?0U:1U))return {};
    }
    const auto marker=direction==Direction::receive?(payload_present?'Z':'S'):(refused?'S':'z');
    const auto expected=std::string(1,marker)+' '+std::to_string(fields[2])+" 0 0 0 0 0 "+(direction==Direction::receive?"payload.rep\n":"payload.qwk\n");
    if(dsz!=expected)return {};
    return Completion{fields[7],fields[8],static_cast<unsigned>(refused)};
}
class OutputLedger final {
public:
    bool record(std::string_view bytes)noexcept{
        const auto next=message_receive_detail::advanced_count(sent_,bytes.size());if(!next)return false;sent_=*next;
        for(char c:bytes){tail_[at_]=c;at_=(at_+1)%tail_.size();if(size_<tail_.size())++size_;}return true;
    }
    bool finished(Direction direction,std::uint64_t produced)const noexcept{
        constexpr std::string_view receiver="**\x18" "B0800000000022d\r\n";
        constexpr std::string_view sender="**\x18" "B0800000000022d\r\nOO";
        const auto expected=direction==Direction::receive?receiver:sender;
        if(sent_!=produced||size_<expected.size())return false;
        const auto first=(at_+tail_.size()-expected.size())%tail_.size();
        for(std::size_t n=0;n<expected.size();++n)if(tail_[(first+n)%tail_.size()]!=expected[n])return false;
        return true;
    }
    std::uint64_t sent()const noexcept{return sent_;}
private:
    std::array<char,22> tail_{};std::size_t at_{},size_{};std::uint64_t sent_{};
};
} // namespace sfx::apps::runtime::qwk_packet_detail
