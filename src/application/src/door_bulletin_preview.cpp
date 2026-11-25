#include "sfx/application/door_bulletin_preview.hpp"
namespace sfx::application {
bool valid_door_bulletin_preview(const DoorBulletinPreview& p)noexcept {
    if(!valid_door_bulletin_id(p.id)||!valid_door_text(p.title,64)||p.diagnostic.size()>512||p.columns!=80||(p.rows!=0&&p.rows!=25)||p.cells.size()!=std::size_t(p.columns)*p.rows)return false;
    if(!p.available&&(p.rows||!p.cells.empty()))return false;
    for(const auto c:p.diagnostic)if(static_cast<unsigned char>(c)<32||c==127)return false;
    for(const auto& c:p.cells){if(c.glyph<32||c.glyph==127)return false;}
    return true;
}
std::vector<std::uint8_t> encode_door_bulletin_preview(const DoorBulletinPreview& p){
    if(!valid_door_bulletin_preview(p))return {};
    std::vector<std::uint8_t> out;
    const auto number=[&](std::uint64_t n){for(unsigned i=0;i<8;++i)out.push_back(static_cast<std::uint8_t>(n>>(8*i)));};const auto string=[&](std::string_view s){number(s.size());out.insert(out.end(),s.begin(),s.end());};
    number(1);number(p.id.value);number(p.available);string(p.title);string(p.diagnostic);number(p.columns);number(p.rows);for(const auto& c:p.cells){out.push_back(c.glyph);out.push_back(c.attribute);}return out;
}
std::optional<DoorBulletinPreview> decode_door_bulletin_preview(const std::vector<std::uint8_t>& bytes){
    if(bytes.size()>5024)return {};
    std::size_t at{};bool valid=true;const auto number=[&](){if(bytes.size()-at<8){valid=false;return std::uint64_t{};}std::uint64_t n{};for(unsigned i=0;i<8;++i)n|=std::uint64_t(bytes[at++])<<(8*i);return n;};
    const auto string=[&](std::size_t max){const auto n=number();if(!valid||n>max||n>bytes.size()-at){valid=false;return std::string{};}std::string out(bytes.begin()+at,bytes.begin()+at+static_cast<std::size_t>(n));at+=static_cast<std::size_t>(n);return out;};
    if(number()!=1)return {};
    DoorBulletinPreview p;const auto id=number(),available=number();if(id<1||id>999||available>1)return {};p.id={static_cast<std::uint16_t>(id)};p.available=available!=0;p.title=string(64);p.diagnostic=string(512);const auto columns=number(),rows=number();if(columns!=80||(rows!=0&&rows!=25)||!valid||bytes.size()-at!=columns*rows*2)return {};p.columns=80;p.rows=static_cast<std::uint16_t>(rows);for(std::size_t i=0;i<columns*rows;++i){p.cells.push_back({bytes[at],bytes[at+1]});at+=2;}if(!valid_door_bulletin_preview(p))return {};return p;
}
}
