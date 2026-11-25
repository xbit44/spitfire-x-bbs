#include "sfx/application/qwk.hpp"
#include <iostream>
#include <stdexcept>
#include <algorithm>

namespace app=sfx::application;
namespace {
unsigned checks{};
void check(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
const app::QwkFile& find(const std::vector<app::QwkFile>& files,const std::string& name){
    const auto f=std::find_if(files.begin(),files.end(),[&](const app::QwkFile& v){return v.name==name;});
    if(f==files.end())throw std::runtime_error("missing encoded file");
    return *f;
}
std::string rep_message(std::uint16_t conference,std::string subject="subject",std::string to="All Callers",std::string body="hello\n",bool private_mail=false){
    std::string h(128,' ');
    const auto put=[&](std::size_t offset,std::size_t length,const std::string& value){h.replace(offset,std::min(length,value.size()),value.substr(0,length));};
    h[0]=private_mail?'*':' ';put(1,7,std::to_string(conference));put(8,8,"10-13-26");put(16,5,"13:00");
    put(21,25,to);put(46,25,"Forged Author");put(71,25,subject);put(116,6,std::to_string(1+(body.size()+127)/128));
    h[122]=static_cast<char>(0xe1);h[123]=static_cast<char>(conference&255);h[124]=static_cast<char>(conference>>8);
    std::replace(body.begin(),body.end(),'\n',static_cast<char>(0xe3));h+=body;h.append((128-body.size()%128)%128,' ');return h;
}
std::vector<app::QwkFile> rep(const std::string& records){std::string first(128,' ');first.replace(0,4,"SFX1");return {{"sfx1.msg",first+records}};}
}
int main(){try{
    app::QwkPreferences p;
    check(!app::valid_qwk_preferences(p),"owner wrap default is not invented");
    check(app::valid_qwk_preferences(p,false),"otherwise approved missing-only defaults valid");
    p.word_wrap=false;check(app::valid_qwk_preferences(p),"explicit wrap Off valid");
    check(p.private_mail==app::QwkPrivateMode::unread && !p.own_public_posts && p.encoding==app::QwkEncoding::cp437 && p.preserve_colors && p.indexes && p.full_headers,"approved packet defaults except reserved wrap");
    p.encoding=app::QwkEncoding::utf8;p.full_headers=false;check(!app::valid_qwk_preferences(p),"UTF8 requires full headers without silently changing another choice");p={};p.word_wrap=false;
    for(const auto n:{1U,10000U,65535U})check(app::valid_qwk_message_count(n),"valid finite QWK count");
    for(const auto n:{0U,65536U,0xffffffffU})check(!app::valid_qwk_message_count(n),"invalid count is not Unlimited");
    for(const auto id:{"SFX1","a","X-BIT","Abcdefgh","Z_123"})check(app::valid_qwk_board_id(id),"DOS board ID accepted");
    for(const auto id:{"","123","123456789","ABCDEFGHI","SYsOp","netmail","CON","PRN","AUX","NUL","CLOCK$","COM1","LPT9","a.b","a/b","a\\b","a:b","a b","a?","a\n"})check(!app::valid_qwk_board_id(id),"unsafe/reserved board ID rejected");
    check(app::canonical_qwk_board_id("x-bit")=="X-BIT","board identity case insensitive");
    std::string all;for(unsigned n=0;n<256;++n)all+=static_cast<char>(n);
    check(app::qwk_utf8_to_cp437(app::qwk_cp437_to_utf8(all))==all,"all 256 supported bytes lossless UTF8 conversion");
    for(const auto& bad:{std::string("\xc0\xaf"),std::string("\xed\xa0\x80"),std::string("\xe0\x80\x80"),std::string("\xc2"),std::string("\x80"),std::string("\xf0\x9f\x98\x80")})check(!app::qwk_utf8_to_cp437(bad),"malformed/unrepresentable UTF8 refused not replaced");
    app::QwkPacketMetadata meta{"SFX1","Test BBS","Town","","Sysop","Caller One","10-13-2026,13:00:00",{{0,"Private mail"},{1,"Public Chat"},{999,"Final public echo"}}};
    app::QwkRecord m{1,123,0,false,false,"Caller Two","All Callers",std::string(70,'s'),"hello\n","10-13-26","13:00","<stable-sfx-id>","","20261013200000+0000",false};
    auto packet=app::encode_qwk(meta,p,{m});check(static_cast<bool>(packet),"full normal QWK encoded");
    const auto& dat=find(packet.files,"MESSAGES.DAT").bytes;check(dat.size()==384 && dat[128+122]==static_cast<char>(0xe1) && dat[128+123]==1 && dat[128+124]==0,"fixed 128-byte active header and LE16 conference");
    check(dat.substr(256,6)==std::string("hello")+static_cast<char>(0xe3),"classic QWK newlines and complete body");
    check(find(packet.files,"HEADERS.DAT").bytes.find(m.subject)!=std::string::npos,"full subject kept alongside narrowed compatibility field");
    check(find(packet.files,"HEADERS.DAT").bytes.find("[80]")!=std::string::npos,"headers keyed to hexadecimal byte offset");
    check(find(packet.files,"DOOR.ID").bytes.find("CONTROLTYPE = ADD")!=std::string::npos && find(packet.files,"DOOR.ID").bytes.find("RESET")==std::string::npos,"advertises only settled ADD/DROP support");
    const auto& index=find(packet.files,"001.NDX").bytes;check(index==std::string("\0\0\0\x82\x01",5),"record2 exact MBF index encoding");
    p.full_headers=false;p.indexes=false;packet=app::encode_qwk(meta,p,{m});check(packet && packet.files.size()==3,"classic no-index leaves always-on control/door/messages only");
    check(m.subject==std::string(70,'s'),"compatibility export does not rewrite original field");
    check(!app::encode_qwk(meta,p,{}),"empty codec doesn't manufacture packet");
    check(!app::encode_qwk(meta,p,{m,m},1),"combined count enforced without truncation");
    auto broken=meta;broken.conferences.push_back({1,"Collision"});check(!app::encode_qwk(broken,p,{m}),"duplicate conference rejected");
    broken=meta;broken.created="nonsense";check(!app::encode_qwk(broken,p,{m}),"invalid packet datetime rejected");
    m.body=std::string(81,'x')+"\n";p.word_wrap=true;packet=app::encode_qwk(meta,p,{m});check(packet && find(packet.files,"MESSAGES.DAT").bytes[256+80]==static_cast<char>(0xe3),"wrap On bounds long prose without dropping bytes");
    m.body="xb> "+std::string(90,'x')+"\n";packet=app::encode_qwk(meta,p,{m});check(packet&&find(packet.files,"MESSAGES.DAT").bytes.find(std::string(1,static_cast<char>(0xe3))+"xb> ")!=std::string::npos,"QWK exported quote continuations keep initials marker");m.body=std::string(81,'x')+"\n";
    m.artwork=true;packet=app::encode_qwk(meta,p,{m});check(packet && find(packet.files,"MESSAGES.DAT").bytes[256+80]=='x',"artwork layout not damaged by prose wrapping");
    m.artwork=false;m.body="\x1b[31m"+std::string(80,'x')+"\x1b[0m\n";packet=app::encode_qwk(meta,p,{m});
    auto expected_color=m.body;std::replace(expected_color.begin(),expected_color.end(),'\n',static_cast<char>(0xe3));check(packet&&find(packet.files,"MESSAGES.DAT").bytes.substr(256,expected_color.size())==expected_color,"wrap On does not split supported generated SGR/rendition bytes");
    m.body=std::string(90,static_cast<char>(0xc4))+"\n";packet=app::encode_qwk(meta,p,{m});check(packet&&find(packet.files,"MESSAGES.DAT").bytes[256+80]==static_cast<char>(0xc4)&&find(packet.files,"MESSAGES.DAT").bytes[256+90]==static_cast<char>(0xe3),"wrap On preserves CP437 box/block layout without caller art flag");
    m.artwork=false;m.body=std::string("Before ")+static_cast<char>(0xe3)+" after\nNext "+static_cast<char>(0xe3)+"\n";
    const auto original=m.body;
    for(const bool wrap:{false,true})for(const bool artwork:{false,true}){
        p.word_wrap=wrap;m.artwork=artwork;auto following=m;following.number=124;following.body="Following message\n";
        packet=app::encode_qwk(meta,p,{m,following});check(bool(packet),"classic content collision no longer blocks complete packet");
        const auto& classic=find(packet.files,"MESSAGES.DAT").bytes;
        const std::string expected="Before * after\xe3Next *\xe3";
        check(classic.size()==640&&classic.substr(256,expected.size())==expected,"content pi becomes star and real newline markers retain framing");
        check(classic[384+122]==static_cast<char>(0xe1)&&classic.substr(512,18)==std::string("Following message\xe3",18),"next complete header/body remain correctly aligned");
        check(m.body==original,"classic fallback preserves original body for wrap and artwork settings");
    }
    p.full_headers=true;p.encoding=app::QwkEncoding::utf8;packet=app::encode_qwk(meta,p,{m});const std::string expected_utf8="Before \xcf\x80 after\nNext \xcf\x80\n";check(packet && find(packet.files,"MESSAGES.DAT").bytes.substr(256,expected_utf8.size())==expected_utf8,"UTF8 preserves pi and actual line endings");
    auto input=rep(rep_message(1));auto parsed=app::decode_qwk_reply("SFX1",input);check(parsed && parsed.records.size()==1 && parsed.records[0].message.body=="hello\n","ordinary REP parsed without posting side effect");
    check(parsed.records[0].message.from=="Forged Author","codec data is not storage author authority");
    const auto identity=parsed.logical_identity;input[0].name="SFX1.MSG";check(app::decode_qwk_reply("sfx1",input).logical_identity==identity,"case-only archive filename doesn't change logical identity");
    input.push_back({"VOTING.DAT",""});check(!app::decode_qwk_reply("SFX1",input),"functional extension cannot silently succeed");
    for(const auto name:{"../SFX1.MSG","C:/SFX1.MSG","SFX1.MSG/","SFX1.EXE","TODOOR.EXT","NEWFILES.DAT","nested.zip"})check(!app::decode_qwk_reply("SFX1",{{name,"anything"}}),"only exact allowed reply files accepted");
    input=rep(rep_message(1));input.push_back(input.front());check(!app::decode_qwk_reply("SFX1",input),"duplicate/case colliding files rejected");
    input=rep(rep_message(1));input[0].bytes.pop_back();check(!app::decode_qwk_reply("SFX1",input),"truncated body rejected");
    input=rep(rep_message(1));input[0].bytes[0]='X';check(app::decode_qwk_reply("SFX1",input).error==app::QwkCodecError::wrong_board,"inner ID verified independent of outer name");
    for(const auto offset:{128U,128U+122U,128U+123U,128U+96U}){input=rep(rep_message(1));input[0].bytes[offset]='V';check(!app::decode_qwk_reply("SFX1",input),"bad status/alive/routing/password record refused atomically");}
    input=rep(rep_message(1));input.push_back({"HEADERS.DAT","[80]\r\nSubject: complete full subject\r\nSender: Full Caller\r\nTo: Caller One\r\nUtf8: false\r\n"});parsed=app::decode_qwk_reply("SFX1",input);check(parsed && parsed.records[0].message.subject=="complete full subject","extended fields override classic safely");
    const auto full_identity=parsed.logical_identity;input[1].bytes="[000080]\nutf8=false\nTo=Caller One\nSubject=complete full subject\nSender=Full Caller\n";check(app::decode_qwk_reply("SFX1",input).logical_identity==full_identity,"canonical header key case/order/separators don't defeat logical replay");
    input[1].bytes+="Subject: duplicate\n";check(!app::decode_qwk_reply("SFX1",input),"duplicate full header key rejected");
    input[1].bytes="[100]\nSubject: orphan\n";check(!app::decode_qwk_reply("SFX1",input),"orphan header can't create ignored data");
    input[1].bytes="[80]\nSenderIpAddr: private\n";check(!app::decode_qwk_reply("SFX1",input),"connection metadata isn't supported packet payload");
    input=rep(rep_message(1,"ADD","SFXQWK","\n"));parsed=app::decode_qwk_reply("SFX1",input);check(parsed && parsed.records[0].command==app::QwkScanCommand::add,"NCM plain ADD empty-body control record accepted");
    input=rep(rep_message(999,"DROP","sfxqwk","\n"));parsed=app::decode_qwk_reply("SFX1",input);check(parsed && parsed.records[0].command==app::QwkScanCommand::drop,"plain DROP allowed on stable conference");
    for(const auto command:{"RESET","ADD 1","DROP ALL","CONTROL","ADD YOURS"})check(!app::decode_qwk_reply("SFX1",rep(rep_message(1,command,"SFXQWK","\n"))),"unsupported controls fail entire packet");
    check(!app::decode_qwk_reply("SFX1",rep(rep_message(1)+rep_message(1,"RESET","SFXQWK","\n"))),"one invalid final record yields no decoded import batch");
    check(!app::decode_qwk_reply("SFX1",rep(rep_message(1,"ADD","SFXQWK","extra"))),"extra control parameters/body refused");
    input=rep(rep_message(1,"subject","All Callers","second\n"));check(app::decode_qwk_reply("SFX1",input).logical_identity!=identity,"different logical message isn't body-based dedup");
    m.body=std::string(static_cast<std::size_t>(app::kQwkExpandedBytes),'x');p.encoding=app::QwkEncoding::cp437;p.word_wrap=false;check(app::encode_qwk(meta,p,{m}).error==app::QwkCodecError::limit_exceeded,"message at expanded cap can't exclude metadata/padding from limit");
    std::cout<<"PASS qwk codec "<<checks<<" checks; no actual reader/storage/transport acceptance\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL qwk codec after "<<checks<<": "<<e.what()<<'\n';return 1;}}
