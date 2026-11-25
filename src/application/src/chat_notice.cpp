#include "sfx/application/chat_notice.hpp"
namespace sfx::application {
std::vector<std::uint8_t> encode_chat_notice(const ChatNoticeRequest& r){
    if(r.text.empty()||r.text.size()>512||chat_safe_text(r.text)!=r.text||
       (r.target&&(!r.target->valid()||r.room))||(!r.target&&r.override_dnd))return {};
    std::vector<std::uint8_t> b{1,static_cast<std::uint8_t>(r.target.has_value()),static_cast<std::uint8_t>(r.override_dnd)};
    const auto n=[&](std::uint64_t v,unsigned bytes){for(unsigned i=0;i<bytes;++i)b.push_back(static_cast<std::uint8_t>(v>>(8*i)));};
    n(r.room,2);if(r.target){b.insert(b.end(),r.target->runtime_epoch.bytes.begin(),r.target->runtime_epoch.bytes.end());n(r.target->node,2);n(r.target->generation,8);}
    n(r.text.size(),2);b.insert(b.end(),r.text.begin(),r.text.end());return b;
}
std::optional<ChatNoticeRequest> decode_chat_notice(const std::vector<std::uint8_t>& b){
    if(b.size()<7||b.size()>545||b[0]!=1||b[1]>1||b[2]>1)return {};
    ChatNoticeRequest r;r.override_dnd=b[2]!=0;std::size_t p=3;bool ok=true;
    const auto n=[&](unsigned bytes){std::uint64_t v{};if(bytes>b.size()-p){ok=false;return v;}for(unsigned i=0;i<bytes;++i)v|=static_cast<std::uint64_t>(b[p++])<<(8*i);return v;};
    r.room=static_cast<ChatRoomId>(n(2));if(b[1]){if(b.size()<33)return {};NodeAssignmentIdentity id;for(auto& c:id.runtime_epoch.bytes)c=b[p++];id.node=static_cast<std::uint16_t>(n(2));id.generation=n(8);r.target=id;}
    const auto size=n(2);if(!ok||size!=b.size()-p)return {};r.text.assign(b.begin()+p,b.end());if(encode_chat_notice(r)!=b)return {};return r;
}
#include "chat_operator_codec.inc"
}
