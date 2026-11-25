// File metadata uses UTF-8 internally. Native Windows and caller-terminal
// encodings are converted at their boundaries, not used as catalog keys.
#include "file_unicode_fold.inc"
bool next_file_scalar(std::string_view text,std::size_t& offset,std::uint32_t& scalar) noexcept {
    if(offset>=text.size())return false;
    const auto first=static_cast<unsigned char>(text[offset++]);
    if(first<0x80){scalar=first;return true;}
    unsigned remaining{};std::uint32_t minimum{};
    if(first>=0xc2&&first<=0xdf){remaining=1;minimum=0x80;scalar=first&0x1f;}
    else if(first>=0xe0&&first<=0xef){remaining=2;minimum=0x800;scalar=first&0x0f;}
    else if(first>=0xf0&&first<=0xf4){remaining=3;minimum=0x10000;scalar=first&7;}
    else return false;
    if(text.size()-offset<remaining)return false;
    while(remaining--){const auto byte=static_cast<unsigned char>(text[offset++]);if((byte&0xc0)!=0x80)return false;scalar=(scalar<<6)|(byte&0x3f);}
    return scalar>=minimum&&scalar<=0x10ffff&&!(scalar>=0xd800&&scalar<=0xdfff);
}
std::uint32_t fold_file_scalar(std::uint32_t scalar) noexcept {
    const auto end=std::end(file_unicode_folds);
    const auto found=std::lower_bound(std::begin(file_unicode_folds),end,scalar,[](const auto& pair,auto value){return pair.source<value;});
    return found!=end&&found->source==scalar?found->target:scalar;
}
void append_file_scalar(std::string& text,std::uint32_t scalar){
    if(scalar<0x80)text+=static_cast<char>(scalar);
    else if(scalar<0x800){text+=static_cast<char>(0xc0|(scalar>>6));text+=static_cast<char>(0x80|(scalar&63));}
    else if(scalar<0x10000){text+=static_cast<char>(0xe0|(scalar>>12));text+=static_cast<char>(0x80|((scalar>>6)&63));text+=static_cast<char>(0x80|(scalar&63));}
    else{text+=static_cast<char>(0xf0|(scalar>>18));text+=static_cast<char>(0x80|((scalar>>12)&63));text+=static_cast<char>(0x80|((scalar>>6)&63));text+=static_cast<char>(0x80|(scalar&63));}
}
bool unsafe_file_scalar(std::uint32_t value) noexcept {
    return (value>=0x7f&&value<=0x9f)||value==0x061c||value==0x200e||value==0x200f||
        (value>=0x202a&&value<=0x202e)||(value>=0x2066&&value<=0x2069);
}
