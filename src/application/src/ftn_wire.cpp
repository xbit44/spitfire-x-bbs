#include "sfx/application/ftn_wire.hpp"
#include <algorithm>
#include <limits>
#include <set>

namespace sfx::application {
namespace {
bool decimal(std::string_view s,std::uint64_t& value){
    if(s.empty()||s.size()>20)return false;
    value=0;
    for(char c:s){if(c<'0'||c>'9'||value>(UINT64_MAX-static_cast<unsigned>(c-'0'))/10)return false;value=value*10+static_cast<unsigned>(c-'0');}return true;
}
std::vector<std::string_view> words(std::string_view s){
    std::vector<std::string_view> out;
    while(!s.empty()){while(!s.empty()&&s.front()==' ')s.remove_prefix(1);if(s.empty())break;if(out.size()==128)return {};const auto end=s.find(' ');out.push_back(s.substr(0,end));if(end==s.npos)break;s.remove_prefix(end+1);}
    return out;
}
std::string fold(std::string_view s){std::string out(s);for(auto& c:out)if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');return out;}
int hex(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
bool command_text(std::string_view s){for(unsigned char c:s)if(c<32||c==127)return false;return true;}
std::uint16_t word(std::string_view s,std::size_t at){return static_cast<std::uint16_t>(static_cast<unsigned char>(s[at])|(static_cast<unsigned char>(s[at+1])<<8));}
void word_at(std::string& s,std::size_t at,std::uint16_t n){s[at]=static_cast<char>(n&255);s[at+1]=static_cast<char>(n>>8);}
void push_word(std::string& s,std::uint16_t n){s+=static_cast<char>(n&255);s+=static_cast<char>(n>>8);}
bool field_text(std::string_view s,std::size_t maximum,bool blank=false){if(s.size()>maximum||(!blank&&s.empty()))return false;for(unsigned char c:s)if(c<32||c==127||c==0x9b)return false;return true;}
bool packet_time(const FtnPacketHeader& h){return h.year>=1900&&h.year<=9999&&h.month<12&&h.day>=1&&h.day<=31&&h.hour<24&&h.minute<60&&h.second<60;}
bool read_z(std::string_view bytes,std::size_t& at,std::string& out,std::size_t maximum){
    if(at>=bytes.size())return false;
    const auto end=bytes.find('\0',at);if(end==bytes.npos||end-at>maximum)return false;out.assign(bytes.substr(at,end-at));at=end+1;return true;
}
bool packed_fields(const FtnPackedMessage& m){return field_text(m.date,19)&&field_text(m.to,35,true)&&field_text(m.from,35)&&field_text(m.subject,71,true)&&m.text.size()<=kFtnMessageMaximumBytes&&m.text.find('\0')==m.text.npos;}
void erase_frame_text(std::string& value)noexcept{volatile char* bytes=value.empty()?nullptr:&value[0];for(std::size_t i=0;i<value.size();++i)bytes[i]=0;value.clear();}
}

BinkpDecoder::~BinkpDecoder(){erase_frame_text(payload_);}
bool BinkpDecoder::feed(std::string_view bytes,const std::function<bool(BinkpFrame)>& receive){
    if(failed_)return false;
    while(!bytes.empty()){
        if(header_used_<2){header_[header_used_++]=static_cast<std::uint8_t>(bytes.front());bytes.remove_prefix(1);if(header_used_!=2)continue;
            command_=(header_[0]&0x80)!=0;expected_=((header_[0]&0x7fU)<<8)|header_[1];used_=0;payload_.clear();
            if((command_&&!expected_)||(command_&&expected_>kBinkpCommandMaximumBytes)){failed_=true;return false;}
            if(!expected_){header_used_=0;if(!receive({false,0,{}})){failed_=true;return false;}continue;}
        }
        const auto count=std::min(expected_-used_,bytes.size());payload_.append(bytes.substr(0,count));bytes.remove_prefix(count);used_+=count;
        if(used_==expected_){BinkpFrame frame;frame.command=command_;if(command_){frame.id=static_cast<std::uint8_t>(payload_.front());frame.bytes=payload_.substr(1);erase_frame_text(payload_);
                if(!frame.bytes.empty()&&frame.bytes.back()=='\0')frame.bytes.pop_back();
                if(!command_text(frame.bytes)){erase_frame_text(frame.bytes);failed_=true;return false;}}
            else frame.bytes=std::move(payload_);
            header_used_=used_=expected_=0;if(!receive(std::move(frame))){failed_=true;return false;}
        }
    }return true;
}
std::string binkp_command(BinkpCommand command,std::string_view argument){
    if(argument.size()+1>kBinkpCommandMaximumBytes||!command_text(argument))return {};
    const auto size=argument.size()+1;std::string out;out+=static_cast<char>(0x80|(size>>8));out+=static_cast<char>(size&255);out+=static_cast<char>(command);out.append(argument);return out;
}
std::string binkp_data(std::string_view bytes){
    if(bytes.size()>kBinkpFrameMaximumBytes)return {};
    std::string out;out+=static_cast<char>(bytes.size()>>8);out+=static_cast<char>(bytes.size()&255);out.append(bytes);return out;
}
bool ftn_message_wire_name(std::string_view name)noexcept{
    if(!valid_ftn_leaf_name(name)||name.size()<=4)return false;
    const auto lower=[](char c){return c>='A'&&c<='Z'?static_cast<char>(c+32):c;};
    const auto suffix=name.substr(name.size()-4);
    if(suffix[0]!='.')return false;
    if(lower(suffix[1])=='p'&&lower(suffix[2])=='k'&&lower(suffix[3])=='t')return true;
    const auto sequence=lower(suffix[3]);
    if(!((sequence>='0'&&sequence<='9')||(sequence>='a'&&sequence<='z')))return false;
    for(const auto day:{"su","mo","tu","we","th","fr","sa"})
        if(lower(suffix[1])==day[0]&&lower(suffix[2])==day[1])return true;
    return false;
}
bool valid_ftn_leaf_name(std::string_view s)noexcept{
    if(s.empty()||s.size()>128||s.front()==' '||s.back()==' '||s.back()=='.'||s=="."||s=="..")return false;
    for(unsigned char c:s)if(c<32||c>=127||std::string_view("<>:\"/\\|?*").find(static_cast<char>(c))!=s.npos)return false;
    auto stem=s.substr(0,s.find('.'));while(!stem.empty()&&stem.back()==' ')stem.remove_suffix(1);
    const auto equal=[&](std::string_view b){if(stem.size()!=b.size())return false;for(std::size_t i=0;i<b.size();++i){auto c=stem[i];if(c>='a'&&c<='z')c=static_cast<char>(c-'a'+'A');if(c!=b[i])return false;}return true;};
    for(const auto name:{"CON","PRN","AUX","NUL","CLOCK$","CONIN$","CONOUT$"})if(equal(name))return false;
    if(stem.size()==4&&stem[3]>='1'&&stem[3]<='9'){
        const auto prefix=stem.substr(0,3);char a=prefix[0],b=prefix[1],c=prefix[2];if(a>='a'&&a<='z')a-=32;if(b>='a'&&b<='z')b-=32;if(c>='a'&&c<='z')c-=32;
        if((a=='C'&&b=='O'&&c=='M')||(a=='L'&&b=='P'&&c=='T'))return false;
    }return true;
}
std::optional<std::string> decode_binkp_name(std::string_view s){
    if(s.size()>512)return {};
    std::string out;
    for(std::size_t i=0;i<s.size();){char c=s[i++];if(c=='\\'){if(i<s.size()&&(s[i]=='x'||s[i]=='X'))++i;if(s.size()-i<2||hex(s[i])<0||hex(s[i+1])<0)return {};c=static_cast<char>((hex(s[i])<<4)|hex(s[i+1]));i+=2;}out+=c;}
    return valid_ftn_leaf_name(out)?std::optional<std::string>(std::move(out)):std::nullopt;
}
std::string encode_binkp_name(std::string_view s){
    if(!valid_ftn_leaf_name(s))return {};
    std::string out;
    for(unsigned char c:s){if(c==' '||c=='\\'){out+="\\x";out+="0123456789abcdef"[c>>4];out+="0123456789abcdef"[c&15];}else out+=static_cast<char>(c);}return out;
}
std::optional<BinkpFile> parse_binkp_file(std::string_view s,bool offset,bool v11){
    if(s.size()>1024)return {};
    const auto w=words(s);if(w.size()!=(offset?4U:3U))return {};const auto name=decode_binkp_name(w[0]);BinkpFile f;
    if(!name||!decimal(w[1],f.size)||f.size>kFtnTransferMaximumBytes||!decimal(w[2],f.timestamp)||f.timestamp>UINT32_MAX)return {};
    f.name=*name;if(offset){if(w[3]=="-1"){if(!v11)return {};f.request_offset=true;}else if(!decimal(w[3],f.offset)||f.offset>f.size)return {};}
    return f;
}
std::string format_binkp_file(const BinkpFile& f,bool offset){
    const auto name=encode_binkp_name(f.name);if(name.empty()||f.size>kFtnTransferMaximumBytes||f.timestamp>UINT32_MAX||f.offset>f.size)return {};
    return name+" "+std::to_string(f.size)+" "+std::to_string(f.timestamp)+(offset?" "+(f.request_offset?std::string("-1"):std::to_string(f.offset)):std::string{});
}
bool same_binkp_file(const BinkpFile& a,const BinkpFile& b)noexcept{return a.name==b.name&&a.size==b.size&&a.timestamp==b.timestamp;}
std::optional<std::vector<FtnAddress>> parse_binkp_addresses(std::string_view s){
    if(s.empty()||s.size()>kBinkpCommandMaximumBytes)return {};
    const auto w=words(s);if(w.empty()||w.size()>64)return {};std::set<std::string> seen;std::vector<FtnAddress> out;
    for(const auto value:w){const auto address=parse_ftn_address(value);if(!address||!seen.insert(format_ftn_address(*address)).second)return {};out.push_back(*address);}return out;
}
std::string binkp_advertised_system(std::string_view information) {
    if(information.substr(0,4)!="SYS ")return {};
    auto name=information.substr(4);
    const auto first=name.find_first_not_of(' ');
    if(first==name.npos)return {};
    name=name.substr(first,128);
    std::string out;
    out.reserve(name.size());
    for(const unsigned char ch:name)out+=ch>=32&&ch<=126?static_cast<char>(ch):'?';
    while(!out.empty()&&out.back()==' ')out.pop_back();
    return out;
}
bool observe_binkp_information(BinkpOptions& o,std::string_view s,bool handshake){
    if(!handshake)return true;
    const auto w=words(s);if(w.empty())return true;
    if(w[0]=="VER")for(const auto token:w){const auto version=fold(token);if(version=="binkp/1.1"||version=="(binkp/1.1)")o.version11=true;}
    if(w[0]=="OPT")for(const auto token:w){if(token=="NR")o.request_nr=true;
        // Some mailers advertise CRAM-MD5 support without issuing a challenge.
        if(token=="CRAM-MD5")continue;
        if(token.substr(0,5)=="CRAM-"){
            const auto split=token.find('-',5);if(split==token.npos)return false;const auto hashes=token.substr(5,split-5);bool md5=false;
            std::size_t at{};while(at<hashes.size()){const auto end=hashes.find('/',at);if(hashes.substr(at,end==hashes.npos?hashes.npos:end-at)=="MD5")md5=true;if(end==hashes.npos)break;at=end+1;}
            if(!md5)continue;
            const auto challenge=token.substr(split+1);if(challenge.size()<16||challenge.size()>128||challenge.size()%2)return false;
            for(const auto ch:challenge)if(hex(ch)<0)return false;
            if(!o.challenge_hex.empty()&&fold(o.challenge_hex)!=fold(challenge))return false;
            o.challenge_hex=fold(challenge);
        }
    }
    return true;
}

FtnPacketResult decode_ftn_packet(std::string_view bytes,std::string_view domain,bool retain_rejected){
    const auto bad=[](const char* reason){return FtnPacketResult{{},reason};};
    if(bytes.size()<60||bytes.size()>kFtnPacketMaximumBytes)return bad("packet_size");
    if(word(bytes,18)!=2)return bad("unsupported_packet_type");
    FtnPacket packet;auto& h=packet.header;
    h.origin.node=word(bytes,0);h.destination.node=word(bytes,2);h.origin.net=word(bytes,20);h.destination.net=word(bytes,22);
    h.year=word(bytes,4);h.month=word(bytes,6);h.day=word(bytes,8);h.hour=word(bytes,10);h.minute=word(bytes,12);h.second=word(bytes,14);if(!packet_time(h))return bad("packet_date");
    h.origin.zone=word(bytes,34);h.destination.zone=word(bytes,36);h.origin.domain=h.destination.domain=std::string(domain);
    std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes.data()+26),8,h.password.begin());
    const auto capability=word(bytes,44),copy=word(bytes,40);const auto swapped=static_cast<std::uint16_t>((capability>>8)|(capability<<8));
    h.profile=FtnPacketProfile::type2;
    if((capability&1)&&copy==swapped){
        if(capability&~std::uint16_t{1})return bad("unsupported_packet_capability");
        h.profile=FtnPacketProfile::type2plus;
        const auto oz=word(bytes,46),dz=word(bytes,48);if(oz)h.origin.zone=oz;if(dz)h.destination.zone=dz;
        h.origin.point=word(bytes,50);h.destination.point=word(bytes,52);if(h.origin.net==65535&&h.origin.point)h.origin.net=word(bytes,38);
    }else if(capability||copy)return bad("invalid_packet_capability");
    h.product=static_cast<unsigned char>(bytes[24])|(h.profile==FtnPacketProfile::type2plus?static_cast<unsigned char>(bytes[42])<<8:0);h.major=static_cast<std::uint8_t>(bytes[25]);h.minor=h.profile==FtnPacketProfile::type2plus?static_cast<std::uint8_t>(bytes[43]):0;
    std::size_t at=58;
    while(at+2<=bytes.size()){
        const auto type=word(bytes,at);at+=2;if(!type){if(at!=bytes.size())return bad("packet_trailing_bytes");return {std::move(packet),{}};}
        if(type!=2||bytes.size()-at<32)return bad("packed_message_header");
        if(packet.messages.size()==4096)return bad("packet_message_limit");
        FtnPackedMessage m;
        m.origin_node=word(bytes,at);m.destination_node=word(bytes,at+2);m.origin_net=word(bytes,at+4);m.destination_net=word(bytes,at+6);m.attributes=word(bytes,at+8);m.cost=word(bytes,at+10);at+=12;
        const auto date=bytes.substr(at,20);const auto end=date.find('\0');if(end==date.npos)return bad("message_date");m.date=std::string(date.substr(0,end));at+=20;
        if(!read_z(bytes,at,m.to,35)||!read_z(bytes,at,m.from,35)||!read_z(bytes,at,m.subject,71)||!read_z(bytes,at,m.text,kFtnMessageMaximumBytes)||!packed_fields(m))return bad("message_fields");
        if(!retain_rejected&&!inspect_ftn_message(h,m))return bad("message_controls");
        packet.messages.push_back(std::move(m));
    }return bad("packet_missing_terminator");
}
std::optional<std::string> encode_ftn_packet(const FtnPacket& packet){
    const auto& h=packet.header;if(!packet_time(h)||packet.messages.size()>4096||static_cast<unsigned>(h.profile)>1)return {};
    if(h.profile==FtnPacketProfile::type2&&(h.origin.point||h.destination.point))return {};
    std::string out(58,'\0');word_at(out,0,h.origin.node);word_at(out,2,h.destination.node);word_at(out,4,h.year);word_at(out,6,h.month);word_at(out,8,h.day);word_at(out,10,h.hour);word_at(out,12,h.minute);word_at(out,14,h.second);word_at(out,18,2);
    word_at(out,20,h.origin.net);word_at(out,22,h.destination.net);out[24]=static_cast<char>(h.product&255);out[25]=static_cast<char>(h.major);std::copy(h.password.begin(),h.password.end(),out.begin()+26);word_at(out,34,h.origin.zone);word_at(out,36,h.destination.zone);
    if(h.profile==FtnPacketProfile::type2plus){word_at(out,40,0x0100);out[42]=static_cast<char>(h.product>>8);out[43]=static_cast<char>(h.minor);word_at(out,44,1);word_at(out,46,h.origin.zone);word_at(out,48,h.destination.zone);word_at(out,50,h.origin.point);word_at(out,52,h.destination.point);}
    for(const auto& m:packet.messages){if(!packed_fields(m)||!inspect_ftn_message(h,m)||out.size()+m.text.size()+256>kFtnPacketMaximumBytes)return {};
        for(const auto n:{std::uint16_t{2},m.origin_node,m.destination_node,m.origin_net,m.destination_net,m.attributes,m.cost})push_word(out,n);
        out+=m.date;out.append(20-m.date.size(),'\0');for(const auto* s:{&m.to,&m.from,&m.subject,&m.text}){out+=*s;out+='\0';}}
    out.append(2,'\0');return out;
}
std::optional<FtnMessageMetadata> inspect_ftn_message(const FtnPacketHeader& h,const FtnPackedMessage& m){
    FtnMessageMetadata meta;meta.origin={h.origin.zone,m.origin_net,m.origin_node,0,h.origin.domain};meta.destination={h.destination.zone,m.destination_net,m.destination_node,0,h.destination.domain};
    std::set<std::string> singletons;std::optional<std::uint16_t> from_point,to_point;std::optional<FtnAddress> echo_origin;auto text=std::string_view(m.text);std::size_t rows{};
    while(!text.empty()){
        if(++rows>65536)return {};
        const auto at=text.find_first_of("\r\n");auto line=text.substr(0,at);if(at==text.npos)text={};else{const auto separator=text[at];text.remove_prefix(at+1);if(separator=='\r'&&!text.empty()&&text.front()=='\n')text.remove_prefix(1);}
        if(rows==1&&line.substr(0,5)=="AREA:"){const auto tag=line.substr(5);if(!valid_ftn_echo_tag(tag))return {};meta.area=std::string(tag);continue;}
        if(!meta.area.empty()&&line.substr(0,10)==" * Origin:") {
            const auto open=line.rfind('('),close=line.rfind(')');
            if(open!=line.npos&&close==line.size()-1&&open<close){auto address=parse_ftn_address(line.substr(open+1,close-open-1));if(address){if(!address->domain.empty()&&!h.origin.domain.empty()&&fold(address->domain)!=fold(h.origin.domain))return {};if(address->domain.empty())address->domain=h.origin.domain;echo_origin=std::move(address);}}
        }
        if(line.empty()||line.front()!=1)continue;
        if(line.size()>4096||meta.controls.size()==4096)return {};
        meta.controls.emplace_back(line);line.remove_prefix(1);
        const auto split=line.find(' ');auto key=line.substr(0,split),value=split==line.npos?std::string_view{}:line.substr(split+1);if(!key.empty()&&key.back()==':')key.remove_suffix(1);
        if(key=="MSGID"||key=="REPLY"||key=="CHRS"||key=="CHARSET"||key=="INTL"||key=="FMPT"||key=="TOPT"){
            // An empty optional reply identifier supplies no thread linkage.
            // Keep the exact control in the raw message and fingerprint.
            if(key=="REPLY"&&std::all_of(value.begin(),value.end(),[](char c){return c==' '||c=='\t';}))continue;
            if(!singletons.insert(std::string(key)).second||value.empty())return {};
            if(key=="MSGID")meta.msgid=std::string(value);else if(key=="REPLY")meta.reply=std::string(value);
            else if(key=="CHRS"||key=="CHARSET"){if(!meta.charset.empty())return {};meta.charset=std::string(value);}
            else if(meta.area.empty()&&key=="INTL"){
                const auto pair=words(value);if(pair.size()!=2)return {};auto destination=parse_ftn_address(pair[0]),origin=parse_ftn_address(pair[1]);
                if(!destination||!origin||destination->point||origin->point||(!destination->domain.empty()&&fold(destination->domain)!=fold(h.destination.domain))||(!origin->domain.empty()&&fold(origin->domain)!=fold(h.origin.domain)))return {};
                destination->domain=h.destination.domain;origin->domain=h.origin.domain;meta.destination=*destination;meta.origin=*origin;
            }else if(meta.area.empty()&&(key=="FMPT"||key=="TOPT")){std::uint64_t n{};if(!decimal(value,n)||n>65535)return {};if(key=="FMPT")from_point=static_cast<std::uint16_t>(n);else to_point=static_cast<std::uint16_t>(n);}
        }
    }
    if(meta.area.empty()){if(m.to.empty())return {};meta.origin.point=from_point.value_or(0);meta.destination.point=to_point.value_or(0);}else if(echo_origin)meta.origin=*echo_origin;return meta;
}
std::optional<FtnMessageMetadata> inspect_ftn_held_message(const FtnPacketHeader& h,const FtnPackedMessage& m){
    if(auto valid=inspect_ftn_message(h,m))return valid;
    // Remove identity controls only in a temporary metadata view. Ambiguous
    // identifiers are never selected, rewritten on disk or forwarded.
    auto view=m;view.text.clear();std::string_view remaining=m.text;
    while(!remaining.empty()){
        const auto end=remaining.find_first_of("\r\n");const auto row=remaining.substr(0,end);
        const bool identity=row.substr(0,7)==std::string_view("\1MSGID:",7)||row.substr(0,6)==std::string_view("\1MSGID",6)||row.substr(0,7)==std::string_view("\1REPLY:",7)||row.substr(0,6)==std::string_view("\1REPLY",6);
        if(!identity){view.text.append(row);view.text+='\r';}
        if(end==remaining.npos)break;
        const auto separator=remaining[end];remaining.remove_prefix(end+1);if(separator=='\r'&&!remaining.empty()&&remaining.front()=='\n')remaining.remove_prefix(1);
    }
    auto meta=inspect_ftn_message(h,view);if(!meta)return {};
    meta->msgid.clear();meta->reply.clear();return meta;
}
}
