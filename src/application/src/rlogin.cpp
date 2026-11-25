#include "sfx/application/rlogin.hpp"
#include <algorithm>
#include <cstring>
#include <deque>
#include <utility>

namespace sfx::application {
namespace {
template<class Buffer> struct EraseBuffer {
    Buffer& value;
    ~EraseBuffer(){if(!value.empty())erase_rlogin_secret(value.data(),value.size()*sizeof(typename Buffer::value_type));}
};
}
void erase_rlogin_secret(void* p,std::size_t bytes) noexcept {
    auto* v=static_cast<volatile unsigned char*>(p);while(bytes--)*v++=0;
}
RloginSecret::RloginSecret(std::string_view v):bytes_(v.begin(),v.end()){}
RloginSecret::RloginSecret(RloginSecret&& v) noexcept:bytes_(std::move(v.bytes_)){}
RloginSecret& RloginSecret::operator=(RloginSecret&& v) noexcept {
    if(this!=&v){clear();bytes_=std::move(v.bytes_);}return *this;
}
RloginSecret::~RloginSecret(){clear();}
std::string_view RloginSecret::view() const noexcept {return {bytes_.empty()?"":bytes_.data(),bytes_.size()};}
void RloginSecret::clear() noexcept {if(!bytes_.empty())erase_rlogin_secret(bytes_.data(),bytes_.size());bytes_.clear();}
std::optional<RloginSecret> rlogin_handshake(const RloginTarget& t,const RloginCallerIdentity& c,
    std::string_view secret,std::string_view pepper,const RloginSha1& hash){
    if(!valid_rlogin_target(t) || !valid_door_text(c.alias,127) || !valid_door_text(secret,128,true) ||
        !valid_door_text(pepper,128,true) || !valid_door_text(c.board_qwk_id,128,true))return {};
    RloginSecret client(t.client_name.empty()?c.alias:t.client_name);
    if(t.authentication==RloginAuthentication::traditional){
        if(!secret.empty())client=RloginSecret(secret);
        else if(t.prompt_credential || !t.protected_credential.empty())return {};
    }else{
        if(secret.empty() || !c.account_number || !c.firston_date || !hash ||
            (t.authentication==RloginAuthentication::synchronet_predefined_hash && !pepper.empty()))return {};
        // Independent implementation of pinned Synchronet rlogin.js semantics:
        // SHA1(password + decimal user.number + decimal firston_date +
        //      (configured salt || system.qwk_id) + pepper), hexadecimal.
        const auto n=std::to_string(c.account_number),d=std::to_string(c.firston_date);
        const auto& salt=t.salt.empty()?c.board_qwk_id:t.salt;
        if(salt.empty())return {};
        std::vector<char> input;input.reserve(secret.size()+n.size()+d.size()+salt.size()+pepper.size());
        const auto append=[&](std::string_view s){input.insert(input.end(),s.begin(),s.end());};
        append(secret);append(n);append(d);append(salt);append(pepper);
        EraseBuffer<std::vector<char>> wipe{input};
        auto digest=hash({input.data(),input.size()});if(!digest)return {};
        EraseBuffer<std::array<std::uint8_t,20>> erase_digest{*digest};
        std::array<char,40> hex{};constexpr char alphabet[]="0123456789abcdef";
        EraseBuffer<std::array<char,40>> erase_hex{hex};
        for(std::size_t i=0;i<digest->size();++i){hex[i*2]=alphabet[(*digest)[i]>>4];hex[i*2+1]=alphabet[(*digest)[i]&15];}
        client=RloginSecret({hex.data(),hex.size()});erase_rlogin_secret(hex.data(),hex.size());erase_rlogin_secret(digest->data(),digest->size());
    }
    const auto& server=t.server_identity==RloginIdentity::caller_alias?c.alias:t.server_name;
    if(!valid_door_text(client.view(),128,true) || !valid_door_text(server,127))return {};
    std::vector<char> frame;frame.reserve(client.view().size()+server.size()+t.terminal_type.size()+4);
    EraseBuffer<std::vector<char>> erase_frame{frame};
    frame.push_back(0);for(const auto value:{client.view(),std::string_view(server),std::string_view(t.terminal_type)}){frame.insert(frame.end(),value.begin(),value.end());frame.push_back(0);}
    RloginSecret result({frame.data(),frame.size()});erase_rlogin_secret(frame.data(),frame.size());return result;
}
std::array<std::uint8_t,12> rlogin_window_size(std::uint16_t rows,std::uint16_t columns) noexcept {
    return {{255,255,'s','s',static_cast<std::uint8_t>(rows>>8),static_cast<std::uint8_t>(rows),
        static_cast<std::uint8_t>(columns>>8),static_cast<std::uint8_t>(columns),0,0,0,0}};
}
const char* rlogin_result_code(RloginEnd e) noexcept {
    switch(e){
    case RloginEnd::remote_closed:return "remote_closed";
    case RloginEnd::gateway_escape:return "gateway_escape";
    case RloginEnd::operator_terminated:return "operator_terminated";
    case RloginEnd::caller_disconnected:return "caller_disconnected";
    case RloginEnd::shutdown:return "shutdown";
    case RloginEnd::authority_lost:return "authority_lost";
    case RloginEnd::calling_time:return "calling_time";
    case RloginEnd::inactivity:return "inactivity";
    case RloginEnd::connection_failed:return "connection_failed";
    case RloginEnd::handshake_failed:return "handshake_failed";
    case RloginEnd::transport_failed:return "transport_failed";
    case RloginEnd::backpressure_timeout:return "backpressure_timeout";
    }
    return "invalid";
}
RloginBridgeResult run_rlogin_bridge(const RloginBridgePorts& p,std::vector<std::uint8_t> prefix,bool delimiter){
    RloginBridgeResult result;
    if(!p.caller_read||!p.remote_read||!p.caller_write||!p.remote_write||!p.stop||!p.now||!p.wait||prefix.size()>4096)return result;
    struct Buffer {
        struct Mark {std::uint64_t end{},since{};};
        std::array<char,kRloginBridgeBufferBytes> data{};std::size_t begin{},end{};
        std::uint64_t produced{},consumed{};std::deque<Mark> marks;
        std::size_t size()const{return end-begin;}
        void compact(){if(begin){std::memmove(data.data(),data.data()+begin,size());end-=begin;begin=0;}}
        void append(std::uint8_t c,std::uint64_t now){
            data[end++]=static_cast<char>(c);++produced;
            if(marks.empty() || (marks.back().since!=now && marks.size()<64))marks.push_back({produced,now});
            else marks.back().end=produced;
        }
        void consume(std::size_t n){begin+=n;consumed+=n;while(!marks.empty() && marks.front().end<=consumed)marks.pop_front();if(!size())begin=end=0;}
        void discard(){consume(size());}
    } outbound,inbound;
    bool escape{},remote_closed{},window_requested{};std::uint64_t escape_since{};
    std::optional<std::pair<std::uint16_t,std::uint16_t>> last_dimensions;
    const auto finish=[&](RloginEnd e){result.end=e;return result;};
    for(;;){
        if(const auto stop=p.stop())return finish(*stop);
        const auto now=p.now();bool progress{};
        // A lone prefix is an ordinary game key after a bounded lookahead.
        if(escape && now-escape_since>=500){outbound.compact();if(outbound.end<outbound.data.size()){outbound.append(29,now);escape=false;}}
        if(p.urgent){if(const auto urgent=p.urgent()){
            if(*urgent&2){if(p.caller_discard&&!p.caller_discard())return finish(RloginEnd::caller_disconnected);inbound.discard();}
            // BBS gateway stays eight-bit raw, as an rlogin client started with
            // forced-raw mode; START/STOP keys remain ordinary game input.
            if(*urgent&128)window_requested=true;
        }}
        if(window_requested&&p.dimensions){outbound.compact();if(outbound.data.size()-outbound.end>=12){const auto dims=p.dimensions();const auto frame=rlogin_window_size(dims.first,dims.second);for(const auto c:frame)outbound.append(c,now);last_dimensions=dims;window_requested=false;}}
        if(last_dimensions && p.dimensions && p.dimensions()!=*last_dimensions){outbound.compact();if(outbound.data.size()-outbound.end>=12){const auto dims=p.dimensions();const auto f=rlogin_window_size(dims.first,dims.second);for(const auto c:f)outbound.append(c,now);last_dimensions=dims;}}
        const auto flush=[&](Buffer& buffer,const auto& write,std::uint64_t& count)->std::optional<RloginEnd>{
            if(!buffer.size())return {};
            const auto offered=std::min<std::size_t>(buffer.size(),4096);
            const auto w=write({buffer.data.data()+buffer.begin,offered});
            if(!valid_interactive_stream_write_result(offered,w))return RloginEnd::transport_failed;
            if(w.bytes_written){buffer.consume(w.bytes_written);count+=w.bytes_written;progress=true;}
            if(w.outcome!=InteractiveStreamOutcome::completed && w.outcome!=InteractiveStreamOutcome::timed_out)return RloginEnd::transport_failed;
            // Hard occupancy deadline is not renewed by trickle progress.
            if(!buffer.marks.empty() && now-buffer.marks.front().since>=5000)return RloginEnd::backpressure_timeout;
            return {};
        };
        if(const auto e=flush(outbound,p.remote_write,result.caller_bytes))return finish(*e);
        if(const auto e=flush(inbound,p.caller_write,result.remote_bytes))return finish(*e);
        if(remote_closed && !inbound.size())return finish(RloginEnd::remote_closed);
        outbound.compact();inbound.compact();
        if(outbound.data.size()-outbound.end>=4097){
            InteractiveStreamReadResult r;
            if(!prefix.empty()){r={InteractiveStreamOutcome::completed,std::move(prefix)};prefix.clear();}
            else r=p.caller_read(4096);
            if(!valid_interactive_stream_read_result(4096,r))return finish(RloginEnd::transport_failed);
            if(r.outcome==InteractiveStreamOutcome::completed){
                if(p.keyboard)p.keyboard();
                progress=true;
                for(const auto c:r.bytes){
                    if(delimiter){delimiter=false;if(c==10)continue;}
                    if(escape){escape=false;if(c=='d'||c=='D')return finish(RloginEnd::gateway_escape);
                        outbound.append(29,now);if(c==29)continue;
                    }else if(c==29){escape=true;escape_since=now;continue;}
                    outbound.append(c,now);
                }
            }else if(r.outcome!=InteractiveStreamOutcome::timed_out)return finish(RloginEnd::caller_disconnected);
        }
        if(!remote_closed && inbound.data.size()-inbound.end>=4096){
            const auto r=p.remote_read(4096);
            if(!valid_interactive_stream_read_result(4096,r))return finish(RloginEnd::transport_failed);
            if(r.outcome==InteractiveStreamOutcome::completed){for(const auto c:r.bytes)inbound.append(c,now);progress=true;}
            else if(r.outcome==InteractiveStreamOutcome::disconnected)remote_closed=true;
            else if(r.outcome!=InteractiveStreamOutcome::timed_out)return finish(RloginEnd::transport_failed);
        }
        result.outbound_high_water=std::max(result.outbound_high_water,outbound.size());result.inbound_high_water=std::max(result.inbound_high_water,inbound.size());
        if(!progress)p.wait();
    }
}
} // namespace sfx::application
