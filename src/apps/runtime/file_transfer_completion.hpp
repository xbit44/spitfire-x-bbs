#pragma once
// Independently authored native SFX protocol contract, not GPL helper source.
// Requires separately verified locked objects, current authority, successful
// child exit, complete actual relay ledgers and durable accounting by the host.
#include "sfx/application/file.hpp"
#include "sfx/application/phase3a_storage.hpp"
#include "qwk_packet_completion.hpp"
#include <array>
#include <charconv>
#include <optional>

namespace sfx::apps::runtime::file_transfer_detail {
enum class Direction {receive,send};
struct ExpectedFile {std::uint64_t maximum{},actual{};};
struct SlotCompletion {bool offered{},completed{},skipped{};std::uint64_t bytes{};};
struct Completion {std::uint64_t consumed{},produced{};std::uint32_t attempted{},refused{};std::vector<SlotCompletion> files;};
inline constexpr std::size_t maximum_receipt=16384;
inline char protocol_code(application::TransferProtocolPreference protocol) noexcept {
    using P=application::TransferProtocolPreference;
    switch(protocol){case P::xmodem:return 'X';case P::ymodem:return 'Y';case P::ymodem_g:return 'G';case P::zmodem:return 'Z';default:return 0;}
}
class OutputLedger final {
public:
    bool record(std::string_view bytes) noexcept {
        if(!zmodem_.record(bytes))return false;
        for(char byte:bytes){tail_[at_]=static_cast<unsigned char>(byte);at_=(at_+1)%tail_.size();if(size_<tail_.size())++size_;}
        return true;
    }
    bool finished(Direction direction,application::TransferProtocolPreference protocol,std::uint64_t produced)const noexcept {
        const auto code=protocol_code(protocol);
        if(code=='Z')return zmodem_.finished(direction==Direction::send?qwk_packet_detail::Direction::send:qwk_packet_detail::Direction::receive,produced);
        if(!code||zmodem_.sent()!=produced||!size_)return false;
        if(direction==Direction::receive)return tail_[(at_+tail_.size()-1)%tail_.size()]==(code=='G'?71:6); // G requests final empty header; other modes ACK.
        if(code=='X')return tail_[(at_+tail_.size()-1)%tail_.size()]==4; // EOT acknowledged by peer.
        if(size_!=tail_.size())return false;
        // YMODEM batch ending: 128-byte empty header and its two CRC bytes.
        for(std::size_t i=0;i<tail_.size();++i){const unsigned expected=i==0?1:i==2?255:0;if(tail_[(at_+i)%tail_.size()]!=expected)return false;}
        return true;
    }
private:
    qwk_packet_detail::OutputLedger zmodem_;
    std::array<unsigned char,133> tail_{};std::size_t at_{},size_{};
};
inline std::optional<Completion> completion(Direction direction,std::string_view receipt,const std::vector<ExpectedFile>& expected,
    application::TransferProtocolPreference protocol=application::TransferProtocolPreference::zmodem){
    if(expected.empty()||expected.size()>application::kFileQueueMaximum||receipt.size()>maximum_receipt)return {};
    for(const auto& f:expected)if(!f.maximum||f.maximum>application::kMaximumCallerFileBytes||f.actual>f.maximum)return {};
    const auto literal=[&](std::string_view word){if(receipt.substr(0,word.size())!=word)return false;receipt.remove_prefix(word.size());return true;};
    const auto number=[&](char delimiter)->std::optional<std::uint64_t>{
        const auto end=receipt.find(delimiter);if(end==std::string_view::npos||end==0||end>20)return {};
        const auto text=receipt.substr(0,end);if(text.size()>1&&text.front()=='0')return {};
        std::uint64_t value{};const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
        if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size())return {};
        receipt.remove_prefix(end+1);return value;
    };
    const auto code=protocol_code(protocol);if(!code||(code=='X'&&expected.size()!=1))return {};
    if(code=='Z'){if(!literal("SFX-FILE-TRANSFER 1\ndirection "))return {};}
    else {if(!literal("SFX-FILE-TRANSFER 2\nprotocol ")||receipt.size()<2||receipt[0]!=code||receipt[1]!='\n')return {};receipt.remove_prefix(2);if(!literal("direction "))return {};}
    if(!literal(direction==Direction::receive?"R\n":"S\n"))return {};
    std::array<std::uint64_t,7> fields{};std::size_t i{};
    for(const auto key:{"count ","attempted ","refused ","batch ","failure ","input ","output "}){
        if(!literal(key))return {};
        const auto n=number('\n');if(!n)return {};fields[i++]=*n;
    }
    if(fields[0]!=expected.size()||fields[1]>application::kFileQueueMaximum||fields[2]>fields[1]||fields[3]!=1||fields[4]!=0||!fields[5]||!fields[6])return {};
    Completion result{fields[5],fields[6],static_cast<std::uint32_t>(fields[1]),static_cast<std::uint32_t>(fields[2]),{}};
    std::uint64_t offered{},completed{};
    for(i=0;i<expected.size();++i){
        if(!literal("file "))return {};
        std::array<std::uint64_t,6> row{};
        for(std::size_t col=0;col<row.size();++col){const auto n=number(col==row.size()-1?'\n':' ');if(!n)return {};row[col]=*n;}
        if(row[0]!=i||row[1]>1||row[2]>1||row[5]>1||row[4]>expected[i].maximum)return {};
        const bool seen=row[1]!=0,done=row[2]!=0;const auto skip=row[3];
        if(direction==Direction::receive){
            if(skip!=0&&skip!=2)return {};
            if(done?(!seen||skip||row[5]!=1||row[4]!=expected[i].actual):
                (row[4]||row[5]||expected[i].actual||(seen!=(skip==2))))return {};
        }else{
            if(!seen||skip>1||done==(skip!=0)||row[5]!=static_cast<unsigned>(done)||row[4]!=(done?expected[i].actual:0))return {};
        }
        offered+=seen;completed+=done;result.files.push_back({seen,done,skip!=0,row[4]});
    }
    if(!receipt.empty()||offered>fields[1])return {};
    if(direction==Direction::receive){if(completed+fields[2]!=fields[1])return {};}
    else if(fields[1]!=expected.size()||fields[2]!=0)return {};
    return result;
}
} // namespace sfx::apps::runtime::file_transfer_detail
