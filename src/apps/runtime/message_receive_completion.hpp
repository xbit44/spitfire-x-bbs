#pragma once
// Independently authored SFX relay accounting. No separate GPL helper code is
// included here. Counts only successful send/write prefixes, never offered data.
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace sfx::apps::runtime::message_receive_detail {
inline std::optional<std::uint64_t> advanced_count(std::uint64_t count,std::size_t prefix_size)noexcept{
    if(prefix_size>(std::numeric_limits<std::uint64_t>::max)()-count)return {};
    return count+prefix_size;
}
class ForwardedInput final {
public:
    bool record(std::string_view prefix)noexcept{
        const auto count=advanced_count(sent_,prefix.size());if(!count)return false;
        for(unsigned char byte:prefix)suffix_=static_cast<std::uint16_t>((suffix_<<8)|byte);
        sent_=*count;return true;
    }
    bool completion_tail(std::uint64_t consumed,std::string_view pending)const noexcept{
        if(!consumed || consumed>sent_)return false;
        const auto remainder=sent_-consumed;
        if(remainder>2 || pending.size()>2-remainder)return false;
        std::array<unsigned char,2> tail{};std::size_t count=0;
        if(remainder==2)tail[count++]=static_cast<unsigned char>(suffix_>>8);
        if(remainder)tail[count++]=static_cast<unsigned char>(suffix_);
        for(unsigned char byte:pending)tail[count++]=byte;
        if(!count)return true;
        if(count==1)return tail[0]=='\r' || tail[0]=='\n';
        return tail[0]=='\r' && tail[1]=='\n';
    }
    std::uint64_t sent()const noexcept{return sent_;}
private:
    std::uint64_t sent_{};std::uint16_t suffix_{};
};

inline std::optional<std::uint64_t> completed_input(std::string_view receipt,std::size_t payload_bytes){
    if(!payload_bytes || payload_bytes>262144)return {};
    const auto prefix=std::string("SFX-MESSAGE-RECEIVE 2\nattempted 1\ncompleted 1\nbytes ")+
        std::to_string(payload_bytes)+"\nzeof 1\nbatch 1\nfailure 0\ninput ";
    if(receipt.size()<=prefix.size()+1 || receipt.substr(0,prefix.size())!=prefix || receipt.back()!='\n')return {};
    const auto digits=receipt.substr(prefix.size(),receipt.size()-prefix.size()-1);
    if(digits.empty() || digits.size()>20 || digits.front()<'1' || digits.front()>'9')return {};
    std::uint64_t count{};const auto parsed=std::from_chars(digits.data(),digits.data()+digits.size(),count);
    if(parsed.ec!=std::errc{} || parsed.ptr!=digits.data()+digits.size() || !count)return {};
    return count;
}

class ForwardedOutput final {
public:
    // Exact reviewed helper's last protocol header: hex ZFIN, zero position,
    // CRC16 022d and CRLF. Require all twenty bytes actually handed to caller.
    static constexpr std::string_view final_header="**\x18" "B0800000000022d\r\n";
    void record(std::string_view prefix)noexcept{
        for(char byte:prefix){last_[position_]=byte;position_=(position_+1)%last_.size();if(size_<last_.size())++size_;}
    }
    bool finished()const noexcept{
        if(size_!=last_.size())return false;
        for(std::size_t i=0;i<last_.size();++i)if(last_[(position_+i)%last_.size()]!=final_header[i])return false;
        return true;
    }
private:
    std::array<char,20> last_{};std::size_t position_{},size_{};
};
static_assert(ForwardedOutput::final_header.size()==20);
} // namespace sfx::apps::runtime::message_receive_detail
