#include "sfx/application/door_drop.hpp"
#include "sfx/application/qwk.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace sfx::application {
namespace {
void need(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::string number(std::uint64_t n){return std::to_string(n);}
std::string clean(std::string value,std::size_t limit=255){
    for(auto& c:value)if(static_cast<unsigned char>(c)<32||c==127)c=' ';
    if(value.size()>limit)value.resize(limit);
    return value;
}
std::string legacy(std::string value){const auto converted=qwk_utf8_to_cp437(value);need(converted.has_value(),"door_drop_text_not_cp437");need(converted->size()<=255,"door_drop_legacy_text_too_long");return clean(*converted);}
std::string upper(std::string s){for(auto& c:s)if(c>='a'&&c<='z')c-=32;return s;}
std::pair<std::string,std::string> names(std::string s){auto at=s.find(' ');return {s.substr(0,at),at==s.npos?std::string{}:s.substr(at+1)};}
std::string with_separator(std::string value){if(!value.empty()&&value.back()!='/'&&value.back()!='\\')value+=value.find('\\')==value.npos?'/':'\\';return value;}
std::vector<std::uint8_t> lines(const std::vector<std::string>& values){
    std::vector<std::uint8_t> out;
    for(const auto& line:values){need(line.size()<=1024&&line.find_first_of("\r\n\0",0,3)==line.npos,"door_drop_line_invalid");out.insert(out.end(),line.begin(),line.end());out.push_back('\r');out.push_back('\n');}
    return out;
}
// Explicit little-endian records. No compiler packing or borrowed producer code.
struct Binary {
    std::vector<std::uint8_t> bytes;
    explicit Binary(std::size_t size):bytes(size){}
    void u16(std::size_t at,std::uint64_t n){need(n<=65535&&at+2<=bytes.size(),"door_drop_word_range");bytes[at]=std::uint8_t(n);bytes[at+1]=std::uint8_t(n>>8);}
    void u32(std::size_t at,std::uint64_t n){need(n<=UINT32_MAX&&at+4<=bytes.size(),"door_drop_dword_range");for(unsigned k=0;k<4;++k)bytes[at+k]=std::uint8_t(n>>(k*8));}
    void text(std::size_t at,std::size_t width,std::string s,bool spaces=false){need(at+width<=bytes.size(),"door_drop_text_range");s=clean(s);std::fill_n(bytes.begin()+at,width,std::uint8_t(spaces?' ':0));std::copy_n(s.begin(),std::min(s.size(),spaces?width:width-1),bytes.begin()+at);}
    void pascal(std::size_t at,std::size_t width,std::string s){s=clean(s);need(at+width+1<=bytes.size(),"door_drop_pascal_range");const auto n=std::min(s.size(),width);bytes[at]=std::uint8_t(n);std::copy_n(s.begin(),n,bytes.begin()+at+1);}
};
bool utf8_text(std::string_view value){
    if(value.empty()||value.size()>1024||value.front()==' '||value.back()==' ')return false;
    for(std::size_t i=0;i<value.size();){const auto first=static_cast<unsigned char>(value[i++]);std::uint32_t cp{};unsigned more{};
        if(first<128)cp=first;else if(first>=0xc2&&first<=0xdf){cp=first&31;more=1;}else if(first>=0xe0&&first<=0xef){cp=first&15;more=2;}else if(first>=0xf0&&first<=0xf4){cp=first&7;more=3;}else return false;
        const unsigned count=more;if(i+more>value.size())return false;while(more--){const auto b=static_cast<unsigned char>(value[i++]);if((b&0xc0)!=0x80)return false;cp=(cp<<6)|(b&63);}
        if((count==1&&cp<128)||(count==2&&cp<2048)||(count==3&&cp<65536)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)||cp<32||(cp>=127&&cp<=159))return false;
        if((i==count+1||i==value.size())&&(cp==0xa0||cp==0x1680||(cp>=0x2000&&cp<=0x200a)||cp==0x2028||cp==0x2029||cp==0x202f||cp==0x205f||cp==0x3000))return false;
    }return true;
}
std::string trim(std::string s){const auto a=s.find_first_not_of(" \t\r");if(a==s.npos)return {};return s.substr(a,s.find_last_not_of(" \t\r")-a+1);}
bool utc_deadline(std::string_view s){
    if(s.empty())return true;
    if(s.size()!=20||s[4]!='-'||s[7]!='-'||s[10]!='T'||s[13]!=':'||s[16]!=':'||s[19]!='Z')return false;
    const auto value=[&](unsigned at,unsigned count){unsigned result{};for(unsigned i=at;i<at+count;++i){if(s[i]<'0'||s[i]>'9')return 10000u;result=result*10+s[i]-'0';}return result;};
    const auto year=value(0,4),month=value(5,2),day=value(8,2);
    if(!year||year>9999||month<1||month>12||value(11,2)>23||value(14,2)>59||value(17,2)>59)return false;
    const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    return day>=1&&day<=days[month-1]+(month==2&&year%4==0&&(year%100!=0||year%400==0)?1:0);
}
// The producer uses ordinary language/script/region/variant tags. Extensions
// and private-use tags are not needed by this caller projection; reject them
// rather than emitting a malformed tag that a 1.0 consumer cannot use.
bool language_tag(std::string_view s){
    if(s.size()<2||s.size()>35)return false;
    const auto alpha=[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z');};
    std::vector<std::string_view> parts;std::size_t at{};
    for(;;){auto end=s.find('-',at);auto part=s.substr(at,end==s.npos?s.size()-at:end-at);if(part.empty()||part.size()>8)return false;parts.push_back(part);if(end==s.npos)break;at=end+1;}
    if(parts[0].size()<2||!std::all_of(parts[0].begin(),parts[0].end(),alpha))return false;
    std::size_t i=1;
    if(i<parts.size()&&parts[i].size()==4&&std::all_of(parts[i].begin(),parts[i].end(),alpha))++i;
    if(i<parts.size()&&((parts[i].size()==2&&std::all_of(parts[i].begin(),parts[i].end(),alpha))||(parts[i].size()==3&&std::all_of(parts[i].begin(),parts[i].end(),[](char c){return c>='0'&&c<='9';}))))++i;
    std::map<std::string,bool> variants;
    for(;i<parts.size();++i){const auto part=parts[i];if(part.size()<5&&!(part.size()==4&&part[0]>='0'&&part[0]<='9'))return false;
        if(!std::all_of(part.begin(),part.end(),[&](unsigned char c){return alpha(c)||(c>='0'&&c<='9');})||!variants.emplace(upper(std::string(part)),true).second)return false;}
    return true;
}

std::string ini_text(std::string_view source, bool utf8, std::size_t limit) {
    const auto first_text = source.find_first_not_of(" \t");
    if (first_text == source.npos) return {};
    source = source.substr(first_text, source.find_last_not_of(" \t") - first_text + 1);
    std::string out;
    while (!source.empty()) {
        const auto lead = static_cast<unsigned char>(source.front());
        std::size_t count = lead < 0x80 ? 1 : lead >= 0xc2 && lead <= 0xdf ? 2 :
            lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
        std::uint32_t point = count == 1 ? lead : lead & (0x7fu >> count);
        bool valid = count != 0 && source.size() >= count;
        for (std::size_t i = 1; valid && i < count; ++i) {
            const auto byte = static_cast<unsigned char>(source[i]);
            valid = (byte & 0xc0) == 0x80;
            point = (point << 6) | (byte & 0x3f);
        }
        constexpr std::uint32_t minimum[]{0, 0, 0x80, 0x800, 0x10000};
        valid = valid && point >= minimum[count] && point <= 0x10ffff &&
            !(point >= 0xd800 && point <= 0xdfff);
        if (!valid) count = 1;
        std::string encoded = "?";
        if (valid && point == 0xa0) encoded = " ";
        else if (valid && point >= 32 && !(point >= 127 && point <= 159) &&
                 point != 0x2028 && point != 0x2029 &&
                 !(point >= 0x202a && point <= 0x202e) &&
                 !(point >= 0x2066 && point <= 0x2069)) {
            const auto scalar = source.substr(0, count);
            encoded = utf8 ? std::string(scalar) : qwk_utf8_to_cp437(scalar).value_or("?");
        }
        if (out.empty() && encoded == " ") { source.remove_prefix(count); continue; }
        if (encoded.size() > limit - out.size()) break;
        out += encoded;
        source.remove_prefix(count);
    }
    return trim(std::move(out));
}

DoorDropResult make_doordrop_ini(const DoorDropContext& c) {
    constexpr std::uint64_t max_int = 2147483647;
    const auto alphanumeric = [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
    };
    need(c.account <= max_int && !c.user_key.empty() && c.user_key.size() <= 64 &&
        alphanumeric(c.user_key.front()) && std::all_of(c.user_key.begin(),c.user_key.end(),[&](char ch) {
            return alphanumeric(ch) || ch == '-' || ch == '_' || ch == '.';
        }), "door_ini_identity_invalid");
    const auto& time = c.file_time;
    need((time.size() == 19 || time.size() == 25) && utc_deadline(time.substr(0,19)+"Z"), "door_ini_time_invalid");
    if (time.size() == 25) {
        need((time[19] == '+' || time[19] == '-') && time[22] == ':' &&
            time[20] >= '0' && time[20] <= '2' && time[21] >= '0' && time[21] <= '9' &&
            time.substr(20,2) <= "23" && time[23] >= '0' && time[23] <= '5' && time[24] >= '0' && time[24] <= '9',
            "door_ini_time_offset_invalid");
    }
    need(!c.software_version.empty(),"door_ini_version_unavailable");
    const auto charset = c.encoding == "IBM437" ? "CP437" : c.encoding;
    need(charset == "CP437" || charset == "UTF-8" || charset == "US-ASCII", "door_ini_charset_unavailable");
    need(!c.remaining_seconds || *c.remaining_seconds <= max_int,"door_ini_time_left_range");
    std::string path;
    if(c.path_style==DoorDropPathStyle::posix){
        need(!c.dos&&!c.drop_path.empty()&&c.drop_path.front()=='/'&&
            c.drop_path.find_first_of("\\\r\n\0",0,4)==c.drop_path.npos,"door_ini_path_invalid");
        path=c.drop_path+(c.drop_path.back()=='/'?"":"/")+"DOORDROP.INI";
    }else{
        need(c.path_style==DoorDropPathStyle::windows,"door_ini_path_invalid");
        path=with_separator(c.drop_path)+"DOORDROP.INI";
        need(c.drop_path.size()>=3&&c.drop_path[1]==':'&&(c.drop_path[2]=='\\'||c.drop_path[2]=='/')&&
            path.find_first_of("\r\n\0",0,3)==path.npos,"door_ini_path_invalid");
    }

    std::vector<std::string> rows{"[file]"};
    const auto field = [&](std::string_view key, std::string_view value) {
        need(key.size() <= 32 && key.size() + value.size() + 1 <= 255, "door_ini_line_range");
        rows.push_back(std::string(key) + "=" + std::string(value));
    };
    const auto text = [&](std::string_view key, std::string_view value, bool required = false) {
        auto encoded = ini_text(value,c.doordrop_utf8,254-key.size());
        if (!encoded.empty()) field(key,encoded);
        else if (required) field(key,"?");
    };
    if (c.doordrop_utf8) field("FILE_UTF8","1");
    field("FILE_TIME",time);
    rows.emplace_back("[system]");
    text("SYS_SOFTWARE",c.software+" "+c.software_version,true);
    text("SYS_NAME",c.board,true);
    text("SYS_OP",c.sysop,true);
    field("SYS_NODE_NUM",number(c.node));
    field("SYS_NODE_COUNT",number(c.nodes));
    if (valid_qwk_board_id(c.board_code) && canonical_qwk_board_id(c.board_code)==c.board_code &&
        !(c.board_code.front()>='0' && c.board_code.front()<='9')) field("SYS_QWKID",c.board_code);
    field("SYS_VENDOR","SFX");
    text("SYS_VERSION",c.software_version);
    rows.emplace_back("[comm]");
    field("COMM_CHARSET",charset);
    switch (c.io) {
        case DoorIo::standard: field("COMM_TYPE","stdio"); break;
        case DoorIo::socket:
            need(!c.dos && c.socket != 0 && c.socket != UINT64_MAX,"door_ini_socket_invalid");
            field("COMM_TYPE","socket"); field("COMM_HANDLE",number(c.socket)); break;
        case DoorIo::fossil_or_uart:
        case DoorIo::fossil_only:
            need(c.dos,"door_ini_fossil_invalid");
            field("COMM_TYPE","fossil"); field("COMM_PORT","1"); break;
        case DoorIo::uart_only:
            need(c.dos,"door_ini_uart_invalid");
            field("COMM_TYPE","uart"); field("COMM_PORT","1");
            field("UART_BASE","1016"); field("UART_IRQ","4"); break;
        default: need(false,"door_ini_io_invalid");
    }
    rows.emplace_back("[user]");
    text("USER_ALIAS",c.alias,true);
    field("USER_NUMBER",number(c.account));
    field("USER_KEY",c.user_key);
    field("USER_ROLE",c.sysop_access?"sysop":"user");
    rows.emplace_back("[terminal]");
    field("TERM_TYPE",c.ansi?"ansi":"dumb");
    field("TERM_COLS",number(c.columns));
    field("TERM_ROWS",number(c.rows));
    if (c.remaining_seconds) {
        rows.emplace_back("[session]"); field("TIME_LEFT",number(*c.remaining_seconds));
    }
    if (!c.door_code.empty() || !c.door_name.empty()) {
        rows.emplace_back("[door]"); text("DOOR_CODE",c.door_code); text("DOOR_NAME",c.door_name);
    }
    rows.emplace_back("[x-sfx]");
    field("X_SFX_LEVEL",number(c.security));
    return {{{"DOORDROP.INI",lines(rows)}},{{"DOORDROP_INI",path}}, {}};
}
}

DoorDropResult make_door_drop_files(DoorDropFile format,const DoorDropContext& c){
    DoorDropResult result;
    try{
        need(static_cast<unsigned>(format)<=static_cast<unsigned>(DoorDropFile::doordrop_ini),"door_drop_format_invalid");
        if(format==DoorDropFile::none)return result;
        need(c.account&&c.node>=1&&c.node<=128&&c.nodes>=c.node&&c.nodes<=128&&c.security>=1&&c.security<=999&&c.columns&&c.rows,"door_drop_context_invalid");
        if (format == DoorDropFile::doordrop_ini) return make_doordrop_ini(c);
        need(utf8_text(c.alias)&&utf8_text(c.board)&&utf8_text(c.sysop),"door_drop_identity_invalid");
        const auto seconds=std::min<std::uint64_t>(c.remaining_seconds.value_or(32767ull*60),32767);
        const auto minutes=std::min<std::uint64_t>(c.remaining_seconds.value_or(32767ull*60)/60,32767);
        const auto name=format==DoorDropFile::bbsdev_drp?c.alias:legacy(c.alias), board=format==DoorDropFile::bbsdev_drp?c.board:legacy(c.board), sysop=format==DoorDropFile::bbsdev_drp?c.sysop:legacy(c.sysop);
        const auto first=names(name), op=names(sysop);
        const auto node=with_separator(format==DoorDropFile::bbsdev_drp?c.node_path:legacy(c.node_path)), drop=with_separator(format==DoorDropFile::bbsdev_drp?c.drop_path:legacy(c.drop_path)), startup=with_separator(format==DoorDropFile::bbsdev_drp?c.startup_path:legacy(c.startup_path));
        const bool serial=c.dos&&c.io!=DoorIo::standard;
        const std::string com=serial?"1":"0", rate=serial?"38400":"115200";
        const auto add=[&](const char* filename,std::vector<std::string> fields){result.files.push_back({filename,lines(fields)});};
        if(format==DoorDropFile::door32_sys){
            need(!c.dos,"door32_requires_native_program");
            add("DOOR32.SYS",{c.io==DoorIo::socket?"2":"0",c.io==DoorIo::socket?number(c.socket):"0",rate,legacy(c.software),number(c.account),name,name,number(c.security),number(minutes),c.ansi?"1":"0",number(c.node)});
        }else if(format==DoorDropFile::bbsdev_drp){
            need(utf8_text(c.user_key)&&utf8_text(c.software),"bbsdev_identity_invalid");
            need(c.encoding=="IBM437"||c.encoding=="UTF-8"||c.encoding=="US-ASCII","bbsdev_encoding_invalid");
            need(language_tag(c.language),"bbsdev_language_invalid");
            std::string mode="stdio",parameter;
            if(c.io==DoorIo::socket){need(!c.dos,"bbsdev_socket_requires_native");mode="socket";parameter=number(c.socket);}
            else if(c.dos&&c.io==DoorIo::uart_only){mode="uart";parameter="03F8,4";}
            else if(c.dos&&c.io!=DoorIo::standard){mode="fossil";parameter="0";}
            need(utc_deadline(c.deadline_utc),"bbsdev_deadline_invalid");
            add("BBSDEV.DRP",{"1.0",mode,parameter,c.alias,c.user_key,number(c.columns),number(c.rows),c.ansi?"Y":"N","N","",c.deadline_utc,c.encoding,c.language,c.software,c.board,c.sysop,number(c.security),number(c.node),"Y"});
            need(!c.drop_path.empty()&&(c.drop_path[0]=='/'||(c.drop_path.size()>2&&c.drop_path[1]==':')),"bbsdev_absolute_path_required");
            result.environment["BBSDEV_DRP"]=with_separator(c.drop_path)+"BBSDEV.DRP";
        }else if(format==DoorDropFile::door_sys31||format==DoorDropFile::door_sys52){
            std::vector<std::string> f(52);
            const auto put=[&](unsigned line,std::string value){f.at(line-1)=std::move(value);};
            put(1,"COM"+com+":");put(2,rate);put(3,"8");put(4,number(c.node));put(5,rate);
            for(unsigned line=6;line<=9;++line)put(line,line==6?"Y":"N");
            put(10,name);put(15,number(c.security));put(16,"0");put(17,c.date);put(18,number(seconds));put(19,number(minutes));put(20,c.ansi?"GR":"NG");put(21,number(c.rows));put(22,"N");put(24,"0");put(25,"12/31/99");put(26,number(c.account));put(27,"Z");
            for(unsigned line=28;line<=31;++line)put(line,"0");
            if(format==DoorDropFile::door_sys52){put(33,node);put(34,node);put(35,sysop);put(36,name);put(37,"00:00");put(38,"Y");put(39,"N");put(40,"Y");put(41,"7");put(42,"0");put(43,c.date);put(44,c.logon_clock);put(45,c.logon_clock);for(unsigned line=46;line<=49;++line)put(line,"0");put(51,"0");put(52,"0");}
            add("DOOR.SYS",std::move(f)); // 31-field layout retains its 21 empty compatibility lines.
        }else if(format==DoorDropFile::sfdoors_dat){
            add("SFDOORS.DAT",{number(c.account),name,"",first.first,rate,com,number(minutes),number(c.seconds_today),drop,c.ansi?"TRUE":"FALSE",number(c.security),"0","0",number(minutes),number(c.logon_seconds_today),"0","FALSE","FALSE","FALSE",rate,"TRUE","0","0",number(c.node),"0","0","0","0","0","0","",""});
        }else if(format==DoorDropFile::doorfile_sr){
            add("DOORFILE.SR",{name,c.ansi?"1":"0","1",number(c.rows),rate,com,number(minutes),name});
        }else if(format==DoorDropFile::tribbs_sys){
            add("TRIBBS.SYS",{number(c.account),name,"",number(c.security),"N",c.ansi?"Y":"N",number(minutes),"","","",number(c.node),com,rate,rate,"Y","Y",board,sysop,name,"N"});
        }else if(format==DoorDropFile::chain_txt){
            add("CHAIN.TXT",{number(c.account),upper(name),upper(name),"","0","?","0",c.date,number(c.columns),number(c.rows),number(c.security),"0",c.sysop_access?"1":"0",c.ansi?"1":"0","1",number(seconds),node,node,"node.log",rate,com,board,sysop,number(c.logon_seconds_today),number(c.elapsed_seconds),"0","0","0","0","8N1"});
        }else if(format==DoorDropFile::callinfo_bbs){
            add("CALLINFO.BBS",{name,serial?"6":"7","",number(c.security),number(minutes),c.ansi?"COLOR":"MONO","",number(c.account),number(seconds),c.clock,c.clock+" "+c.date,"","0","0","0","0","",c.date+" "+c.clock,"NOVICE","All",c.date,"0",number(c.rows),"0","0","0","8","REMOTE","COM"+com,"",rate,"FALSE","Normal Connection",c.date+" "+c.clock,number(c.node),"0"});
        }else if(format==DoorDropFile::xtrn_dat){
            add("XTRN.DAT",{name,board,sysop,sysop,node,node,number(c.nodes),number(c.node),number(c.remaining_seconds.value_or(32767ull*60)),c.ansi?"Yes":"No",number(c.rows),"0",number(c.security),number(c.security),"","?",number(c.account),"",com,serial?"4":"0",serial?"3f8":"0",rate,"Yes","Yes","","","","","","","0","0","","","","","0","","","","","","0",name,rate,startup,node,drop,legacy(c.board_code),"0",c.io==DoorIo::socket?number(c.socket):"-1"});
        }else if(format==DoorDropFile::dorinfo_node||format==DoorDropFile::dorinfo1_def){
            need(format!=DoorDropFile::dorinfo_node||!c.dos||c.node<=15,"door_dorinfo_node_exceeds_dos_filename_use_dorinfo1");
            std::ostringstream file;file<<"DORINFO"<<std::uppercase<<std::hex<<(format==DoorDropFile::dorinfo1_def?1:c.node)<<".DEF";
            add(file.str().c_str(),{upper(board),upper(op.first),upper(op.second),"COM"+com,rate+" BAUD,N,8,1","0",upper(first.first),upper(first.second),"",c.ansi?"1":"0",number(c.security),number(minutes)});
            // QBBS 2.90 903-byte EXITINFO companion; Pascal strings and LE scalars.
            // Offsets are checked against the pinned QuickBBS packed definitions.
            need(c.account<=32767,"door_exitinfo_account_range");
            Binary exit(903);exit.u16(0,serial?38400:0);exit.pascal(170,8,c.date);
            exit.pascal(241,35,name);exit.pascal(277,25,"");exit.pascal(345,5,c.logon_clock);exit.pascal(351,8,c.date);
            exit.bytes[360]=c.ansi?8:0;exit.u16(373,c.security);exit.u16(387,std::min<std::uint32_t>(c.elapsed_seconds/60,32767));exit.u16(389,c.rows);
            exit.pascal(420,5,c.logon_clock);exit.pascal(426,8,c.logon_date);exit.u16(435,minutes);exit.u16(445,c.account);exit.bytes[644]=1;exit.bytes[645]=1;exit.bytes[646]=1;exit.u16(648,c.rows);exit.bytes[650]=1;exit.bytes[701]=c.ansi?1:0;
            result.files.push_back({"EXITINFO.BBS",std::move(exit.bytes)});
        }else if(format==DoorDropFile::pcboard_sys){
            // PCBoard 15.3 SYS (144 bytes) and USERS.SYS fixed record (218 bytes).
            need(c.account<=65535,"door_pcboard_account_range");
            Binary pcb(144);for(unsigned at:{0u,2u,4u,6u,9u,117u,119u})pcb.text(at,2," 0",true);
            pcb.text(0,2,"-1",true);pcb.text(9,2,"-1",true);pcb.bytes[8]=' ';pcb.bytes[11]=c.ansi?'Y':'N';pcb.bytes[12]='U';pcb.text(13,5,rate,true);pcb.text(18,5,rate,true);
            pcb.u16(23,c.account);pcb.text(25,15,first.first,true);pcb.text(40,12,"",true);pcb.u16(52,c.logon_seconds_today/60);pcb.text(56,5,c.logon_clock,true);pcb.u16(61,minutes);pcb.u16(63,32767);pcb.text(80,4,"",true);pcb.text(84,25,name,true);pcb.u16(109,minutes);pcb.bytes[111]=std::uint8_t(c.node);pcb.text(112,5,"00:00",true);pcb.bytes[125]=serial?'1':'0';pcb.bytes[126]=' ';pcb.bytes[128]=c.ansi?1:0;pcb.bytes[133]='Y';pcb.bytes[134]='N';
            const auto elapsed_minutes=std::min<std::uint32_t>(c.elapsed_seconds/60,32767);
            pcb.u16(54,static_cast<std::uint16_t>(-static_cast<std::int32_t>(elapsed_minutes)));
            result.files.push_back({"PCBOARD.SYS",std::move(pcb.bytes)});
            Binary user(40+218);user.u16(0,1530);user.u32(2,c.account);user.u16(6,218);user.u16(8,1);user.text(40,26,name);user.bytes[40+101]='Z';user.u16(40+105,c.security);user.bytes[40+109]=std::uint8_t(std::min<unsigned>(c.rows,255));user.u32(40+197,c.account);
            user.u16(40+180,elapsed_minutes);
            result.files.push_back({"USERS.SYS",std::move(user.bytes)});
        }
        need(!result.files.empty(),"door_drop_writer_unavailable");
    }catch(const std::exception& e){result.files.clear();result.environment.clear();result.diagnostic=e.what();}
    return result;
}

std::optional<DoorDriverSettings> parse_door_driver_settings(std::string_view shared,std::string_view startup,std::string* diagnostic){
    try{
        DoorDriverSettings result;
        for(const auto source:{shared,startup}){
            need(source.size()<=16384,"door_ini_too_large");std::istringstream stream{std::string(source)};std::string line,section;std::map<std::string,bool> seen;
            while(std::getline(stream,line)){
                line=trim(line);if(line.empty()||line.front()==';'||line.front()=='#')continue;
                need(line.find('\0')==line.npos,"door_ini_nul");
                if(line.front()=='['){need(line.back()==']',"door_ini_section");section=upper(trim(line.substr(1,line.size()-2)));need(section=="SFXEXEC"||section=="RUNTIME","door_ini_unknown_section");continue;}
                auto equals=line.find('=');need(equals!=line.npos,"door_ini_assignment");const auto key=upper(trim(line.substr(0,equals)));auto value=trim(line.substr(equals+1));const auto comment=value.find(';');if(comment!=value.npos)value=trim(value.substr(0,comment));
                need(seen.emplace(section+":"+key,true).second,"door_ini_duplicate_key");
                if(key=="YIELDINTERVAL"&&(section.empty()||section=="SFXEXEC")){
                    need(!value.empty()&&std::all_of(value.begin(),value.end(),[](char c){return (c>='0'&&c<='9')||c=='.';}),"door_ini_yield_invalid");
                    std::istringstream number_stream(value);number_stream.imbue(std::locale::classic());double n{};number_stream>>n;need(number_stream&&!number_stream.rdbuf()->in_avail()&&std::isfinite(n)&&n>=0&&n<=1000,"door_ini_yield_range");result.yield_interval_ms=n;
                }else if((key=="CANDISCONNECT"&&(section.empty()||section=="SFXEXEC"))||(key=="ALLOWX64DOS"&&section=="RUNTIME")){
                    value=upper(value);need(value=="TRUE"||value=="FALSE","door_ini_boolean");if(key=="CANDISCONNECT")result.can_disconnect=value=="TRUE";else result.allow_x64_dos=value=="TRUE";
                }else if(key=="DOSINPUTINTERVALMS"&&(section.empty()||section=="SFXEXEC")){
                    unsigned interval{};const auto parsed=std::from_chars(value.data(),value.data()+value.size(),interval);
                    need(!value.empty()&&parsed.ec==std::errc{}&&parsed.ptr==value.data()+value.size()&&interval<=100,"door_ini_dos_input_interval_range");
                    result.dos_input_interval_ms=interval;
                }else need(false,"door_ini_unknown_key");
            }
        }return result;
    }catch(const std::exception& e){if(diagnostic)*diagnostic=e.what();return {};}
}
std::string encode_door_driver_settings(const DoorDriverSettings& settings){
    std::ostringstream out;out.imbue(std::locale::classic());out<<"YieldInterval="<<std::setprecision(8)<<settings.yield_interval_ms<<"\r\nCanDisconnect="<<(settings.can_disconnect?"true":"false")<<"\r\n[UART]\r\nComPort=1\r\n";return out.str();
}
} // namespace sfx::application
