#include "sfx/application/ftn_wire.hpp"
#include "sfx/application/ftn_mail.hpp"
#include "sfx/application/message_source.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <fstream>
#include <iterator>
namespace app=sfx::application;
namespace {
unsigned checks{};
void check(bool value,const char* text){++checks;if(!value)throw std::runtime_error(text);}
app::FtnPacketHeader header(){app::FtnPacketHeader h;h.year=2026;h.month=10;h.day=2;h.origin={42,100,2,0,{}};h.destination={42,100,1,0,{}};return h;}
app::FtnPackedMessage message(){app::FtnPackedMessage m;m.origin_net=100;m.origin_node=2;m.destination_net=100;m.destination_node=1;m.date="02 Nov 26  16:52:51";m.to="All";m.from="Caller";m.subject="Historical test";m.text="AREA:FIDOTEST\r\1MSGID: 42:100/2 00000001\rMessage body\r * Origin: Test (42:100/2)\r";return m;}
}
int main(int argc,char** argv){try{
    const auto h=header();auto m=message();
    check(app::inspect_ftn_message(h,m).has_value(),"normal historical echo metadata");
    m.text="AREA:FIDOTEST\r\1MSGID: 42:100/2 00000001\r\1REPLY: \rMessage body\r * Origin: Test (42:100/2)\r";
    const auto optional=app::inspect_ftn_message(h,m);
    check(optional.has_value()&&optional->reply.empty(),"empty optional REPLY must not reject the received echo");
    check(app::encode_ftn_packet({h,{m}}).has_value(),"empty optional REPLY retained in exact wire serialization");
    m=message();m.to.clear();
    check(app::encode_ftn_packet({h,{m}}).has_value(),"blank public recipient must remain representable on wire");
    const auto meta=app::inspect_ftn_message(h,m);check(meta.has_value(),"blank public recipient retains echo metadata");
    const auto content=app::ftn_message_fingerprint_material(m,*meta);m.to="All";
    check(content.content!=app::ftn_message_fingerprint_material(m,*meta).content,"wire blank recipient must not be rewritten in content identity");
    m=message();m.text="AREA:FIDOTEST\r\1CHRS: UTF-8 4\rTest \xe2\x80\x99 punctuation \xf0\x9f\x98\x80\r";
    const auto unicode=app::inspect_ftn_message(h,m);check(unicode.has_value(),"UTF-8 metadata");
    const auto source=app::ftn_message_display_source(m,*unicode);
    check(source.has_value(),"display fallback must not reject a Unicode message");
    check(m.text.find("\xf0\x9f\x98\x80")!=m.text.npos,"original Unicode wire bytes untouched");
    for(const auto* charset:{"CP850 2","CP866 2","CP852 2","LATIN-1 2","ISO-8859-1 2","CP1252 2"}){
        m=message();m.text="AREA:FIDOTEST\r\1CHRS: "+std::string(charset)+"\rText \x82\r";
        const auto metadata=app::inspect_ftn_message(h,m);check(metadata.has_value(),"declared legacy charset metadata");
        check(app::ftn_message_display_source(m,*metadata).has_value(),"declared legacy charset must produce a bounded display rendition");
    }
    m=message();m.text="AREA:FIDOTEST\rBefore \x1b]0;hidden title\7After \x1b[1Acontinued\r";
    const auto safe_meta=app::inspect_ftn_message(h,m);const auto inert=app::ftn_message_display_source(m,*safe_meta);
    check(inert&&inert->format==app::MessageSourceFormat::plain&&inert->bytes.find("Before ")!=std::string::npos&&inert->bytes.find("After ")!=std::string::npos&&inert->bytes.find("continued")!=std::string::npos&&inert->bytes.find("hidden title")==std::string::npos,"unsupported terminal sequences keep surrounding text in an inert plain rendition");
    check(inert&&bool(app::validate_message_source(*inert,app::kMaximumMessageLines))&&m.text.find("\x1b]0;")!=std::string::npos,"display fallback validates without rewriting the retained original");
    m=message();m.text="AREA:FIDOTEST\r\x1b[31mSupported ANSI color\x1b[0m\r";const auto ansi_meta=app::inspect_ftn_message(h,m);const auto ansi=app::ftn_message_display_source(m,*ansi_meta);check(ansi&&ansi->format==app::MessageSourceFormat::ansi_canvas&&bool(app::validate_message_source(*ansi,app::kMaximumMessageLines)),"supported ANSI remains available under existing per-echo presentation policy");
    std::size_t total{},held{},unsupported{};
    m=message();m.text="AREA:FIDOTEST\r\1CHRS: CP437 2\r\x1b[31mABC\x1b[2DZ\r\x1b[32mNext\r";
    const auto preserved=m.text;const auto recovered=app::ftn_received_ansi_canvas(m,false);
    check(recovered&&recovered->text.rows[0]=="AZC"&&recovered->cells[0].attribute==4&&recovered->cells[80].attribute==2,"received v2 recovers authored color and cursor geometry from old retained wire");
    check(m.text==preserved,"received view never changes original wire or routing metadata");
    m=message();m.text="\1INTL 2:100/1@fidonet 2:100/2@fidonet\r\1CHRS: CP437 2\r\x1b[31mPrivate art\r";
    check(app::ftn_received_ansi_canvas(m,false,"fidonet").has_value(),"domain-qualified private NetMail uses its retained network domain");
    for(int arg=1;arg<argc;++arg){std::ifstream in(argv[arg],std::ios::binary);check(bool(in),"fixture readable");const std::string bytes{std::istreambuf_iterator<char>(in),{}};
        const auto packet=app::decode_ftn_packet(bytes,{},true);check(packet.packet.has_value(),("bounded original packet framing: "+packet.reason).c_str());
        for(const auto& message:packet.packet->messages){++total;const auto metadata=app::inspect_ftn_message(packet.packet->header,message);if(!metadata){++held;check(app::inspect_ftn_held_message(packet.packet->header,message).has_value(),"ambiguous ID observation remains available");continue;}
            const auto rendition=app::ftn_message_display_source(message,*metadata);const auto from=app::ftn_message_field(message.from,*metadata,35),to=app::ftn_message_field(message.to,*metadata,35),subject=app::ftn_message_field(message.subject,*metadata,71);
            if(!rendition||!from||!to||!subject){++unsupported;std::cout<<"Unrepresented fixture item "<<arg<<":"<<(&message-packet.packet->messages.data()+1)<<" charset="<<metadata->charset<<" body="<<bool(rendition)<<" from="<<bool(from)<<" to="<<bool(to)<<" subject="<<bool(subject)<<"\n";}
            if(rendition){const auto display=app::validate_message_source(*rendition,app::kMaximumMessageLines);if(!display){std::cout<<"Source validation "<<arg<<":"<<(&message-packet.packet->messages.data()+1)<<" code="<<static_cast<unsigned>(display.validation)<<" controls=";for(unsigned byte=0;byte<32;++byte)if(byte!=9&&byte!=10&&byte!=13&&rendition->bytes.find(static_cast<char>(byte))!=std::string::npos)std::cout<<byte<<",";std::cout<<"\n";}}
        }
    }
    std::cout<<"PASS "<<checks<<" received-FTN compatibility and immutable-source checks; fixture messages="<<total<<" ambiguous="<<held<<" unsupported="<<unsupported<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<"\n";return 1;}}
