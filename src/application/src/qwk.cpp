#include "sfx/application/qwk.hpp"
#include "sfx/application/message.hpp"
#include "../../apps/product_version.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace sfx::application {
namespace {
constexpr std::array<std::uint16_t,128> cp437_high{{
    0x00c7,0x00fc,0x00e9,0x00e2,0x00e4,0x00e0,0x00e5,0x00e7,
    0x00ea,0x00eb,0x00e8,0x00ef,0x00ee,0x00ec,0x00c4,0x00c5,
    0x00c9,0x00e6,0x00c6,0x00f4,0x00f6,0x00f2,0x00fb,0x00f9,
    0x00ff,0x00d6,0x00dc,0x00a2,0x00a3,0x00a5,0x20a7,0x0192,
    0x00e1,0x00ed,0x00f3,0x00fa,0x00f1,0x00d1,0x00aa,0x00ba,
    0x00bf,0x2310,0x00ac,0x00bd,0x00bc,0x00a1,0x00ab,0x00bb,
    0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,
    0x2555,0x2563,0x2551,0x2557,0x255d,0x255c,0x255b,0x2510,
    0x2514,0x2534,0x252c,0x251c,0x2500,0x253c,0x255e,0x255f,
    0x255a,0x2554,0x2569,0x2566,0x2560,0x2550,0x256c,0x2567,
    0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256b,
    0x256a,0x2518,0x250c,0x2588,0x2584,0x258c,0x2590,0x2580,
    0x03b1,0x00df,0x0393,0x03c0,0x03a3,0x03c3,0x00b5,0x03c4,
    0x03a6,0x0398,0x03a9,0x03b4,0x221e,0x03c6,0x03b5,0x2229,
    0x2261,0x00b1,0x2265,0x2264,0x2320,0x2321,0x00f7,0x2248,
    0x00b0,0x2219,0x00b7,0x221a,0x207f,0x00b2,0x25a0,0x00a0
}};
char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c-'a'+'A') : c; }
std::string uppercase(std::string_view v) { std::string r(v); for(auto& c:r)c=upper(c); return r; }
std::string_view trim(std::string_view v) {
    while(!v.empty() && (v.front()==' ' || v.front()=='\t'))v.remove_prefix(1);
    while(!v.empty() && (v.back()==' ' || v.back()=='\t' || v.back()=='\r'))v.remove_suffix(1);
    return v;
}
bool text_field(std::string_view v, std::size_t maximum, bool blank=false) {
    return v.size()<=maximum && (blank || !trim(v).empty()) &&
        std::none_of(v.begin(),v.end(),[](unsigned char c){return c<32 || c==127;});
}
std::optional<std::uint32_t> decimal(std::string_view text, std::uint32_t maximum) {
    text=trim(text); if(text.empty())return {};
    std::uint32_t value{}; const auto r=std::from_chars(text.data(),text.data()+text.size(),value);
    if(r.ec!=std::errc{} || r.ptr!=text.data()+text.size() || value>maximum)return {};
    return value;
}
bool date_time(std::string_view date, std::string_view time) {
    if(date.size()!=8 || time.size()!=5 || date[2]!='-' || date[5]!='-' || time[2]!=':')return false;
    const auto month=decimal(date.substr(0,2),12), day=decimal(date.substr(3,2),31),
        year=decimal(date.substr(6,2),99), hour=decimal(time.substr(0,2),23), minute=decimal(time.substr(3,2),59);
    if(!month || !*month || !day || !*day || !year || !hour || !minute)return false;
    constexpr unsigned days[]{31,28,31,30,31,30,31,31,30,31,30,31};
    const auto maximum=days[*month-1]+((*month==2 && *year%4==0)?1U:0U);
    return *day<=maximum;
}
void field(std::string& block, std::size_t offset, std::size_t width, std::string_view text) {
    block.replace(offset,std::min(width,text.size()),text.substr(0,width));
}
std::string hex(std::size_t value) {
    std::array<char,2*sizeof(std::size_t)> buf{};
    const auto r=std::to_chars(buf.data(),buf.data()+buf.size(),value,16);
    return {buf.data(),r.ptr};
}
void identity_field(std::string& out,std::string_view v) {
    out+=std::to_string(v.size());out+=':';out+=v;out+=';';
}
std::string rewrap(std::string_view source) {
    std::string out;
    for(std::size_t pos=0;pos<source.size();) {
        const auto end=source.find('\n',pos); const auto count=(end==std::string_view::npos?source.size():end)-pos;
        auto line=source.substr(pos,count);
        const auto rows=wrap_message_line(line,80);
        for(std::size_t row=0;row<rows.size();++row){out+=rows[row].text;if(row+1<rows.size()||end!=std::string_view::npos)out+='\n';}
        if(end==std::string_view::npos)break;
        pos=end+1;
    }
    return out;
}
void mbf_index(std::string& out,std::uint32_t record,std::uint16_t conference) {
    // Microsoft Binary Format: 24-bit significand, exponent biased by 129.
    unsigned bit{}; for(auto n=record;n>1;n>>=1)++bit;
    const auto significant=record<<(23-bit);
    out+=static_cast<char>(significant&255);out+=static_cast<char>((significant>>8)&255);
    out+=static_cast<char>((significant>>16)&127);out+=static_cast<char>(bit+129);
    out+=static_cast<char>(conference&255);
}
using HeaderFields=std::map<std::string,std::string>;
using Headers=std::map<std::size_t,HeaderFields>;
bool parse_headers(std::string_view input, Headers& headers) {
    HeaderFields* current=nullptr;
    static const std::set<std::string> allowed{"UTF8","SENDER","TO","SUBJECT","MESSAGE-ID","IN-REPLY-TO","WHENWRITTEN"};
    for(std::size_t pos=0;pos<input.size();) {
        const auto end=input.find('\n',pos);auto line=input.substr(pos,(end==std::string_view::npos?input.size():end)-pos);
        pos=end==std::string_view::npos?input.size():end+1;line=trim(line);
        if(line.empty())continue;
        if(line.front()=='[') {
            if(line.size()<3 || line.back()!=']')return false;
            auto key=line.substr(1,line.size()-2);std::size_t offset{};
            const auto r=std::from_chars(key.data(),key.data()+key.size(),offset,16);
            if(r.ec!=std::errc{} || r.ptr!=key.data()+key.size() || offset<128 || offset%128 || !headers.emplace(offset,HeaderFields{}).second)return false;
            current=&headers.at(offset);continue;
        }
        if(!current)return false;
        const auto delimiter=line.find_first_of(":=");if(delimiter==std::string_view::npos)return false;
        const auto key=uppercase(trim(line.substr(0,delimiter)));auto value=line.substr(delimiter+1);
        while(!value.empty() && (value.front()==' ' || value.front()=='\t'))value.remove_prefix(1);
        if(!allowed.count(key) || !text_field(value,1024,true) || !current->emplace(key,std::string(value)).second)return false;
    }
    return true;
}
} // namespace

bool valid_qwk_preferences(const QwkPreferences& p,bool require_wrap_choice) noexcept {
    return static_cast<unsigned>(p.private_mail)<=static_cast<unsigned>(QwkPrivateMode::none) &&
        static_cast<unsigned>(p.encoding)<=static_cast<unsigned>(QwkEncoding::utf8) &&
        (p.encoding!=QwkEncoding::utf8 || p.full_headers) && (!require_wrap_choice || p.word_wrap.has_value());
}
bool valid_qwk_message_count(std::uint32_t n) noexcept { return n>=1 && n<=kQwkMaximumMessageCount; }
std::string canonical_qwk_board_id(std::string_view id) { return uppercase(id); }
std::string qwk_message_identifier(std::string_view board,std::uint32_t reference){
    if(!valid_qwk_board_id(board)||!reference||reference>kQwkMaximumReference)return {};
    return "<"+std::to_string(reference)+"."+canonical_qwk_board_id(board)+"@sfx.invalid>";
}
bool valid_qwk_board_id(std::string_view id) noexcept {
    if(id.empty() || id.size()>8 || upper(id.front())<'A' || upper(id.front())>'Z')return false;
    constexpr std::string_view punctuation="!#$%&'()-@^_`{}~";
    for(const char c:id)if(!((upper(c)>='A' && upper(c)<='Z') || (c>='0' && c<='9') || punctuation.find(c)!=std::string_view::npos))return false;
    const auto v=uppercase(id);
    if(v=="SYSOP" || v=="NETMAIL" || v=="CON" || v=="PRN" || v=="AUX" || v=="NUL" || v=="CLOCK$")return false;
    return !(v.size()==4 && (v.substr(0,3)=="COM" || v.substr(0,3)=="LPT") && v[3]>='1' && v[3]<='9');
}
std::string qwk_cp437_to_utf8(std::string_view text) {
    std::string out;out.reserve(text.size());
    for(const unsigned char byte:text) {
        const auto c=static_cast<std::uint32_t>(byte<128?byte:cp437_high[byte-128]);
        if(c<128)out+=static_cast<char>(c);
        else if(c<2048){out+=static_cast<char>(0xc0|(c>>6));out+=static_cast<char>(0x80|(c&63));}
        else {out+=static_cast<char>(0xe0|(c>>12));out+=static_cast<char>(0x80|((c>>6)&63));out+=static_cast<char>(0x80|(c&63));}
    }
    return out;
}
std::optional<std::string> qwk_utf8_to_cp437(std::string_view text) {
    std::string out;
    for(std::size_t i=0;i<text.size();) {
        const auto first=static_cast<unsigned char>(text[i++]);std::uint32_t value{};unsigned tails{};
        if(first<128){out+=static_cast<char>(first);continue;}
        if(first>=0xc2 && first<=0xdf){value=first&31;tails=1;}
        else if(first>=0xe0 && first<=0xef){value=first&15;tails=2;}
        else return {};
        if(tails>text.size()-i)return {};
        for(unsigned n=0;n<tails;++n){const auto c=static_cast<unsigned char>(text[i++]);if((c&0xc0)!=0x80)return {};value=(value<<6)|(c&63);}
        if((tails==1 && value<128) || (tails==2 && value<2048) || (value>=0xd800 && value<=0xdfff))return {};
        const auto found=std::find(cp437_high.begin(),cp437_high.end(),value);if(found==cp437_high.end())return {};
        out+=static_cast<char>(128+std::distance(cp437_high.begin(),found));
    }
    return out;
}

QwkCodecResult encode_qwk(const QwkPacketMetadata& meta,const QwkPreferences& preferences,
    const std::vector<QwkRecord>& records,std::uint32_t maximum_messages) {
    const auto fail=[](QwkCodecError e,std::string detail){return QwkCodecResult{e,std::move(detail),{}};};
    if(!valid_qwk_preferences(preferences) || !valid_qwk_message_count(maximum_messages))return fail(QwkCodecError::invalid_preferences,"Invalid packet preferences.");
    if(!valid_qwk_board_id(meta.board_id) || !text_field(meta.board_name,255) || !text_field(meta.caller_name,255) ||
        !text_field(meta.city,255,true) || !text_field(meta.phone,255,true) || !text_field(meta.sysop,255,true) ||
        meta.created.size()!=19 || meta.created[10]!=',' || meta.created[16]!=':' ||
        !date_time(meta.created.substr(0,6)+meta.created.substr(8,2),meta.created.substr(11,5)) ||
        !decimal(meta.created.substr(17,2),59) || meta.conferences.empty() || meta.conferences.size()>1001)return fail(QwkCodecError::invalid_metadata,"Invalid packet metadata.");
    if(records.empty() || records.size()>maximum_messages)return fail(QwkCodecError::limit_exceeded,"Packet message count is outside the configured limit.");
    std::set<std::uint16_t> conferences;
    for(const auto& c:meta.conferences)if(!text_field(c.name,255) || !conferences.insert(c.number).second)return fail(QwkCodecError::invalid_metadata,"Invalid conference mapping.");
    if(!conferences.count(0))return fail(QwkCodecError::invalid_metadata,"Missing separate private mail conference.");
    if(meta.netmail_conference&&(!*meta.netmail_conference||!conferences.count(*meta.netmail_conference)))return fail(QwkCodecError::invalid_metadata,"Invalid NetMail conference.");
    const auto board=canonical_qwk_board_id(meta.board_id);
    std::string control=meta.board_name+"\r\n"+meta.city+"\r\n"+meta.phone+"\r\n"+meta.sysop+"\r\n0,"+board+"\r\n"+meta.created+"\r\n"+meta.caller_name+"\r\n\r\n0\r\n"+std::to_string(records.size())+"\r\n"+std::to_string(meta.conferences.size()-1)+"\r\n";
    for(const auto& c:meta.conferences)control+=std::to_string(c.number)+"\r\n"+c.name+"\r\n";
    control+="\r\n\r\n\r\n";
    std::string messages(128,' ');field(messages,0,128,"Produced by Spitfire-X BBS");
    std::string headers,personal;std::map<std::uint16_t,std::string> indexes;
    std::uint64_t total=control.size();
    for(const auto& m:records) {
        if(!conferences.count(m.conference) || m.private_mail!=(m.conference==0||(meta.netmail_conference&&m.conference==*meta.netmail_conference)) || m.number==0 || m.number>kQwkMaximumReference || m.reference>99999999 ||
            !text_field(m.from,255) || !text_field(m.to,255) || !text_field(m.subject,71,true) || !date_time(m.date,m.time) ||
            !text_field(m.message_id,1024,true) || !text_field(m.in_reply_to,1024,true) || !text_field(m.when_written,64,true) || m.body.empty())return fail(QwkCodecError::invalid_record,"A complete message cannot be represented in this packet.");
        if(m.body.size()>kQwkExpandedBytes)return fail(QwkCodecError::limit_exceeded,"A complete message exceeds the expanded packet limit.");
        const bool deliberate_layout=m.artwork||m.body.find('\x1b')!=std::string::npos||
            std::any_of(m.body.begin(),m.body.end(),[](unsigned char c){return c>=0xb0&&c<=0xdf;});
        // Never split a generated SGR sequence or CP437 box/block layout as
        // though those bytes were an overlong prose line. This does not choose
        // the shipping wrap default or modify the stored original message.
        auto body=preferences.word_wrap.value() && !deliberate_layout?rewrap(m.body):m.body;
        if(body.back()!='\n')body+='\n';
        if(preferences.encoding==QwkEncoding::utf8)body=qwk_cp437_to_utf8(body);
        else {
            // Classic QWK reserves this byte for line breaks. Substitute only
            // content in the exported copy before encoding actual newlines.
            std::replace(body.begin(),body.end(),static_cast<char>(0xe3),'*');
            std::replace(body.begin(),body.end(),'\n',static_cast<char>(0xe3));
        }
        const auto offset=messages.size();const auto blocks=1+(body.size()+127)/128;
        if(blocks>999999 || offset/128+1>0x7fffff)return fail(QwkCodecError::limit_exceeded,"A complete message exceeds packet representation limits.");
        std::string header(128,' ');header[0]=m.private_mail?(m.read?'+':'*'):(m.read?'-':' ');
        field(header,1,7,std::to_string(m.number));field(header,8,8,m.date);field(header,16,5,m.time);
        field(header,21,25,m.to);field(header,46,25,m.from);field(header,71,25,m.subject);
        if(m.reference)field(header,108,8,std::to_string(m.reference));
        field(header,116,6,std::to_string(blocks));header[122]=static_cast<char>(0xe1);
        header[123]=static_cast<char>(m.conference&255);header[124]=static_cast<char>(m.conference>>8);
        header[125]=0;header[126]=0;
        if(preferences.full_headers) {
            const auto encoded=[&](const std::string& s){return preferences.encoding==QwkEncoding::utf8?qwk_cp437_to_utf8(s):s;};
            headers+='['+hex(offset)+"]\r\nUtf8: "+(preferences.encoding==QwkEncoding::utf8?"true":"false")+"\r\nSender: "+encoded(m.from)+"\r\nTo: "+encoded(m.to)+"\r\nSubject: "+encoded(m.subject)+"\r\n";
            if(!m.message_id.empty())headers+="Message-ID: "+m.message_id+"\r\n";
            if(!m.in_reply_to.empty())headers+="In-Reply-To: "+m.in_reply_to+"\r\n";
            if(!m.when_written.empty())headers+="WhenWritten: "+m.when_written+"\r\n";
            headers+="\r\n";
        }
        messages+=header;messages+=body;messages.append((128-body.size()%128)%128,' ');
        if(preferences.indexes){mbf_index(indexes[m.conference],static_cast<std::uint32_t>(offset/128+1),m.conference);if(m.private_mail || uppercase(m.to)==uppercase(meta.caller_name))mbf_index(personal,static_cast<std::uint32_t>(offset/128+1),m.conference);}
        if(messages.size()>kQwkExpandedBytes || headers.size()>kQwkExpandedBytes)return fail(QwkCodecError::limit_exceeded,"Expanded packet exceeds 64 MiB.");
    }
    QwkCodecResult result;
    result.files={{"CONTROL.DAT",std::move(control)},{"DOOR.ID","DOOR = Spitfire-X\r\nVERSION = " SFX_PRODUCT_VERSION_SHORT_STRING "\r\nSYSTEM = Spitfire-X\r\nCONTROLNAME = SFXQWK\r\nCONTROLTYPE = ADD\r\nCONTROLTYPE = DROP\r\nMIXEDCASE = YES\r\n"},{"MESSAGES.DAT",std::move(messages)}};
    if(meta.netmail_conference)result.files.push_back({"TOREADER.EXT","AREA 0 PL\r\nAREA "+std::to_string(*meta.netmail_conference)+" PN\r\n"});
    if(preferences.full_headers)result.files.push_back({"HEADERS.DAT",std::move(headers)});
    for(auto& pair:indexes){auto name=std::to_string(pair.first);if(name.size()<3)name.insert(0,3-name.size(),'0');result.files.push_back({name+".NDX",std::move(pair.second)});}
    if(!personal.empty())result.files.push_back({"PERSONAL.NDX",std::move(personal)});
    total=0;for(const auto& f:result.files){if(f.bytes.size()>kQwkExpandedBytes-total)return fail(QwkCodecError::limit_exceeded,"Expanded packet including metadata exceeds 64 MiB.");total+=f.bytes.size();}
    return result;
}

QwkReplyPacket decode_qwk_reply(std::string_view expected_board,const std::vector<QwkFile>& files) {
    const auto fail=[](QwkCodecError e,std::string detail){QwkReplyPacket r;r.error=e;r.detail=std::move(detail);return r;};
    if(!valid_qwk_board_id(expected_board))return fail(QwkCodecError::invalid_metadata,"Invalid configured board ID.");
    const auto board=canonical_qwk_board_id(expected_board);std::set<std::string> names;std::string_view data,raw_headers,raw_controls;std::uint64_t total{};
    for(const auto& f:files){const auto name=uppercase(f.name);if(!names.insert(name).second || (name!=board+".MSG" && name!="HEADERS.DAT" && name!="TODOOR.EXT"))return fail(QwkCodecError::unsupported_content,"Reply packet contains duplicate or unsupported files.");
        if(f.bytes.size()>kQwkExpandedBytes-total)return fail(QwkCodecError::limit_exceeded,"Expanded reply packet exceeds 64 MiB.");
        total+=f.bytes.size();
        if(name==board+".MSG")data=f.bytes;else if(name=="HEADERS.DAT")raw_headers=f.bytes;else raw_controls=f.bytes;
    }
    if(data.size()<128 || data.size()%128)return fail(QwkCodecError::invalid_record,"Reply message file is truncated or missing.");
    if(uppercase(trim(data.substr(0,128)))!=board)return fail(QwkCodecError::wrong_board,"Reply packet belongs to a different board.");
    Headers headers;if(!parse_headers(raw_headers,headers))return fail(QwkCodecError::invalid_headers,"Invalid or unsupported reply headers.");
    QwkReplyPacket result;result.board_id=board;result.logical_identity="SFX-QWK-REP-1;";identity_field(result.logical_identity,board);
    std::set<std::size_t> used_headers;
    for(std::size_t offset=128;offset<data.size();) {
        const auto header=data.substr(offset,128);const auto blocks=decimal(header.substr(116,6),999999);
        const auto conf=decimal(header.substr(1,7),65535);const auto ref_field=trim(header.substr(108,8));const auto ref=ref_field.empty()?std::optional<std::uint32_t>{0}:decimal(ref_field,99999999);
        const auto binary=static_cast<std::uint16_t>(static_cast<unsigned char>(header[123])|(static_cast<unsigned char>(header[124])<<8));
        if(!blocks || *blocks<2 || *blocks>(data.size()-offset)/128 || !conf || *conf!=binary || !ref ||
            (header[0]!=' ' && header[0]!='*') || static_cast<unsigned char>(header[122])!=0xe1 || !trim(header.substr(96,12)).empty() || header[127]!=' ' ||
            !date_time(header.substr(8,8),header.substr(16,5)))return fail(QwkCodecError::invalid_record,"Malformed or unsupported reply record.");
        QwkReplyRecord record;auto& m=record.message;m.conference=binary;m.reference=*ref;m.private_mail=header[0]=='*';
        m.date=std::string(header.substr(8,8));m.time=std::string(header.substr(16,5));
        m.from=std::string(trim(header.substr(46,25)));m.to=std::string(trim(header.substr(21,25)));m.subject=std::string(trim(header.substr(71,25)));
        bool utf8=false;
        const auto h=headers.find(offset);
        if(h!=headers.end()){
            used_headers.insert(offset);const auto& values=h->second;
            const auto flag=values.find("UTF8");if(flag!=values.end()){const auto f=uppercase(flag->second);if(f!="TRUE" && f!="FALSE")return fail(QwkCodecError::invalid_headers,"Invalid reply encoding flag.");utf8=f=="TRUE";}
            const auto copy=[&](const char* key,std::string& target)->bool{const auto value=values.find(key);if(value==values.end())return true;if(!utf8){target=value->second;return true;}const auto decoded=qwk_utf8_to_cp437(value->second);if(!decoded)return false;target=*decoded;return true;};
            if(!copy("SENDER",m.from) || !copy("TO",m.to) || !copy("SUBJECT",m.subject) || !copy("MESSAGE-ID",m.message_id) || !copy("IN-REPLY-TO",m.in_reply_to) || !copy("WHENWRITTEN",m.when_written))return fail(QwkCodecError::unsupported_content,"Reply uses characters outside SFX CP437 support.");
        }
        auto body=data.substr(offset+128,(static_cast<std::size_t>(*blocks)-1)*128);
        while(!body.empty() && (body.back()==' ' || body.back()=='\0'))body.remove_suffix(1);
        if(utf8){const auto decoded=qwk_utf8_to_cp437(body);if(!decoded)return fail(QwkCodecError::unsupported_content,"Reply uses characters outside SFX CP437 support.");m.body=*decoded;}
        else {m.body=std::string(body);std::replace(m.body.begin(),m.body.end(),static_cast<char>(0xe3),'\n');}
        // NCM 0.54 redundantly writes long fields into the body as well as
        // HEADERS.DAT. Strip only an exact, header-backed matching preamble;
        // ordinary authored text and unbacked kludges are never interpreted.
        if(h!=headers.end()){
            std::string preamble;
            if(m.from.size()>25)preamble+="From: "+m.from+'\n';
            if(m.to.size()>25)preamble+="To: "+m.to+'\n';
            if(m.subject.size()>25)preamble+="Subject: "+m.subject+'\n';
            if(!preamble.empty()){preamble+='\n';if(m.body.compare(0,preamble.size(),preamble)==0)m.body.erase(0,preamble.size());}
        }
        if(!text_field(m.to,255) || !text_field(m.from,255) || !text_field(m.subject,71,true) ||
            !text_field(m.message_id,1024,true) || !text_field(m.in_reply_to,1024,true))return fail(QwkCodecError::invalid_record,"Invalid reply recipient, subject or identifier.");
        if(uppercase(m.to)==kQwkControlName){
            const auto command=uppercase(m.subject);
            const bool blank=std::all_of(m.body.begin(),m.body.end(),[](char c){return c==' ' || c=='\t' || c=='\r' || c=='\n';});
            if((command!="ADD" && command!="DROP") || m.reference || !m.message_id.empty() || !m.in_reply_to.empty() || !blank || !m.conference)return fail(QwkCodecError::unsupported_command,"Only plain ADD and DROP are supported.");
            record.command=command=="ADD"?QwkScanCommand::add:QwkScanCommand::drop;
        }
        identity_field(result.logical_identity,std::to_string(m.conference));identity_field(result.logical_identity,std::to_string(m.reference));identity_field(result.logical_identity,m.private_mail?"private":"public");
        for(const auto* value:{&m.from,&m.to,&m.subject,&m.body,&m.date,&m.time,&m.message_id,&m.in_reply_to,&m.when_written})identity_field(result.logical_identity,*value);
        result.records.push_back(std::move(record));offset+=static_cast<std::size_t>(*blocks)*128;
    }
    // TOREADER.EXT makes QWKE readers use TODOOR.EXT for their existing
    // public ADD/DROP workflow. Normalize only those commands, not executable
    // instructions or arbitrary header changes. Empty author/date identifies
    // this codec-created control record; it can only change its actor's scans.
    if(raw_controls.size()>65536)return fail(QwkCodecError::limit_exceeded,"Offline controls exceed their limit.");
    std::istringstream controls{std::string(raw_controls)};std::string line;std::set<std::uint16_t> changed;
    while(std::getline(controls,line)){
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        if(line.empty())continue;
        std::istringstream words(line);std::string key,number,flags,extra;words>>key>>number>>flags;
        const auto conference=decimal(number,65535);
        if(line.size()>128||key!="AREA"||!conference||!*conference||(flags!="a"&&flags!="D")||(words>>extra)||!changed.insert(static_cast<std::uint16_t>(*conference)).second||changed.size()>999)
            return fail(QwkCodecError::unsupported_command,"Only public AREA add (a) and drop (D) controls are supported.");
        QwkReplyRecord r;r.command=flags=="a"?QwkScanCommand::add:QwkScanCommand::drop;auto& m=r.message;m.conference=static_cast<std::uint16_t>(*conference);m.to=std::string(kQwkControlName);m.subject=flags=="a"?"ADD":"DROP";
        identity_field(result.logical_identity,std::to_string(m.conference));identity_field(result.logical_identity,"0");identity_field(result.logical_identity,"public");
        for(const auto* value:{&m.from,&m.to,&m.subject,&m.body,&m.date,&m.time,&m.message_id,&m.in_reply_to,&m.when_written})identity_field(result.logical_identity,*value);
        result.records.push_back(std::move(r));
    }
    if(result.records.size()>kQwkExpandedBytes/256)return fail(QwkCodecError::limit_exceeded,"Reply record count exceeds its limit.");
    if(result.records.empty() || used_headers.size()!=headers.size())return fail(QwkCodecError::invalid_headers,"Reply packet is empty or contains orphan headers.");
    return result;
}
} // namespace sfx::application
