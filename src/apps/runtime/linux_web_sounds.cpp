#include "linux_web_sounds.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <sodium.h>
#include <cstring>
#include <stdexcept>

namespace sfx::apps::runtime::web {
bool linux_sound_path(std::string_view path) noexcept {
    return !path.empty()&&path.front()=='/'&&application::valid_event_sound_path(path);
}
namespace {
std::uint16_t u16(const std::uint8_t* p){return static_cast<std::uint16_t>(p[0]|p[1]<<8U);}
std::uint32_t u32(const std::uint8_t* p){return p[0]|(std::uint32_t(p[1])<<8U)|(std::uint32_t(p[2])<<16U)|(std::uint32_t(p[3])<<24U);}
}
bool pcm_sound(const std::vector<std::uint8_t>& bytes,std::uint64_t& duration) noexcept {
    duration=0;
    if(bytes.size()<44||bytes.size()>2U*1024U*1024U||std::memcmp(bytes.data(),"RIFF",4)||std::memcmp(bytes.data()+8,"WAVE",4)||u32(bytes.data()+4)!=bytes.size()-8)return false;
    bool format{},data{};std::uint32_t rate{},length{};std::uint16_t block{};
    for(std::size_t at=12;at<bytes.size();){
        if(bytes.size()-at<8)return false;
        const auto n=u32(bytes.data()+at+4);const auto start=at+8;
        if(n>bytes.size()-start)return false;
        if(!std::memcmp(bytes.data()+at,"fmt ",4)){
            if(format||(n!=16&&(n!=18||u16(bytes.data()+start+16)!=0)))return false;
            format=true;const auto* p=bytes.data()+start;const auto channels=u16(p+2),bits=u16(p+14);const auto hz=u32(p+4);rate=u32(p+8);block=u16(p+12);
            if(u16(p)!=1||(channels!=1&&channels!=2)||(bits!=8&&bits!=16)||hz<8000||hz>48000||block!=channels*(bits/8)||rate!=hz*block)return false;
        }else if(!std::memcmp(bytes.data()+at,"data",4)){if(data)return false;data=true;length=n;}
        const auto next=start+n+(n&1U);if(next>bytes.size())return false;at=next;
    }
    if(!format||!data||!length||!block||length%block||length>std::uint64_t(rate)*30U)return false;
    duration=(std::uint64_t(length)*1000U+rate-1U)/rate;return true;
}
WebSoundFile read_web_sound(std::string_view path){
    if(!linux_sound_path(path))throw std::runtime_error("sound_path_invalid");
    const std::filesystem::path native{std::string(path)};
    const auto folder=platform::linux_host::open_owned_directory(native.parent_path(),false);
    WebSoundFile result;result.bytes=platform::linux_host::read_owned_resource(folder.get(),native.filename().string(),2U*1024U*1024U);
    if(!pcm_sound(result.bytes,result.duration_ms))throw std::runtime_error("sound_pcm_invalid");
    return result;
}
application::Identifier128 web_sound_owner(std::string_view session,std::string_view tab){
    if(session.empty()||session.size()>256||tab.size()!=32||sodium_init()<0)throw std::runtime_error("sound_owner_invalid");
    for(const auto c:tab)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))throw std::runtime_error("sound_tab_invalid");
    const auto input=std::string(session)+'|'+std::string(tab);application::Identifier128 owner;
    crypto_generichash(owner.bytes.data(),owner.bytes.size(),reinterpret_cast<const unsigned char*>(input.data()),input.size(),nullptr,0);
    if(!owner.valid())throw std::runtime_error("sound_owner_invalid");
    return owner;
}
}
