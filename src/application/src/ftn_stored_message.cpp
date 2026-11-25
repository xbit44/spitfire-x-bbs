#include "sfx/application/ftn_stored_message.hpp"
#include <algorithm>
#include <sstream>
#include <charconv>
namespace sfx::application {
std::optional<FtnStoredMessage> parse_ftn_stored_message(std::string_view raw){
    if(raw.size()<191||raw.size()>1024*1024||raw.back()!=0)return {};
    const auto word=[&](std::size_t i){return static_cast<std::uint16_t>(static_cast<unsigned char>(raw[i])|(static_cast<unsigned char>(raw[i+1])<<8));};
    const auto field=[&](std::size_t i,std::size_t n)->std::optional<std::string>{const auto v=raw.substr(i,n);const auto end=v.find('\0');if(end==v.npos)return {};std::string s(v.substr(0,end));for(unsigned char c:s)if(c<32||c==127)return {};return s;};
    const auto from=field(0,36),to=field(36,36),subject=field(72,72);if(!from||!to||!subject)return {};
    FtnStoredMessage m;m.from=*from;m.to=*to;m.subject=*subject;m.attributes=word(186);
    m.origin={word(178),word(172),word(168),word(182),{}};m.destination={word(176),word(174),word(166),word(180),{}};
    bool intl{},fmpt{},topt{};
    std::string body(raw.substr(190,raw.size()-191));if(body.find('\0')!=body.npos)return {};
    std::replace(body.begin(),body.end(),'\r','\n');std::istringstream lines(body);std::string line;
    while(std::getline(lines,line)){
        if(line.empty()||line[0]!='\1')continue;
        std::istringstream words(line.substr(1));std::string key;words>>key;
        if(key=="INTL"){
            std::string dst,src,extra;words>>dst>>src;if(words>>extra)return {};
            const auto d=parse_ftn_address(dst),o=parse_ftn_address(src);if(intl||!d||!o||d->point||o->point||!d->domain.empty()||!o->domain.empty())return {};
            if(d->net!=m.destination.net||d->node!=m.destination.node||o->net!=m.origin.net||o->node!=m.origin.node||
                (m.origin.zone&&m.origin.zone!=o->zone)||(m.destination.zone&&m.destination.zone!=d->zone))return {};
            m.origin.zone=o->zone;m.destination.zone=d->zone;intl=true;
        }else if(key=="FMPT"||key=="TOPT"){
            std::string value,extra;words>>value;if(words>>extra)return {};unsigned n{};const auto r=std::from_chars(value.data(),value.data()+value.size(),n);
            if(value.empty()||r.ec!=std::errc{}||r.ptr!=value.data()+value.size()||n>65535)return {};
            auto& point=key=="FMPT"?m.origin.point:m.destination.point;auto& seen=key=="FMPT"?fmpt:topt;if(seen||(point&&point!=n))return {};point=static_cast<std::uint16_t>(n);seen=true;
        }else if(key=="FLAGS"){
            std::string flag;while(words>>flag){if(flag=="KFS")m.kill_files=true;else if(flag=="TFS")m.truncate_files=true;else if(flag=="HLD")m.hold=true;}
        }
    }
    if(!m.origin.zone||!m.destination.zone||!m.origin.net||!m.destination.net||(m.kill_files&&m.truncate_files))return {};
    // Quoted paths may contain spaces. Unquoted whitespace separates file specs.
    for(std::size_t at=0;at<m.subject.size();){
        while(at<m.subject.size()&&m.subject[at]==' ')++at;
        if(at==m.subject.size())break;
        const bool quoted=m.subject[at]=='"';if(quoted)++at;const auto begin=at;
        while(at<m.subject.size()&&(quoted?m.subject[at]!='"':m.subject[at]!=' '))++at;
        if(begin==at||(quoted&&at==m.subject.size())||m.attachments.size()==16)return {};
        m.attachments.push_back(m.subject.substr(begin,at-begin));if(quoted){++at;if(at<m.subject.size()&&m.subject[at]!=' ')return {};}
    }
    if(m.attachments.empty())return {};
    return m;
}
}
