#pragma once
// SFX-owned, platform-neutral serialization only. No driver or launcher code is
// linked across this boundary. The Windows endpoints additionally authenticate
// the inherited pipes, parent process, installed images and private session.
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfx::core::dos_host {
inline constexpr std::size_t maximum_request=32768,maximum_frame=4096,maximum_queued_frames=64;
struct Request {
    std::uint32_t parent_pid{},account{},node{},mode{};
    std::uint64_t parent_birth{},generation{},sequence{};
    // All six handles are an explicit CreateProcess inheritance allowlist.
    std::uint64_t parent_process{},input{},output{},error{},control{},status{};
    bool can_disconnect{true},allow_x64{};
    std::string root,startup,node_directory,drop_directory,private_directory;
    std::string command,session,driver_ini,bbsdev_path;
    std::string doordrop_path;
};
namespace detail {
inline void put32(std::vector<std::uint8_t>& out,std::uint32_t n){for(unsigned i=0;i<4;++i)out.push_back(static_cast<std::uint8_t>(n>>(8*i)));}
inline void put64(std::vector<std::uint8_t>& out,std::uint64_t n){for(unsigned i=0;i<8;++i)out.push_back(static_cast<std::uint8_t>(n>>(8*i)));}
inline void put_text(std::vector<std::uint8_t>& out,const std::string& s){put32(out,static_cast<std::uint32_t>(s.size()));out.insert(out.end(),s.begin(),s.end());}
struct Reader {
    const std::vector<std::uint8_t>& bytes;std::size_t at{};bool good{true};
    std::uint64_t integer(unsigned count){if(!good||count>bytes.size()-at){good=false;return 0;}std::uint64_t n{};for(unsigned i=0;i<count;++i)n|=std::uint64_t(bytes[at++])<<(8*i);return n;}
    std::uint32_t u32(){return static_cast<std::uint32_t>(integer(4));}
    std::string text(std::size_t bound){const auto n=u32();if(!good||n>bound||n>bytes.size()-at){good=false;return {};}
        std::string s(bytes.begin()+at,bytes.begin()+at+n);at+=n;if(s.find('\0')!=s.npos)good=false;return s;}
};
inline bool path(std::string_view s){return s.size()>=3&&s.size()<=1024&&((s[0]>='A'&&s[0]<='Z')||(s[0]>='a'&&s[0]<='z'))&&s[1]==':'&&(s[2]=='\\'||s[2]=='/')&&s.find_first_of("\r\n\0",0,3)==s.npos;}
inline bool valid(const Request& r){
    if(!r.parent_pid||!r.parent_birth||!r.account||r.node<1||r.node>128||!r.generation||!r.sequence||(r.mode!=3&&r.mode!=4&&r.mode!=8&&r.mode!=12))return false;
    std::array<std::uint64_t,6> handles{r.parent_process,r.input,r.output,r.error,r.control,r.status};std::sort(handles.begin(),handles.end());
    if(handles.front()==0||handles.back()>=UINT32_MAX||std::adjacent_find(handles.begin(),handles.end())!=handles.end())return false;
    if(!path(r.root)||!path(r.startup)||!path(r.node_directory)||!path(r.drop_directory)||!path(r.private_directory)||(!r.bbsdev_path.empty()&&!path(r.bbsdev_path)))return false;
    if (!r.doordrop_path.empty() && (!r.bbsdev_path.empty() || !path(r.doordrop_path) ||
        r.doordrop_path.size() > 80 || r.doordrop_path.substr(r.doordrop_path.find_last_of("/\\")+1) != "DOORDROP.INI")) return false;
    if(r.command.empty()||r.command.size()>4096||r.command.find_first_of("\r\n\0",0,3)!=r.command.npos||r.driver_ini.size()>16384||r.driver_ini.find('\0')!=r.driver_ini.npos)return false;
    return r.session.size()==32&&std::all_of(r.session.begin(),r.session.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
}
inline std::string environment_file(const Request& r) {
    std::string contents = r.command + "\r\n";
    if (!r.bbsdev_path.empty()) contents += "BBSDEV_DRP=" + r.bbsdev_path + "\r\n";
    if (!r.doordrop_path.empty()) contents += "DOORDROP_INI=" + r.doordrop_path + "\r\n";
    return contents;
}
inline std::optional<std::vector<std::uint8_t>> encode(const Request& r){
    if(!detail::valid(r))return {};std::vector<std::uint8_t> out{'S','F','X','D','O','O','R','1'};detail::put32(out,r.doordrop_path.empty()?1:2);detail::put32(out,0);
    for(auto n:{r.parent_pid,r.account,r.node,r.mode})detail::put32(out,n);
    for(auto n:{r.parent_birth,r.generation,r.sequence,r.parent_process,r.input,r.output,r.error,r.control,r.status})detail::put64(out,n);
    detail::put32(out,(r.can_disconnect?1u:0u)|(r.allow_x64?2u:0u));
    for(const auto* s:{&r.root,&r.startup,&r.node_directory,&r.drop_directory,&r.private_directory,&r.command,&r.session,&r.driver_ini,&r.bbsdev_path})detail::put_text(out,*s);
    if (!r.doordrop_path.empty()) detail::put_text(out,r.doordrop_path);
    if(out.size()>maximum_request)return {};const auto size=static_cast<std::uint32_t>(out.size());for(unsigned i=0;i<4;++i)out[12+i]=static_cast<std::uint8_t>(size>>(i*8));return out;
}
inline std::optional<Request> decode(const std::vector<std::uint8_t>& bytes){
    if(bytes.size()<16||bytes.size()>maximum_request||std::string_view(reinterpret_cast<const char*>(bytes.data()),8)!="SFXDOOR1")return {};
    detail::Reader read{bytes,8};const auto version=read.u32();if((version!=1&&version!=2)||read.u32()!=bytes.size())return {};Request r;
    r.parent_pid=read.u32();r.account=read.u32();r.node=read.u32();r.mode=read.u32();
    r.parent_birth=read.integer(8);r.generation=read.integer(8);r.sequence=read.integer(8);r.parent_process=read.integer(8);r.input=read.integer(8);r.output=read.integer(8);r.error=read.integer(8);r.control=read.integer(8);r.status=read.integer(8);
    const auto flags=read.u32();if(flags>3)return {};r.can_disconnect=(flags&1)!=0;r.allow_x64=(flags&2)!=0;
    r.root=read.text(1024);r.startup=read.text(1024);r.node_directory=read.text(1024);r.drop_directory=read.text(1024);r.private_directory=read.text(1024);r.command=read.text(4096);r.session=read.text(32);r.driver_ini=read.text(16384);r.bbsdev_path=read.text(1024);
    if (version==2) { r.doordrop_path=read.text(80); if (r.doordrop_path.empty()) return {}; }
    if(!read.good||read.at!=bytes.size()||!detail::valid(r))return {};return r;
}
enum class Message : std::uint32_t {started=1,finished=2,failed=3,hangup=4,stop=5};
struct Frame {Message message{Message::failed};std::uint32_t value{};std::string diagnostic;};
inline std::optional<std::vector<std::uint8_t>> encode_frame(const Frame& f){
    if(static_cast<unsigned>(f.message)<1||static_cast<unsigned>(f.message)>5||f.diagnostic.size()>maximum_frame-16||!std::all_of(f.diagnostic.begin(),f.diagnostic.end(),[](unsigned char c){return c>=32&&c<127;}))return {};
    std::vector<std::uint8_t> out;detail::put32(out,static_cast<std::uint32_t>(16+f.diagnostic.size()));detail::put32(out,static_cast<std::uint32_t>(f.message));detail::put32(out,f.value);detail::put_text(out,f.diagnostic);return out;
}
inline std::optional<Frame> decode_frame(const std::vector<std::uint8_t>& bytes){
    if(bytes.size()<16||bytes.size()>maximum_frame)return {};detail::Reader r{bytes};if(r.u32()!=bytes.size())return {};Frame f;f.message=static_cast<Message>(r.u32());f.value=r.u32();f.diagnostic=r.text(maximum_frame-16);
    if(!r.good||r.at!=bytes.size()||!encode_frame(f))return {};return f;
}
} // namespace sfx::core::dos_host
