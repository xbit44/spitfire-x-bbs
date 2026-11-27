#include "sfx/application/ftn_mail.hpp"
#include "sfx/application/message_source.hpp"
#include "sfx/application/qwk.hpp"
#include <algorithm>
#include <set>
namespace sfx::application {
#include "ftn_charset.inc"
namespace {
bool same(std::string_view a,std::string_view b)noexcept{if(a.size()!=b.size())return false;for(std::size_t i=0;i<a.size();++i){auto x=a[i],y=b[i];if(x>='A'&&x<='Z')x=static_cast<char>(x+32);if(y>='A'&&y<='Z')y=static_cast<char>(y+32);if(x!=y)return false;}return true;}
std::string upper(std::string_view s){std::string out(s);for(auto& c:out)if(c>='a'&&c<='z')c=static_cast<char>(c-32);return out;}
std::string_view trim(std::string_view s){while(!s.empty()&&(s.front()==' '||s.front()=='\t'))s.remove_prefix(1);while(!s.empty()&&(s.back()==' '||s.back()=='\t'))s.remove_suffix(1);return s;}
bool printable(std::string_view s,std::size_t maximum){if(s.size()>maximum)return false;for(unsigned char c:s)if(c<32||c==127||c==0x9b)return false;return true;}
bool number(std::string_view s,std::uint64_t& n,std::uint64_t maximum){if(s.empty()||s.size()>20)return false;n=0;for(char c:s){if(c<'0'||c>'9'||n>(maximum-static_cast<unsigned>(c-'0'))/10)return false;n=n*10+static_cast<unsigned>(c-'0');if(n>maximum)return false;}return true;}
std::vector<FtnAddress> addresses(const std::string& primary,const std::vector<std::string>& additional){std::vector<FtnAddress> result;for(const auto& raw:additional)if(const auto a=parse_ftn_address(raw))result.push_back(*a);if(const auto a=parse_ftn_address(primary))result.push_back(*a);return result;}
const FtnNetwork* network(const FtnConfiguration& c,std::string_view code){const auto at=std::find_if(c.networks.begin(),c.networks.end(),[&](const auto& n){return same(n.code,code);});return at==c.networks.end()?nullptr:&*at;}
const FtnLink* link(const FtnConfiguration& c,const FtnPeerMatch& match){const auto at=std::find_if(c.links.begin(),c.links.end(),[&](const auto& l){return same(l.network,match.network)&&same(l.code,match.link);});return at==c.links.end()?nullptr:&*at;}
bool claimed(const FtnLink& l,const FtnAddress& a){for(const auto& expected:addresses(l.address,l.additional_addresses))if(same_ftn_address(expected,a,true))return true;return false;}
std::optional<FtnAddress> address(std::string_view text,std::string_view domain){auto a=parse_ftn_address(text);if(!a)return {};if(!a->domain.empty()&&!domain.empty()&&!same(a->domain,domain))return {};if(a->domain.empty())a->domain=std::string(domain);return a;}
}
bool equal_ftn_secret(std::string_view a,std::string_view b)noexcept{
    if(a.size()>128||b.size()>128)return false;
    unsigned difference=static_cast<unsigned>(a.size()^b.size());
    for(std::size_t i=0;i<128;++i){const unsigned x=i<a.size()?static_cast<unsigned char>(a[i]):0,y=i<b.size()?static_cast<unsigned char>(b[i]):0;difference|=x^y;}return difference==0;
}
bool same_ftn_address(const FtnAddress& a,const FtnAddress& b,bool domainless)noexcept{return a.zone==b.zone&&a.net==b.net&&a.node==b.node&&a.point==b.point&&(same(a.domain,b.domain)||(domainless&&(a.domain.empty()||b.domain.empty())));}
std::optional<FtnPeerMatch> resolve_ftn_peer(const FtnConfiguration& c,const std::vector<FtnAddress>& remote,std::string_view en,std::string_view el){
    if(remote.empty()||remote.size()>64)return {};
    if(!en.empty()&&!el.empty()) {
        const auto* selected_network=network(c,en);
        const auto* selected_link=link(c,{std::string(en),std::string(el)});
        if(!selected_network||!selected_network->enabled||!selected_link||!selected_link->enabled)return {};
        bool selected_claim=false;
        for(const auto& claim:remote) {
            if(!claimed(*selected_link,claim))continue;
            // Other advertised addresses can belong to other networks. A
            // single claim matching two links still cannot identify a peer.
            for(const auto& other:c.links) {
                if(&other==selected_link||!other.enabled)continue;
                const auto* other_network=network(c,other.network);
                if(other_network&&other_network->enabled&&claimed(other,claim))return {};
            }
            selected_claim=true;
        }
        if(selected_claim)return FtnPeerMatch{selected_network->code,selected_link->code};
        return {};
    }
    std::optional<FtnPeerMatch> result;
    for(const auto& l:c.links){const auto* n=network(c,l.network);if(!n||!n->enabled||!l.enabled)continue;
        bool match=false;for(const auto& claim:remote)if(claimed(l,claim))match=true;if(!match)continue;
        // Inbound sessions must match one enabled link across all claims.
        if(result)return {};
        result=FtnPeerMatch{n->code,l.code};
    }
    if(result&&((!en.empty()&&!same(result->network,en))||(!el.empty()&&!same(result->link,el))))return {};
    return result;
}
bool ftn_local_address(const FtnNetwork& n,const FtnAddress& a){for(const auto& local:addresses(n.address,n.additional_addresses))if(same_ftn_address(local,a,true))return true;return false;}
bool ftn_echo_permission(const FtnConfiguration& c,std::string_view net,std::string_view code,std::string_view tag,bool file,bool receive,bool manage){
    const auto* n=network(c,net);const auto* l=link(c,{std::string(net),std::string(code)});if(!n||!n->enabled||!l||!l->enabled||(receive?!ftn_inbound(*l):!ftn_outbound(*l)))return false;
    for(const auto& p:c.permissions)if(same(p.network,n->code)&&same(p.link,l->code)&&same(p.tag,tag)&&p.file_echo==file)return manage?p.manage:receive?p.receive:p.send;
    return false;
}
std::optional<std::string> ftn_route_link(const FtnConfiguration& c,std::string_view net,const FtnAddress& destination){
    const auto* n=network(c,net);if(!n||!n->enabled||ftn_local_address(*n,destination))return {};
    std::optional<std::string> result;
    for(const auto& r:c.routes)if(same(r.network,n->code)){const auto a=parse_ftn_address(r.destination);if(a&&same_ftn_address(*a,destination,true)){const auto* l=link(c,{n->code,r.via_link});if(!l||!l->enabled||!ftn_outbound(*l)||result)return {};result=l->code;}}
    if(result)return result;
    for(const auto& l:c.links)if(same(l.network,n->code)&&l.enabled&&ftn_outbound(l)&&claimed(l,destination)){if(result)return {};result=l.code;}
    if(result)return result;
    // Only the selected network supplies the fallback. Explicit routes and
    // direct configured peers above retain precedence; local mail never gets
    // here. No nodelist entry can itself choose or authorize a direct peer.
    if(!n->default_link.empty()){
        const auto own=parse_ftn_address(n->address);
        if(own&&!own->domain.empty()&&!destination.domain.empty()&&!same(own->domain,destination.domain))return {};
        const auto* via=link(c,{n->code,n->default_link});
        if(via&&via->enabled&&ftn_outbound(*via))return via->code;
    }
    return {};
}
FtnTicResult decode_ftn_tic(std::string_view bytes,std::string_view domain){
    const auto bad=[](const char* reason){return FtnTicResult{{},reason};};if(bytes.empty()||bytes.size()>65536)return bad("tic_size");
    FtnTic t;std::set<std::string> singleton;std::size_t rows{};
    while(!bytes.empty()){
        if(++rows>4096)return bad("tic_rows");
        const auto end=bytes.find_first_of("\r\n");auto row=bytes.substr(0,end);if(end==bytes.npos)bytes={};else{const auto sep=bytes[end];bytes.remove_prefix(end+1);if(sep=='\r'&&!bytes.empty()&&bytes.front()=='\n')bytes.remove_prefix(1);}
        if(!printable(row,4096))return bad("tic_field");
        row=trim(row);if(row.empty())continue;const auto split=row.find_first_of(" \t");if(split==row.npos)return bad("tic_field");const auto key=upper(row.substr(0,split));const auto value=trim(row.substr(split+1));if(value.empty())return bad("tic_field");
        if(key!="DESC"&&key!="LDESC"&&key!="PATH"&&key!="SEENBY"&&key!="SEEN-BY"&&!singleton.insert(key).second)return bad("tic_duplicate_field");
        if(key=="AREA"){if(!valid_ftn_echo_tag(value))return bad("tic_area");t.area=upper(value);}
        else if(key=="FILE"||key=="LFILE"||key=="FULLNAME"){if(!valid_ftn_leaf_name(value))return bad("tic_filename");if(key=="FILE")t.file=std::string(value);else{if(!t.long_file.empty()&&t.long_file!=value)return bad("tic_filename_conflict");t.long_file=std::string(value);}}
        else if(key=="ORIGIN"||key=="FROM"||key=="TO"){const auto a=address(value,domain);if(!a)return bad("tic_address");if(key=="ORIGIN")t.origin=*a;else if(key=="FROM")t.from=*a;else t.to=*a;}
        else if(key=="SIZE"){std::uint64_t n{};if(!number(value,n,kFtnTransferMaximumBytes))return bad("tic_size");t.size=n;}
        else if(key=="CRC"){if(value.size()!=8)return bad("tic_crc");std::uint32_t crc{};for(char ch:value){unsigned d{};if(ch>='0'&&ch<='9')d=static_cast<unsigned>(ch-'0');else if(ch>='A'&&ch<='F')d=static_cast<unsigned>(ch-'A'+10);else if(ch>='a'&&ch<='f')d=static_cast<unsigned>(ch-'a'+10);else return bad("tic_crc");crc=(crc<<4)|d;}t.crc=crc;}
        else if(key=="PW"){if(!printable(value,128))return bad("tic_password");for(unsigned char ch:value)if(ch>126)return bad("tic_password");t.password=std::string(value);}
        else if(key=="DESC")t.descriptions.emplace_back(value);else if(key=="LDESC")t.long_descriptions.emplace_back(value);
        else if(key=="REPLACES"){if(value.size()>128||value.find_first_of("/\\:<>\"")!=value.npos||value=="."||value=="..")return bad("tic_replaces");t.replaces=std::string(value);}
        else if(key=="SEENBY"||key=="SEEN-BY"){if(t.seen_by.size()==1024)return bad("tic_seenby_limit");const auto a=address(value,domain);if(!a)return bad("tic_seenby");t.seen_by.push_back(*a);}
        else if(key=="PATH"){
            if(t.path.size()==128)return bad("tic_path_limit");
            const auto space=value.find(' ');const auto a=address(value.substr(0,space),domain);if(!a)return bad("tic_path");FtnTicPath path;path.address=*a;
            if(space!=value.npos){const auto rest=trim(value.substr(space+1)),part=rest.substr(0,rest.find(' '));std::uint64_t time{};if(!number(part,time,UINT32_MAX))return bad("tic_path_time");path.timestamp=time;const auto annotation=rest.find(' ');if(annotation!=rest.npos)path.annotation=std::string(trim(rest.substr(annotation+1)));}
            t.path.push_back(std::move(path));
        }else t.unknown.emplace_back(row);
    }
    if(t.area.empty()||t.file.empty()||!singleton.count("ORIGIN")||!singleton.count("FROM"))return bad("tic_required_field");
    return {std::move(t),{}};
}
std::optional<std::string> encode_ftn_tic(const FtnTic& t){
    if(!t.size||!t.crc||!valid_ftn_echo_tag(t.area)||!valid_ftn_leaf_name(t.file)||(!t.long_file.empty()&&!valid_ftn_leaf_name(t.long_file))||t.password.size()>128||t.path.empty())return {};
    if(!printable(t.password,128)||trim(t.password)!=t.password||!printable(t.replaces,128))return {};
    for(unsigned char ch:t.password)if(ch>126)return {};
    for(const auto* list:{&t.descriptions,&t.long_descriptions,&t.unknown})for(const auto& value:*list)if(!printable(value,4096))return {};
    for(const auto& p:t.path)if(!printable(p.annotation,1024))return {};
    // Validate public metadata before inserting the secret. Reserve the complete
    // bounded wire buffer so password insertion cannot leave allocation copies.
    const auto password_bytes=t.password.empty()?0:t.password.size()+5;
    std::string out;out.reserve(65536);bool bounded=true;
    const auto row=[&](std::string_view key,std::string_view value){if(!bounded||key.size()+value.size()+3>65536-password_bytes-out.size()){bounded=false;return;}out+=key;out+=' ';out+=value;out+="\r\n";};
    row("Area",t.area);row("Origin",format_ftn_address(t.origin));row("From",format_ftn_address(t.from));if(t.to)row("To",format_ftn_address(*t.to));row("File",t.file);if(!t.long_file.empty())row("Lfile",t.long_file);row("Size",std::to_string(*t.size));
    std::string crc(8,'0');for(unsigned i=0;i<8;++i)crc[7-i]="0123456789ABCDEF"[(*t.crc>>(i*4))&15];row("Crc",crc);
    const auto password_position=out.size();for(const auto& d:t.descriptions)row("Desc",d);for(const auto& d:t.long_descriptions)row("Ldesc",d);if(!t.replaces.empty())row("Replaces",t.replaces);
    for(const auto& p:t.path)row("Path",format_ftn_address(p.address)+(p.timestamp?" "+std::to_string(*p.timestamp):std::string{})+(p.annotation.empty()?std::string{}:" "+p.annotation));
    for(const auto& a:t.seen_by)row("Seenby",format_ftn_address(a));
    for(const auto& u:t.unknown){if(!bounded||u.size()+2>65536-password_bytes-out.size()){bounded=false;break;}out+=u;out+="\r\n";}
    if(!bounded)return {};
    const auto parsed=decode_ftn_tic(out,t.from.domain);if(!parsed.tic||!parsed.tic->password.empty())return {};
    if(!t.password.empty()){out.insert(password_position,"Pw ");out.insert(password_position+3,t.password);out.insert(password_position+3+t.password.size(),"\r\n");}return out;
}
void FtnCrc32::update(std::string_view bytes)noexcept{static const auto table=[]{std::array<std::uint32_t,256> result{};for(std::uint32_t i=0;i<256;++i){auto crc=i;for(unsigned n=0;n<8;++n)crc=(crc>>1)^(0xedb88320U&static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc&1)));result[i]=crc;}return result;}();for(unsigned char c:bytes)state_=(state_>>8)^table[(state_^c)&255];}
std::uint32_t ftn_crc32(std::string_view bytes)noexcept{FtnCrc32 crc;crc.update(bytes);return crc.value();}
FtnTicAdmission admit_ftn_tic(const FtnConfiguration& c,const FtnPeerMatch& match,const FtnTic& t,std::string_view password,bool authenticated,bool external){
    const auto* n=network(c,match.network);const auto* l=link(c,match);if(!n||!l||!claimed(*l,t.from)||!ftn_echo_permission(c,n->code,l->code,t.area,true,true))return FtnTicAdmission::unauthorized;
    if(l->credentials.tic.empty()!=password.empty())return FtnTicAdmission::wrong_password;
    if(!password.empty()){if(!equal_ftn_secret(password,t.password))return FtnTicAdmission::wrong_password;}
    else if(!t.password.empty()||!authenticated||external)return FtnTicAdmission::transport_proof_required;
    if(t.to&&!ftn_local_address(*n,*t.to))return FtnTicAdmission::wrong_destination;
    for(const auto& p:t.path)if(ftn_local_address(*n,p.address))return FtnTicAdmission::loop;
    return FtnTicAdmission::accepted;
}
std::optional<std::vector<FtnAreaFixCommand>> parse_ftn_areafix(std::string_view text){
    if(text.empty()||text.size()>65536)return {};
    std::vector<FtnAreaFixCommand> result;
    while(!text.empty()){
        const auto end=text.find_first_of("\r\n");auto row=trim(text.substr(0,end));if(end==text.npos)text={};else{const auto sep=text[end];text.remove_prefix(end+1);if(sep=='\r'&&!text.empty()&&text.front()=='\n')text.remove_prefix(1);}
        if(row=="---")break;
        if(row.empty()||row.front()=='\1')continue;
        if(result.size()==256||!printable(row,512))return {};
        const auto split=row.find_first_of(" \t");const auto token=upper(row.substr(0,split));const auto argument=split==row.npos?std::string_view{}:trim(row.substr(split+1));FtnAreaFixCommand command;
        if(token=="%LIST")command.kind=FtnAreaFixKind::list;else if(token=="%QUERY"||token=="%LINKED")command.kind=FtnAreaFixKind::linked;else if(token=="%UNLINKED")command.kind=FtnAreaFixKind::unlinked;else if(token=="%HELP")command.kind=FtnAreaFixKind::help;else if(token=="%STATUS")command.kind=FtnAreaFixKind::status;else if(token=="%PAUSE")command.kind=FtnAreaFixKind::pause;else if(token=="%RESUME")command.kind=FtnAreaFixKind::resume;
        else if(token=="%RESCAN"){
            const auto space=argument.find(' ');const auto tag=argument.substr(0,space);if(!valid_ftn_echo_tag(tag))return {};command.kind=FtnAreaFixKind::rescan;command.tag=upper(tag);
            if(space!=argument.npos){std::uint64_t count{};if(!number(trim(argument.substr(space+1)),count,1000)||!count)return {};command.rescan_count=static_cast<std::uint16_t>(count);}
        }else if(token.front()!='%'){
            auto tag=std::string_view(token);command.kind=tag.front()=='-'?FtnAreaFixKind::remove:FtnAreaFixKind::add;if(tag.front()=='+'||tag.front()=='-')tag.remove_prefix(1);if(!valid_ftn_echo_tag(tag)||!argument.empty())return {};command.tag=std::string(tag);
        }
        if(!argument.empty()&&command.kind!=FtnAreaFixKind::rescan&&command.kind!=FtnAreaFixKind::unsupported)return {};
        result.push_back(std::move(command));
    }return result;
}
#include "ftn_areafix_plan.inc"
static std::optional<MessageSource> ftn_display_source(const FtnPackedMessage& message,const FtnMessageMetadata& metadata,bool received){
    if(message.text.empty()||message.text.size()>kFtnMessageMaximumBytes)return {};
    std::string visible;auto remaining=std::string_view(message.text);std::size_t rows{};
    while(!remaining.empty()){
        const auto end=remaining.find_first_of("\r\n");const auto line=remaining.substr(0,end);std::string_view separator;
        if(end!=remaining.npos){const auto count=remaining[end]=='\r'&&end+1<remaining.size()&&remaining[end+1]=='\n'?2U:1U;separator=remaining.substr(end,count);remaining.remove_prefix(end+count);}else remaining={};
        const bool routing=(!rows&&line.substr(0,5)=="AREA:")||(!line.empty()&&line.front()==1)||(!metadata.area.empty()&&line.substr(0,8)=="SEEN-BY:");++rows;
        if(!routing){visible.append(line);if(received&&!separator.empty())visible.append("\r\n");else visible.append(separator);}
    }
    auto converted=ftn_received_text(visible,metadata.charset);if(!converted)return {};
    // Historical CP437 messages also use display glyphs in C0 positions.
    // Render a visible approximation rather than executing terminal controls.
    constexpr char low[32]={' ','*','*','*','*','*','*',' ',' ','\t','\n','M','F','\r','*','*','>','<','|','!','P','S','-','|','^','v','>','?','<','-','^','v'};
    for(auto& c:*converted){const auto byte=static_cast<unsigned char>(c);if(byte<32&&byte!=27&&byte!=7)c=low[byte];else if(byte==127)c='^';}
    MessageSource out;out.format=converted->find('\x1b')==converted->npos?MessageSourceFormat::plain:MessageSourceFormat::ansi_canvas;out.bytes=std::move(*converted);
    if(!received&&out.format==MessageSourceFormat::ansi_canvas&&!validate_message_source(out,kMaximumMessageLines)){
        // Keep the immutable wire original separately. Unsupported terminal
        // sequences get an inert plain rendition; they never reject unrelated
        // text, execute queries, or change the existing source-profile parser.
        std::string plain;plain.reserve(out.bytes.size());
        for(std::size_t at=0;at<out.bytes.size();){const auto c=out.bytes[at++];if(c!=27){plain+=c;continue;}if(at==out.bytes.size())break;
            if(out.bytes[at]=='['){++at;std::size_t count{};while(at<out.bytes.size()&&count++<128){const auto b=static_cast<unsigned char>(out.bytes[at++]);if(b>=0x40&&b<=0x7e)break;}}
            else if(out.bytes[at]==']'||out.bytes[at]=='P'||out.bytes[at]=='_'||out.bytes[at]=='^'){++at;while(at<out.bytes.size()){if(out.bytes[at++]==7)break;if(at>=2&&out.bytes[at-2]==27&&out.bytes[at-1]=='\\')break;}}
            else ++at;
        }
        out.format=MessageSourceFormat::plain;out.bytes=std::move(plain);
    }
    if(!received)for(auto& c:out.bytes)if(c==7)c=' ';
    return out;
}
std::optional<MessageSource> ftn_message_display_source(const FtnPackedMessage& message,const FtnMessageMetadata& metadata){return ftn_display_source(message,metadata,false);}
std::optional<MessageCanvas> ftn_received_ansi_canvas(const FtnPackedMessage& message,bool pipes,std::string_view domain){
    if(message.text.find('\x1b')==message.text.npos)return {};
    FtnPacketHeader header;header.origin.domain=header.destination.domain=std::string(domain);
    const auto metadata=inspect_ftn_message(header,message);if(!metadata)return {};
    const auto source=ftn_display_source(message,*metadata,true);if(!source||source->format!=MessageSourceFormat::ansi_canvas)return {};
    return message_received_ansi_canvas(source->bytes,pipes);
}
std::optional<std::string> ftn_message_field(std::string_view value,const FtnMessageMetadata& metadata,std::size_t maximum){
    auto text=ftn_received_text(value,metadata.charset);if(!text||!printable(*text,maximum))return {};return text;
}
FtnMessageFingerprintMaterial ftn_message_fingerprint_material(const FtnPackedMessage& message,const FtnMessageMetadata& metadata){
    FtnMessageFingerprintMaterial result;result.content="SFX/FTNContent/v1";
    const auto field=[&](std::string_view value){result.content+=';';result.content+=std::to_string(value.size());result.content+=':';result.content.append(value);};
    field(message.date);field(message.to);field(message.from);field(message.subject);field(std::to_string(message.attributes&1));
    auto remaining=std::string_view(message.text);std::size_t rows{};
    while(!remaining.empty()){
        const auto end=remaining.find_first_of("\r\n");const auto line=remaining.substr(0,end);if(end==remaining.npos)remaining={};else{const auto sep=remaining[end];remaining.remove_prefix(end+1);if(sep=='\r'&&!remaining.empty()&&remaining.front()=='\n')remaining.remove_prefix(1);}
        const bool routing=(!rows&&line.substr(0,5)=="AREA:")||(!metadata.area.empty()&&line.substr(0,8)=="SEEN-BY:")||line.substr(0,6)=="\1PATH:"||line.substr(0,5)=="\1TID:"||line.substr(0,5)=="\1Via ";++rows;if(!routing)field(line);
    }
    result.identity=metadata.msgid.empty()?"SFX/FTNNoMSGID/v1;"+result.content:"SFX/FTNMSGID/v1;"+metadata.msgid;
    return result;
}
}
