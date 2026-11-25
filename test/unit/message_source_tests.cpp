#include "sfx/application/message_source.hpp"
#include "sfx/test/terminal_stream_fake.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <fstream>
#include <iterator>
namespace app=sfx::application;
using namespace std::string_literals;
namespace {
unsigned checks{};
void check(bool ok,const char* label){++checks;if(!ok)throw std::runtime_error(label);}
app::ValidatedMessageSource art(std::string bytes,unsigned limit=1000){return app::validate_message_source({app::MessageSourceFormat::ansi_canvas,1,std::move(bytes)},limit);}
}
int main(int argc,char** argv){try{
    if(argc==5 && std::string_view(argv[1])=="--export-canvas"){
        std::ifstream input(argv[2],std::ios::binary);check(bool(input),"canvas fixture opens read-only");
        const std::string bytes{std::istreambuf_iterator<char>(input),{}};
        const auto validated=art(bytes);check(bool(validated),"canvas fixture supported");
        const bool pipes=std::string_view(argv[4])=="on";
        check(pipes || std::string_view(argv[4])=="off","explicit policy variant");
        const auto& canvas=pipes?validated.on:validated.off;
        const std::string prefix=argv[3];
        check(!std::ifstream(prefix+".cells") && !std::ifstream(prefix+".ans"),"new canvas evidence only");
        std::ofstream cells(prefix+".cells",std::ios::binary),wire(prefix+".ans",std::ios::binary);
        for(std::size_t i=0;i<canvas.extent*80U;++i){cells.put(char(canvas.cells[i].glyph));cells.put(char(canvas.cells[i].attribute));}
        for(std::size_t first=0;first<canvas.extent;first+=100){
            const auto page=app::message_canvas_page(canvas,first,std::min<std::size_t>(100,canvas.extent-first),true);check(bool(page),"generated export page");
            sfx::test::DeterministicInteractiveByteStreamFake output;app::StreamCallerTerminal terminal(output);
            check(terminal.present_display({"test.export.canvas",&*page})==app::TerminalOutcome::completed,"safe export render");
            wire.write(output.output().data(),output.output().size());
        }
        check(bool(cells) && bool(wire),"canvas evidence written");
        std::cout<<"PASS export rows="<<canvas.extent<<" checks="<<checks<<'\n';return 0;
    }
    check(!art(""),"empty rejected");check(!art(std::string(262145,'x')),"source cap");
    {
        const std::string source="\x1b[31mABC\x1b[2DZ\r\n\x1b[32mNEXT\x1b[1A\x1b[1GX";
        check(!art(source),"immutable profile1 still refuses v2 cursor operations");
        const auto canvas=app::message_received_ansi_canvas(source,false);
        check(canvas&&canvas->cells[0].glyph=='X'&&canvas->cells[1].glyph=='Z'&&canvas->cells[2].glyph=='C',"v2 virtual up/left/column positioning preserves geometry");
        check(canvas->cells[0].attribute==2&&canvas->cells[1].attribute==4&&canvas->cells[80].attribute==2,"authored colors retained independently of caller text color");
        const auto safe=app::message_received_ansi_canvas("\x1b[31mRed\x1b]0;hidden\7\x1b[6n\x1b[?25l\x1b[0mOK",false);
        check(safe&&safe->text.canonical_lf=="RedOK\n"&&safe->cells[0].attribute==4,"unsupported queries, modes and control strings are inert without discarding colors");
        for(const auto& input:std::vector<std::string>{std::string(262145,'x'),"\x1b[1000BOUT","\x1b[1;81HX","\x1b]unterminated","\x1b["})check(!app::message_received_ansi_canvas(input,false),"v2 bounded malformed or excessive artwork fallback");
        check(app::message_received_ansi_canvas("\x1b[80CX",false)->cells[79].glyph=='X',"received relative right motion clamps to terminal edge");
        const auto literal=app::message_received_ansi_canvas("\x1b[31m|10X",false),pipes=app::message_received_ansi_canvas("\x1b[31m|10X",true);
        check(literal&&literal->text.rows[0]=="|10X"&&literal->cells[0].attribute==4&&pipes&&pipes->text.rows[0]=="X"&&pipes->cells[0].attribute==10,"v2 pipes remain an independent ordered option");
        const auto erased=app::message_received_ansi_canvas("ABCDE\x1b[3D\x1b[KZ",false);
        check(erased&&erased->text.rows[0]=="ABZ","v2 erase line preserves positions");
        const auto rgb=app::message_received_ansi_canvas("\x1b[1;12;34;56tX\x1b[0;90;80;70t \x1b[38;2;1;2;3mY\x1b[0mZ",false);
        check(rgb&&rgb->foreground_rgb[0]==0x0c2238U&&rgb->background_rgb[1]==0x5a5046U&&rgb->foreground_rgb[2]==0x010203U&&rgb->foreground_rgb[3]==0xffffffffU,"CTerm and SGR truecolor retained with explicit reset");
        const auto rgb_page=app::message_canvas_page(*rgb,0,1,true);check(rgb_page.has_value(),"generated bounded RGB page validates");
        bool found_rgb=false;for(const auto& op:rgb_page->operations)if(op.bytes=="\x1b[1;12;34;56t")found_rgb=true;check(found_rgb,"original RGB emitted through safe generated CTerm colors");
        check(app::valid_terminal_ansi_sequence("\x1b[0;255;255;255t")&&!app::valid_terminal_ansi_sequence("\x1b[8;24;80t")&&!app::valid_terminal_ansi_sequence("\x1b[18t")&&!app::valid_terminal_ansi_sequence("\x1b[1;256;0;0t"),"color-only terminal boundary rejects window operations and oversized components");
    }
    for(const std::uint8_t color:{std::uint8_t{6},std::uint8_t{10}}){
        const std::string original="A|20B|12C\n";
        const auto plain=app::validate_message_source({app::MessageSourceFormat::plain,1,original},200,color);
        check(plain&&plain.off.cells[0].attribute==color&&plain.off.cells[1].attribute==color,"plain source base follows caller preference and Off pipe text stays literal");
        check(plain.on.cells[0].attribute==color&&plain.on.cells[1].attribute==(color|64)&&plain.on.cells[2].attribute==76,"enabled background pipe retains caller foreground until explicit foreground pipe");
        const auto legacy=app::message_plain_canvas(app::normalize_message_body(original,200),true,color);
        check(legacy&&legacy->cells[0].attribute==color&&legacy->cells[1].attribute==(color|64)&&legacy->cells[2].attribute==76,"legacy plain rendition honors the same base and ordered pipe overrides");
        const auto authored=app::validate_message_source({app::MessageSourceFormat::ansi_canvas,1,"A\x1b[31mB\x1b[0mC"},200,color);
        check(authored&&authored.off.cells[0].attribute==7&&authored.off.cells[1].attribute==4&&authored.off.cells[2].attribute==7,"authored ANSI initial and reset colors remain independent of caller preference");
    }
    check(!app::validate_message_source({app::MessageSourceFormat::plain,1,"X"},200,16)&&!app::message_plain_canvas(app::normalize_message_body("X",200),false,16),"invalid caller color rejected before rendition");
    {
        const auto canvas=app::message_synchronet_pipe_canvas("|12red |23bg |24text |31\r\nbase\r|01blue\nbase",6);
        check(canvas && canvas->text.rows==std::vector<std::string>{"red bg |24text |31","base","blue","base"},"terminal pipe view leaves high codes literal");
        check(canvas->cells[0].attribute==12 && canvas->cells[4].attribute==124 &&
            canvas->cells[80].attribute==6 && canvas->cells[160].attribute==1 && canvas->cells[240].attribute==6,
            "terminal pipe styles reset on CRLF, CR and LF");
        const auto wrapped=app::message_synchronet_pipe_canvas("|10"+std::string(81,'x')+"\nnext",6);
        check(wrapped && wrapped->extent==3 && wrapped->cells[79].attribute==10 && wrapped->cells[80].attribute==10 &&
            wrapped->cells[160].attribute==6,"soft wrapping preserves color until the stored newline");
        const auto legacy=app::message_plain_canvas(app::normalize_message_body("|24first\nsecond",200),true,6);
        check(legacy && (legacy->cells[80].attribute&128),"legacy mode retains bright background/blink and cross-line color");
        const auto plain=app::message_canvas_page(*canvas,0,4,false);
        check(plain && std::none_of(plain->operations.begin(),plain->operations.end(),[](const auto& op){return op.kind==app::TerminalDisplayOperationKind::ansi_sequence;}),
            "plain terminal rendition remains escape free");
    }
    check(!app::validate_message_source({app::MessageSourceFormat::plain,2,"x"},200),"profile exact");
    auto c=art("\x1b[2J\x1b[2;3H\x1b[1;31mX");
    check(c && c.off.extent==2 && c.off.cells[82].attribute==12,"virtual position/SGR");
    check(art("\x1b[sABC\x1b[uZ").off.text.rows[0]=="ZBC","virtual save restore");
    check(!art("ABC\x1b[2J"),"cleared blank refused");
    check(bool(art("\x1b[44m\x1b[2J")),"nonblack background counts");
    for(unsigned n=0;n<32;++n){
        const auto src="|"+std::string(n<10?"0":"")+std::to_string(n)+"X";
        c=art(src);check(c && c.on.cells[0].attribute==(n<16?n:7|((n-16)<<4)),"32 VGA pipe attributes");
        check(c.off.text.rows[0]==src,"Off literal");
    }
    check(art("|32X|a|9").on.text.rows[0]=="|32X|a|9","invalid literal");
    check(art("|123").on.text.rows[0]=="3","exact two digits");
    check(art("|12\x1b[32mX").on.cells[0].attribute==10,"source-order mixed attributes");
    check(art("\x1b[1;39mX").off.cells[0].attribute==15,"default foreground retains intensity");
    check(art("\x1b[1;5;22;25mX").off.cells[0].attribute==7,"intensity/blink reset");
    check(art(std::string(80,'x')+"\r\nY").charged_extent==2,"CRLF delayed wrap");
    check(art(std::string(81,'x')).charged_extent==2,"glyph delayed wrap");
    check(art(std::string(79,'x')+"\tY").off.cells[80].glyph=='Y',"tab delayed wrap");
    check(art("\x1b[?7l"+std::string(82,'x')).charged_extent==1,"virtual wrap disable");
    check(art("\x1b[1000;1Hx").charged_extent==1000,"exact row cap");
    check(!art("\x1b[201;1Hx\x1b[2Jx",200),"high water no clear refund");
    for(const auto& src:std::vector<std::string>{"\x1b[1001;1Hx","\x1b[1;81Hx","\x1b]0;title\a","\x1bPstuff","\x1b[6n","\x1b[?25l","\x1b[?8h","\x1b[12|31m","\x1b[1;2;3H","\x1b[00000m","\x1b[u","\0"s,"\x1b[","\x1b[38;5;1mX","\x1b[1A","x\x1a"})check(!art(src),"forbidden/malformed CSI/control");
    std::string clears;for(int n=0;n<51;++n)clears+="\x1b[2J";check(!art(clears+"X"),"finite clear work");
    check(!art(std::string(80001,'x')),"global wrap bound");
    check(art("@CLEAR@ @INCLUDE:secret@").on.text.rows[0]=="@CLEAR@ @INCLUDE:secret@","display tokens inert");
    for(const auto& src:std::vector<std::string>{"a\r\n\r\nlast\r",std::string(75,'x')+" word next",std::string(81,'x'),"x\tb","a\n\n"}){
        c=app::validate_message_source({app::MessageSourceFormat::plain,1,src},200);
        check(c && c.off.text.canonical_lf==app::normalize_message_body(src,200).canonical_lf,"plain normalization exact");
    }
    c=app::validate_message_source({app::MessageSourceFormat::plain,1,"|12Hello"},200);
    check(c && c.on.text.canonical_lf=="Hello\n" && c.off.text.canonical_lf=="|12Hello\n","plain policy variants");
    c=art("\x1b[1;5;31mX\x1b[2;3HY");
    for(bool ansi:{false,true}){
        auto p=app::message_canvas_page(c.on,0,2,ansi);check(p && app::valid_terminal_display_program(*p),"safe bounded page");
        sfx::test::DeterministicInteractiveByteStreamFake bytes;app::StreamCallerTerminal term(bytes);
        check(term.present_display({"message.source.test",&*p})==app::TerminalOutcome::completed,"render page");
        check(bytes.output().find("[2;3H")==std::string::npos && bytes.output().find("[2J")==std::string::npos,"no original cursor/clear");
        if(!ansi)check(bytes.output().find('\x1b')==std::string::npos,"ASCII escape-free");
    }
    check(!app::message_canvas_page(c.on,0,101,true) && !app::message_canvas_page(c.on,2,1,true),"page bounds");
    for(const auto& source:std::vector<std::string>{"|12Legacy text\n","|12\n|24 ",std::string(999,'x')+" |12end\n"}){
        const auto body=app::normalize_message_body(source,1000);const auto off=app::message_plain_canvas(body,false),on=app::message_plain_canvas(body,true);
        check(off && on && off->text.canonical_lf==body.canonical_lf && on->text.canonical_lf.find("|12")==std::string::npos,"legacy raw literal vs current pipe rendition");
    }
    sfx::test::DeterministicInteractiveByteStreamFake prefix_stream;prefix_stream.script_non_secret_text("/UL\r\n**\x18\x42" "early");app::StreamCallerTerminal prefix_terminal(prefix_stream);
    const auto command=prefix_terminal.read_visible({"test.upload.command",": ",16,app::EmptyInputPolicy::use_default,{}});
    auto prefix=prefix_terminal.take_binary_input_prefix();
    check(command.value=="/UL" && std::string(prefix.bytes.begin(),prefix.bytes.end())=="**\x18\x42" "early" && !prefix.discard_delimiter_lf,"early protocol moved without flushing or keystroke decoding");
    if(argc==2){
        std::ifstream input(argv[1],std::ios::binary);check(bool(input),"original external xadd1 opens read-only");
        const std::string bytes{std::istreambuf_iterator<char>(input),{}};c=art(bytes);
        check(bytes.size()==3120 && c && c.charged_extent==19 && c.off.operations==1445 && c.off.work==81032,"production parser original xadd1 profile exact");
        check(bool(app::message_canvas_page(c.off,0,19,true)),"original external xadd1 safe generated page");
    }
    std::cout<<"PASS "<<checks<<" message source checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" after "<<checks<<" checks\n";return 1;}}
